// SPDX-FileCopyrightText: Copyright 2026 shadPS4-AYOUB1080p contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <algorithm>
#include <cstdint>

namespace Vulkan {

struct PrewarmBudget {
    static constexpr uint64_t MiB = 1024ULL * 1024;
    static constexpr uint64_t GiB = 1024ULL * MiB;
    uint64_t host_bytes{};
    uint64_t host_reserve{};
    uint64_t device_reserve{};
    uint64_t driver_bytes{};
    uint64_t staging_keep_bytes{};
    uint64_t staging_warm_bytes{};

    static PrewarmBudget FromAvailable(uint64_t host, uint64_t device) {
        PrewarmBudget result;
        // Leave up to 8 GiB of currently available RAM and a quarter of the GPU
        // budget for the game's live allocations. These are reserves, not allocations.
        result.host_reserve = std::min(host, std::clamp(host / 4, 512 * MiB, 8 * GiB));
        result.device_reserve =
            device == 0 ? 0 : std::min(device, std::clamp(device / 4, 256 * MiB, 4 * GiB));
        const auto usable = host - result.host_reserve;
        result.host_bytes = std::min(8 * GiB, usable / 4 * 3 + usable % 4 * 3 / 4);
        result.driver_bytes = std::min(GiB, result.host_bytes / 4);
        result.staging_keep_bytes = std::min(2 * GiB, result.host_bytes / 4);
        result.staging_warm_bytes = std::min(256 * MiB, result.staging_keep_bytes / 4);
        return result;
    }

    bool CanContinue(uint64_t host_available, uint64_t device_available,
                     uint64_t estimated_used) const {
        return estimated_used < host_bytes && host_available > host_reserve &&
               device_available > device_reserve;
    }
};
} // namespace Vulkan
