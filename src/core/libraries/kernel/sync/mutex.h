// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <atomic>
#include <chrono>

#include "common/types.h"

#ifdef _WIN64
#include <windows.h>
#else
#include <mutex>
#endif

namespace Libraries::Kernel {

/// Windows default: a guest mutex takes and releases an uncontended lock in user mode and only
/// enters the kernel to sleep or to wake a sleeper. Set IFSS_GUEST_MUTEX_FAST=0 once per process
/// to restore one kernel mutex object per guest mutex (baseline for A/B).
bool GuestMutexFastPathEnabled() noexcept;
/// Attempts in user mode before a contended fast path lock sleeps (IFSS_GUEST_MUTEX_SPINS).
u32 GuestMutexFastPathSpins() noexcept;

class TimedMutex {
public:
    TimedMutex();
    ~TimedMutex();

    void lock();
    bool try_lock();

    void unlock();

    template <class Rep, class Period>
    bool try_lock_for(const std::chrono::duration<Rep, Period>& rel_time) {
#ifdef _WIN64
        constexpr auto zero = std::chrono::duration<Rep, Period>::zero();
        const auto now = std::chrono::steady_clock::now();

        std::chrono::steady_clock::time_point abs_time = now;
        if (rel_time > zero) {
            constexpr auto max = (std::chrono::steady_clock::time_point::max)();
            if (abs_time < max - rel_time) {
                abs_time += rel_time;
            } else {
                abs_time = max;
            }
        }

        return try_lock_until(abs_time);
#else
        return mtx.try_lock_for(rel_time);
#endif
    }

    template <class Clock, class Duration>
    bool try_lock_until(const std::chrono::time_point<Clock, Duration>& abs_time) {
#ifdef _WIN64
        if (fast) {
            if (TryLockFast()) {
                return true;
            }
            for (;;) {
                const auto now = Clock::now();
                if (abs_time <= now) {
                    return false;
                }
                if (state.exchange(Contended, std::memory_order_acquire) == Unlocked) {
                    return true;
                }
                const auto rel_ms = std::chrono::ceil<std::chrono::milliseconds>(abs_time - now);
                constexpr s64 MaxFiniteWait = static_cast<s64>(INFINITE) - 1;
                const DWORD timeout_ms = static_cast<DWORD>(
                    rel_ms.count() < MaxFiniteWait ? rel_ms.count() : MaxFiniteWait);
                if (WaitForSingleObjectEx(mtx, timeout_ms, true) == WAIT_TIMEOUT) {
                    return false;
                }
            }
        }
        for (;;) {
            const auto now = Clock::now();
            if (abs_time <= now) {
                return false;
            }

            const auto rel_ms = std::chrono::ceil<std::chrono::milliseconds>(abs_time - now);
            u64 res = WaitForSingleObjectEx(mtx, static_cast<u64>(rel_ms.count()), true);
            if (res == WAIT_OBJECT_0) {
                return true;
            } else if (res == WAIT_TIMEOUT) {
                return false;
            }
        }
#else
        return mtx.try_lock_until(abs_time);
#endif
    }

private:
#ifdef _WIN64
    // Fast path states. Contended: a thread may be asleep on the event, so unlock signals it.
    static constexpr u32 Unlocked = 0;
    static constexpr u32 Locked = 1;
    static constexpr u32 Contended = 2;

    bool TryLockFast() noexcept {
        u32 expected = Unlocked;
        return state.load(std::memory_order_relaxed) == Unlocked &&
               state.compare_exchange_strong(expected, Locked, std::memory_order_acquire,
                                             std::memory_order_relaxed);
    }
    void LockContended();

    /// Kernel mutex by default; auto-reset event of the fast path.
    HANDLE mtx;
    std::atomic<u32> state{Unlocked};
    bool fast{};
#else
    std::timed_mutex mtx;
#endif
};

} // namespace Libraries::Kernel
