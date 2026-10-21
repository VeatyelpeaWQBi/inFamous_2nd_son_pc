// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>

namespace VideoCore {
// Within one existing copy command only. The source/destination byte unions and
// order are identical; no command, synchronization or intermediate access moves.
template <typename Region>
std::size_t CoalesceCopyRegions(std::span<const Region> input, std::span<Region> output) {
    if (output.size() < input.size()) return 0;
    std::size_t count{};
    constexpr auto Max = std::numeric_limits<std::uint64_t>::max();
    for (const auto& region : input) {
        if (count) {
            auto& previous = output[count - 1];
            if (previous.size && region.size &&
                previous.srcOffset <= Max - previous.size &&
                previous.dstOffset <= Max - previous.size &&
                previous.size <= Max - region.size &&
                region.srcOffset <= Max - region.size &&
                region.dstOffset <= Max - region.size &&
                previous.srcOffset + previous.size == region.srcOffset &&
                previous.dstOffset + previous.size == region.dstOffset) {
                previous.size += region.size;
                continue;
            }
        }
        output[count++] = region;
    }
    return count;
}
} // namespace VideoCore
