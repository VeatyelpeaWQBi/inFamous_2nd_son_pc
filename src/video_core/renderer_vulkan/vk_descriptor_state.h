// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <array>
#include <bit>
#include <cstdint>
#include <span>
#include "video_core/renderer_vulkan/vk_common.h"

namespace Vulkan {
struct DescriptorSnapshot {
    static constexpr std::uint32_t MaxWrites = 64, MaxInfos = 128;
    struct Write {
        std::uint32_t binding, array_element, count;
        vk::DescriptorType type;
    };
    struct Info {
        std::uint64_t a, b, c;
        bool operator==(const Info &) const = default;
    };
    vk::PipelineLayout layout{};
    std::uint32_t num_writes{~0U}, num_infos{};
    bool partial_eligible{};
    std::array<Write, MaxWrites> writes;
    std::array<Info, MaxInfos> infos;
};

// Full is the conservative default. A bit selects a whole write, never part of an array.
struct DescriptorUpdate {
    enum class Mode { Full, Skip, Partial };
    Mode mode{Mode::Full};
    std::uint64_t changed_writes{};

    std::uint32_t EmittedWrites(std::uint32_t input) const noexcept {
        return mode == Mode::Skip ? 0 : mode == Mode::Full ? input : std::popcount(changed_writes);
    }
};

inline bool CountDescriptorInfos(std::uint32_t set, std::span<const vk::WriteDescriptorSet> writes,
                                 std::uint32_t &count) noexcept {
    count = 0;
    if (set || writes.size() > DescriptorSnapshot::MaxWrites)
        return false;
    for (const auto &w : writes) {
        if (w.pNext || w.descriptorCount > DescriptorSnapshot::MaxInfos - count)
            return false;
        switch (w.descriptorType) {
        case vk::DescriptorType::eSampler:
        case vk::DescriptorType::eCombinedImageSampler:
        case vk::DescriptorType::eSampledImage:
        case vk::DescriptorType::eStorageImage:
        case vk::DescriptorType::eInputAttachment:
            if (!w.pImageInfo && w.descriptorCount)
                return false;
            break;
        case vk::DescriptorType::eUniformTexelBuffer:
        case vk::DescriptorType::eStorageTexelBuffer:
            if (!w.pTexelBufferView && w.descriptorCount)
                return false;
            break;
        case vk::DescriptorType::eUniformBuffer:
        case vk::DescriptorType::eStorageBuffer:
        case vk::DescriptorType::eUniformBufferDynamic:
        case vk::DescriptorType::eStorageBufferDynamic:
            if (!w.pBufferInfo && w.descriptorCount)
                return false;
            break;
        default:
            return false;
        }
        count += w.descriptorCount;
    }
    return true;
}

inline DescriptorSnapshot::Info DescriptorInfoAt(const vk::WriteDescriptorSet &w,
                                                 std::uint32_t i) noexcept {
    switch (w.descriptorType) {
    case vk::DescriptorType::eSampler:
    case vk::DescriptorType::eCombinedImageSampler:
    case vk::DescriptorType::eSampledImage:
    case vk::DescriptorType::eStorageImage:
    case vk::DescriptorType::eInputAttachment: {
        const auto &v = w.pImageInfo[i];
        return {reinterpret_cast<std::uint64_t>(static_cast<VkSampler>(v.sampler)),
                reinterpret_cast<std::uint64_t>(static_cast<VkImageView>(v.imageView)),
                static_cast<std::uint64_t>(v.imageLayout)};
    }
    case vk::DescriptorType::eUniformTexelBuffer:
    case vk::DescriptorType::eStorageTexelBuffer:
        return {reinterpret_cast<std::uint64_t>(static_cast<VkBufferView>(w.pTexelBufferView[i])),
                0, 0};
    default: {
        const auto &v = w.pBufferInfo[i];
        return {reinterpret_cast<std::uint64_t>(static_cast<VkBuffer>(v.buffer)), v.offset,
                v.range};
    }
    }
}

// Called in command order by the single scheduler owner. The shadow owns values, not pointers.
// Binding invalidation (new command buffer, direct access, regular sets) sets num_writes=~0U.
inline DescriptorUpdate UpdateDescriptorState(DescriptorSnapshot &cache, vk::PipelineLayout layout,
                                              std::uint32_t set,
                                              std::span<const vk::WriteDescriptorSet> writes,
                                              bool skip_redundant, bool differential) noexcept {
    std::uint32_t total = 0;
    if (!layout || !CountDescriptorInfos(set, writes, total)) {
        cache.num_writes = ~0U;
        return {};
    }
    bool same_shape =
        cache.layout == layout && cache.num_writes == writes.size() && cache.num_infos == total;
    for (std::uint32_t n = 0; same_shape && n < writes.size(); ++n) {
        const auto &a = cache.writes[n];
        const auto &b = writes[n];
        same_shape = a.binding == b.dstBinding && a.array_element == b.dstArrayElement &&
                     a.count == b.descriptorCount && a.type == b.descriptorType;
    }
    if (differential && skip_redundant && same_shape && cache.partial_eligible) {
        std::uint64_t changed = 0;
        // Eligibility guarantees one descriptor per write, unique/increasing bindings and no
        // arrays.
        for (std::uint32_t n = 0; n < writes.size(); ++n) {
            const auto now = DescriptorInfoAt(writes[n], 0);
            if (cache.infos[n] != now) {
                changed |= std::uint64_t{1} << n;
                cache.infos[n] = now;
            }
        }
        if (!changed)
            return {DescriptorUpdate::Mode::Skip, 0};
        if (static_cast<std::uint32_t>(std::popcount(changed)) < writes.size()) {
            return {DescriptorUpdate::Mode::Partial, changed};
        }
        return {}; // Every write changed: keep the original array without gathering a subset.
    }
    bool same = same_shape;
    std::uint32_t info = 0;
    for (const auto &w : writes) {
        if (!same)
            break;
        for (std::uint32_t i = 0; i < w.descriptorCount; ++i) {
            if (cache.infos[info++] != DescriptorInfoAt(w, i)) {
                same = false;
                break;
            }
        }
    }
    if (same)
        return {skip_redundant ? DescriptorUpdate::Mode::Skip : DescriptorUpdate::Mode::Full, 0};

    cache.layout = layout;
    cache.num_writes = static_cast<std::uint32_t>(writes.size());
    cache.num_infos = total;
    cache.partial_eligible = !writes.empty();
    info = 0;
    for (std::uint32_t n = 0; n < writes.size(); ++n) {
        const auto &w = writes[n];
        cache.writes[n] = {w.dstBinding, w.dstArrayElement, w.descriptorCount, w.descriptorType};
        cache.partial_eligible &= w.descriptorCount == 1 && w.dstArrayElement == 0 &&
                                  w.descriptorType != vk::DescriptorType::eUniformBufferDynamic &&
                                  w.descriptorType != vk::DescriptorType::eStorageBufferDynamic &&
                                  (n == 0 || writes[n - 1].dstBinding < w.dstBinding);
        for (std::uint32_t i = 0; i < w.descriptorCount; ++i) {
            cache.infos[info++] = DescriptorInfoAt(w, i);
        }
    }
    return {};
}
} // namespace Vulkan
