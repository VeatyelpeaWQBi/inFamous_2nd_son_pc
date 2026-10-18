// SPDX-License-Identifier: GPL-2.0-or-later
#include "video_core/renderer_vulkan/vk_pipeline_layout_cache.h"
#include <utility>
#include "common/assert.h"
#include "common/perf_stats.h"

namespace Vulkan {
std::shared_ptr<const PipelineLayoutObjects> PipelineLayoutCache::Create(
    const vk::DescriptorSetLayoutCreateInfo& info,
    std::span<const vk::PushConstantRange> pushes) const {
    auto objects = std::make_shared<PipelineLayoutObjects>();
    auto [descriptor_result, descriptor] = device.createDescriptorSetLayoutUnique(info);
    ASSERT_MSG(descriptor_result == vk::Result::eSuccess,
               "Failed to create descriptor set layout: {}", vk::to_string(descriptor_result));
    objects->descriptor = std::move(descriptor);
    const vk::DescriptorSetLayout handle = *objects->descriptor;
    const vk::PipelineLayoutCreateInfo pipeline_info = {
        .setLayoutCount = 1,
        .pSetLayouts = &handle,
        .pushConstantRangeCount = static_cast<std::uint32_t>(pushes.size()),
        .pPushConstantRanges = pushes.data(),
    };
    auto [pipeline_result, pipeline] = device.createPipelineLayoutUnique(pipeline_info);
    ASSERT_MSG(pipeline_result == vk::Result::eSuccess,
               "Failed to create pipeline layout: {}", vk::to_string(pipeline_result));
    objects->pipeline = std::move(pipeline);
    Common::Perf::Add(Common::Perf::V3LayoutsCreated);
    return objects;
}

std::shared_ptr<const PipelineLayoutObjects> PipelineLayoutCache::Acquire(
    const vk::DescriptorSetLayoutCreateInfo& info,
    std::span<const vk::PushConstantRange> pushes) {
    Common::Perf::Add(Common::Perf::V3LayoutRequests);
    if (!enabled) return Create(info, pushes);
    auto key = PipelineLayoutKey::Make(info, pushes);
    if (!key) {
        Common::Perf::Add(Common::Perf::V3LayoutFallbacks);
        return Create(info, pushes);
    }
    std::scoped_lock lock{mutex};
    if (const auto found = layouts.find(*key); found != layouts.end()) {
        Common::Perf::Add(Common::Perf::V3LayoutHits);
        return found->second;
    }
    auto objects = Create(info, pushes);
    if (layouts.size() < Capacity) layouts.emplace(std::move(*key), objects);
    else Common::Perf::Add(Common::Perf::V3LayoutFallbacks);
    return objects;
}
} // namespace Vulkan
