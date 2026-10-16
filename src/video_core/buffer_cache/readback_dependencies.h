// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <optional>

namespace VideoCore {

/// Reduce window dependencies only when all copied ranges have known, stable writers.
/// QueryTicks still has the memory tracker's 256 KiB granularity: this does not infer exact
/// shader write footprints. Unknown/stale information always keeps the original window waits.
template <typename Copies, typename QueryTicks, typename CurrentSequence>
std::optional<std::array<std::uint64_t, 2>> SelectReadbackDependencies(
    std::uint64_t arena_base, std::uint64_t window_start, std::uint64_t window_end,
    const Copies& copies, const std::array<std::uint64_t, 2>& window_ticks,
    std::uint64_t sequence, std::uint64_t tracking_granularity, QueryTicks&& query_ticks,
    CurrentSequence&& current_sequence) {
    if (tracking_granularity == 0 || window_start >= window_end || copies.empty() ||
        sequence != current_sequence()) {
        return std::nullopt;
    }
    std::array<std::uint64_t, 2> ticks{};
    for (const auto& copy : copies) {
        if (copy.srcOffset > std::numeric_limits<std::uint64_t>::max() - arena_base) {
            return std::nullopt;
        }
        const auto start = arena_base + copy.srcOffset;
        if (start < window_start || start >= window_end || copy.size == 0 ||
            copy.size > window_end - start) {
            return std::nullopt;
        }
        // Check each tracking word: a known writer in one word must not hide missing
        // provenance in another word of the same copy.
        auto address = start;
        auto remaining = copy.size;
        while (remaining != 0) {
            const auto word_remaining = tracking_granularity - address % tracking_granularity;
            const auto size = std::min(remaining, word_remaining);
            const auto writers = query_ticks(address, size);
            if ((writers[0] == 0 && writers[1] == 0) || writers[0] > window_ticks[0] ||
                writers[1] > window_ticks[1]) {
                return std::nullopt;
            }
            for (unsigned q = 0; q < 2; ++q) {
                ticks[q] = std::max(ticks[q], writers[q]);
            }
            address += size;
            remaining -= size;
        }
    }
    if (sequence != current_sequence()) {
        return std::nullopt;
    }
    return ticks;
}

} // namespace VideoCore
