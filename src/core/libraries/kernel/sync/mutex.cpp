// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "mutex.h"

#include <cstdlib>
#include <mutex>
#include <vector>

#include "common/assert.h"

namespace Libraries::Kernel {

namespace {

bool UseFastPath() noexcept {
#ifdef _WIN64
    // Default on. IFSS_GUEST_MUTEX_FAST=0 keeps one kernel mutex per guest mutex.
    static const bool enabled = [] {
        const char* value = std::getenv("IFSS_GUEST_MUTEX_FAST");
        return !(value && value[0] == '0' && value[1] == '\0');
    }();
    return enabled;
#else
    return false;
#endif
}

#ifdef _WIN64
/// Events of destroyed fast-path mutexes are reused, never closed. A guest may destroy a mutex
/// as soon as it is released, while the unlocking thread is still about to signal the event.
/// That signal hits a pooled event and is at most one spurious wakeup.
class EventPool {
public:
    HANDLE Acquire() {
        {
            std::scoped_lock lock{mutex};
            if (!free.empty()) {
                const HANDLE event = free.back();
                free.pop_back();
                return event;
            }
        }
        return CreateEvent(nullptr, FALSE, FALSE, nullptr);
    }

    void Release(HANDLE event) {
        std::scoped_lock lock{mutex};
        free.push_back(event);
    }

private:
    std::mutex mutex;
    std::vector<HANDLE> free;
};

EventPool& Events() {
    // Leaked: a guest thread may still unlock while the process is exiting.
    static EventPool* const pool = new EventPool;
    return *pool;
}
#endif

} // namespace

TimedMutex::TimedMutex() {
#ifdef _WIN64
    fast = UseFastPath();
    mtx = fast ? Events().Acquire() : CreateMutex(nullptr, false, nullptr);
    ASSERT(mtx);
#endif
}

TimedMutex::~TimedMutex() {
#ifdef _WIN64
    if (fast) {
        Events().Release(mtx);
    } else {
        CloseHandle(mtx);
    }
#endif
}

void TimedMutex::lock() {
#ifdef _WIN64
    if (fast) {
        if (!TryLockFast()) {
            LockContended();
        }
        return;
    }
    for (;;) {
        u64 res = WaitForSingleObjectEx(mtx, INFINITE, true);
        if (res == WAIT_OBJECT_0) {
            return;
        }
    }
#else
    mtx.lock();
#endif
}

#ifdef _WIN64
void TimedMutex::LockContended() {
    // Short user-mode spins before sleeping. 64 is the value measured with the fast path on.
    for (u32 spins = 64; spins != 0; --spins) {
        if (TryLockFast()) {
            return;
        }
        YieldProcessor();
    }
    // The thread that wins here keeps Contended: it cannot know whether another thread is still
    // asleep. A pooled event can also wake a waiter without a matching release, so every wakeup
    // tries again. The wait stays alertable.
    while (state.exchange(Contended, std::memory_order_acquire) != Unlocked) {
        WaitForSingleObjectEx(mtx, INFINITE, true);
    }
}
#endif

bool TimedMutex::try_lock() {
#ifdef _WIN64
    if (fast) {
        return TryLockFast();
    }
    return WaitForSingleObjectEx(mtx, 0, true) == WAIT_OBJECT_0;
#else
    return mtx.try_lock();
#endif
}

void TimedMutex::unlock() {
#ifdef _WIN64
    if (fast) {
        // Nothing of this object may be read after the exchange: the guest can destroy the
        // mutex as soon as it is released.
        const HANDLE event = mtx;
        if (state.exchange(Unlocked, std::memory_order_release) == Contended) {
            SetEvent(event);
        }
        return;
    }
    ReleaseMutex(mtx);
#else
    mtx.unlock();
#endif
}

} // namespace Libraries::Kernel
