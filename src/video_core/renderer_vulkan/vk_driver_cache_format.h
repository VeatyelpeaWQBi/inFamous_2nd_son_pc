// SPDX-FileCopyrightText: Copyright 2026 shadPS4-AYOUB1080p contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstring>
#include <span>
#include "common/types.h"

namespace Vulkan {
constexpr size_t DriverCachePrefix = 16;
inline bool DriverCacheCompatible(std::span<const u8> data, u32 vendor, u32 device, u32 driver,
                                  std::span<const u8, 16> uuid) {
    if (data.size() < DriverCachePrefix + 32 || std::memcmp(data.data(), "SHADVKC2", 8) != 0)
        return false;
    const auto word = [&](size_t offset) {
        u32 value;
        std::memcpy(&value, data.data() + offset, 4);
        return value;
    };
    return word(8) == driver && word(12) == 1 && word(16) >= 32 &&
           word(16) <= data.size() - DriverCachePrefix && word(20) == 1 && word(24) == vendor &&
           word(28) == device && std::memcmp(data.data() + 32, uuid.data(), uuid.size()) == 0;
}
} // namespace Vulkan
