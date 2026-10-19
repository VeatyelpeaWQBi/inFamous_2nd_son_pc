// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <algorithm>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <unordered_map>
#include <utility>
#include <vector>
#include "video_core/renderer_vulkan/vk_common.h"

namespace Vulkan {

// Keys own semantic values, never pointers, padding or host handles to mutable resources.
// Extension chains and immutable samplers deliberately retain private layouts.
struct PipelineLayoutKey {
    struct Binding {
        std::uint32_t binding, type, count, stages;
        bool operator==(const Binding&) const = default;
    };
    struct PushRange {
        std::uint32_t stages, offset, size;
        bool operator==(const PushRange&) const = default;
    };
    std::uint32_t descriptor_flags{};
    std::vector<Binding> bindings;
    std::vector<PushRange> pushes;
    bool operator==(const PipelineLayoutKey&) const = default;

    static std::optional<PipelineLayoutKey> Make(
        const vk::DescriptorSetLayoutCreateInfo& info,
        std::span<const vk::PushConstantRange> pushes) {
        if (info.pNext || info.bindingCount > 128 ||
            (info.bindingCount && !info.pBindings)) return {};
        PipelineLayoutKey key;
        key.descriptor_flags = static_cast<std::uint32_t>(info.flags);
        for (std::uint32_t i = 0; i < info.bindingCount; ++i) {
            const auto& b = info.pBindings[i];
            if (b.pImmutableSamplers) return {};
            key.bindings.push_back({b.binding, static_cast<std::uint32_t>(b.descriptorType),
                                    b.descriptorCount, static_cast<std::uint32_t>(b.stageFlags)});
        }
        std::ranges::sort(key.bindings, {}, &Binding::binding);
        for (std::size_t i = 1; i < key.bindings.size(); ++i) {
            if (key.bindings[i - 1].binding == key.bindings[i].binding) return {};
        }
        for (const auto& p : pushes) {
            key.pushes.push_back({static_cast<std::uint32_t>(p.stageFlags), p.offset, p.size});
        }
        return key;
    }
};

struct PipelineLayoutKeyHash {
    std::size_t operator()(const PipelineLayoutKey& key) const noexcept {
        std::size_t result = key.descriptor_flags;
        const auto add = [&result](std::uint32_t value) {
            result ^= std::size_t{value} + std::size_t{0x9e3779b9U} +
                      (result << 6) + (result >> 2);
        };
        add(static_cast<std::uint32_t>(key.bindings.size()));
        for (const auto& b : key.bindings) { add(b.binding); add(b.type); add(b.count); add(b.stages); }
        add(static_cast<std::uint32_t>(key.pushes.size()));
        for (const auto& p : key.pushes) { add(p.stages); add(p.offset); add(p.size); }
        return result;
    }
};

struct PipelineLayoutObjects {
    // Reverse destruction order destroys the pipeline layout before its set layout.
    vk::UniqueDescriptorSetLayout descriptor;
    vk::UniquePipelineLayout pipeline;
};

// Owned by PipelineCache, one Vulkan device per cache. Immutable policy after startup.
// The mutex also covers concurrent pipeline restoration. No lock on draw/dispatch paths.
class PipelineLayoutCache {
public:
    PipelineLayoutCache(vk::Device device, bool enabled) : device{device}, enabled{enabled} {}
    bool SharingEnabled() const noexcept { return enabled; }
    std::shared_ptr<const PipelineLayoutObjects> Acquire(
        const vk::DescriptorSetLayoutCreateInfo& info,
        std::span<const vk::PushConstantRange> pushes);

private:
    std::shared_ptr<const PipelineLayoutObjects> Create(
        const vk::DescriptorSetLayoutCreateInfo& info,
        std::span<const vk::PushConstantRange> pushes) const;
    static constexpr std::size_t Capacity = 1024;
    vk::Device device;
    const bool enabled;
    std::mutex mutex;
    std::unordered_map<PipelineLayoutKey, std::shared_ptr<const PipelineLayoutObjects>,
                       PipelineLayoutKeyHash> layouts;
};
} // namespace Vulkan
