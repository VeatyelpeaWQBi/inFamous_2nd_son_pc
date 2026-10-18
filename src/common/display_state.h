// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "common/types.h"
#include <atomic>
#include <utility>
namespace Common {
class DisplayDimensions {
  public:
    static void Store(std::atomic<u64>& target, u32 width, u32 height) {
        target.store((u64{width} << 32) | height, std::memory_order_relaxed);
    }
    static std::pair<u32, u32> Load(const std::atomic<u64>& source) {
        const auto value = source.load(std::memory_order_relaxed);
        return {static_cast<u32>(value >> 32), static_cast<u32>(value)};
    }
    std::atomic<u64> game{}, output{};
};
inline DisplayDimensions display_dimensions;
} // namespace Common
