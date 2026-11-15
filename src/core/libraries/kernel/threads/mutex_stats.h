// SPDX-FileCopyrightText: Copyright 2026 shadPS4 contributors
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <string>
#include <vector>

#include "common/types.h"

namespace Libraries::Kernel {

/// Diagnostic, off by default (`IFSS_GUEST_MUTEX_STATS=1`): every guest pthread mutex records how
/// long threads waited for it and how long it was held. Independent of the fast-path default.
/// Durations fall into buckets below 1, 4, 16, 64, 256, 1024 and 4096 us, and one from there up.
constexpr size_t GuestMutexBuckets = 8;

struct GuestMutexHistogram {
    std::array<u64, GuestMutexBuckets> count{};
    std::array<u64, GuestMutexBuckets> ns{};

    [[nodiscard]] u64 TotalNs() const noexcept {
        u64 total = 0;
        for (const u64 value : ns) {
            total += value;
        }
        return total;
    }
};

struct GuestMutexCounts {
    /// Blocking and timed locks that succeeded; each one is also in a wait bucket.
    u64 locks{};
    u64 try_locks{};
    u64 try_failures{};
    GuestMutexHistogram wait;
    GuestMutexHistogram hold;
};

struct GuestMutexEntry {
    std::string name;
    u64 address{};
    u32 type{};
    GuestMutexCounts all;
    /// The part of `all` done by the guest main thread.
    GuestMutexCounts main;
};

struct GuestMutexReport {
    bool stats{};
    bool fast_path{};
    u32 fast_path_spins{};
    u64 created{};
    u64 destroyed{};
    /// Every mutex since process start, destroyed ones included.
    GuestMutexCounts all;
    GuestMutexCounts main;
    /// Live mutexes with the longest total wait.
    std::vector<GuestMutexEntry> top;
};

/// Marks the calling thread as the guest main thread for the `main` counts.
void MarkGuestMainThread() noexcept;

GuestMutexReport GetGuestMutexReport(size_t top_count);

} // namespace Libraries::Kernel
