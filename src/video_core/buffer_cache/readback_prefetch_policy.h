// SPDX-FileCopyrightText: Copyright 2026 inFamous_2nd_son_pc contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstdint>
#include <limits>
#include <optional>
#include <utility>

namespace VideoCore {

/// Scheduling hint only, never evidence that data is clean. Covers every page of an observed
/// small CPU read. Unknown bounds retain demand readback. Does not change its download window.
inline std::optional<std::pair<std::uint64_t, std::uint64_t>> DemandReadbackPages(
    std::uint64_t address, std::uint64_t size, std::uint64_t arena_start,
    std::uint64_t arena_size, std::uint64_t page_size) {
    constexpr auto max = std::numeric_limits<std::uint64_t>::max();
    if (page_size == 0 || (page_size & (page_size - 1)) != 0 || size == 0 ||
        size > page_size || arena_size == 0 || arena_size > max - arena_start ||
        address < arena_start || address >= arena_start + arena_size ||
        size > arena_start + arena_size - address) {
        return std::nullopt;
    }
    const auto end = address + size;
    if (end > max - (page_size - 1)) {
        return std::nullopt;
    }
    const auto first = address & ~(page_size - 1);
    const auto last = (end + page_size - 1) & ~(page_size - 1);
    if (first < arena_start || last > arena_start + arena_size) {
        return std::nullopt;
    }
    return std::pair{first, last};
}

} // namespace VideoCore
