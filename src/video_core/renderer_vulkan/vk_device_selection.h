// SPDX-FileCopyrightText: Copyright 2026 inFamous_2nd_son_pc contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace Vulkan {

enum class DeviceKind { Other, Integrated, Discrete, Virtual, Cpu };

struct DeviceCandidate {
    bool compatible{};
    DeviceKind kind{};
    std::uint64_t local_memory{};
    std::array<std::uint8_t, 16> uuid{};
};

// Legacy gpu_id values describe an enumeration position, not a device identity. They must
// never participate in this policy. The returned position is used only with the same list
// of live Vulkan handles that supplied these candidates; it is never persisted.
inline constexpr std::optional<std::size_t> SelectPhysicalDevice(
    std::span<const DeviceCandidate> candidates) {
    std::optional<std::size_t> selected;
    const auto rank = [](DeviceKind kind) {
        return kind == DeviceKind::Discrete ? 2 : kind == DeviceKind::Cpu ? 0 : 1;
    };
    for (std::size_t i = 0; i < candidates.size(); ++i) {
        const auto& candidate = candidates[i];
        if (!candidate.compatible) {
            continue;
        }
        if (!selected) {
            selected = i;
            continue;
        }
        const auto& current = candidates[*selected];
        if (rank(candidate.kind) != rank(current.kind)) {
            if (rank(candidate.kind) > rank(current.kind)) {
                selected = i;
            }
        } else if (candidate.local_memory != current.local_memory) {
            if (candidate.local_memory > current.local_memory) {
                selected = i;
            }
        } else if (candidate.uuid < current.uuid) {
            // Identical models can have identical memory sizes. A device UUID is independent
            // of loader ordering and driver cache versions, unlike pipelineCacheUUID.
            selected = i;
        }
    }
    return selected;
}

} // namespace Vulkan
