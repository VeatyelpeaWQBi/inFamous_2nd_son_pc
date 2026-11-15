// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "mutex.h"

#include <algorithm>
#include <cstdlib>
#include <mutex>
#include <vector>

#include "common/assert.h"

namespace Libraries::Kernel {

namespace {

struct FastPathConfig {
    bool enabled{};
    u32 spins{};
};

FastPathConfig ReadFastPathConfig() noexcept {
    FastPathConfig config;
#ifdef _WIN64
    // Default on after the 2026-11-15 capture A/B and manual check. IFSS_GUEST_MUTEX_FAST=0
    // restores the original kernel mutex for an isolated baseline.
    const char* enabled = std::getenv("IFSS_GUEST_MUTEX_FAST");
    config.enabled = !(enabled && enabled[0] == '0' && enabled[1] == '\0');
    if (config.enabled) {
        config.spins = 64;
        if (const char* spins = std::getenv("IFSS_GUEST_MUTEX_SPINS"); spins && *spins) {
            config.spins =
                static_cast<u32>(std::min<unsigned long>(std::strtoul(spins, nullptr, 10), 4096));
        }
    }
#endif
    return config;
}

const FastPathConfig& FastPath() noexcept {
    static const FastPathConfig config = ReadFastPathConfig();
    return config;
}

#ifdef _WIN64
/// Events of destroyed fast path mutexes are reused, never closed. A guest may destroy a mutex
/// right after another thread released it, while that thread is still about to signal the event:
/// the late signal then reaches a pooled event, where it is at most one spurious wakeup.
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
    // Leaked on purpose: guest threads may still unlock during process exit.
    static EventPool* const pool = new EventPool;
    return *pool;
}
#endif

} // namespace

bool GuestMutexFastPathEnabled() noexcept {
    return FastPath().enabled;
}

u32 GuestMutexFastPathSpins() noexcept {
    return FastPath().spins;
}

TimedMutex::TimedMutex() {
#ifdef _WIN64
    fast = FastPath().enabled;
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
    for (u32 spins = FastPath().spins; spins != 0; --spins) {
        if (TryLockFast()) {
            return;
        }
        YieldProcessor();
    }
    // A thread that got the lock here keeps the Contended mark: it cannot know whether another
    // thread still sleeps. The event can also be signaled without a release (pooled event, an
    // earlier timed out waiter), so every wakeup tries again. The wait stays alertable.
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
