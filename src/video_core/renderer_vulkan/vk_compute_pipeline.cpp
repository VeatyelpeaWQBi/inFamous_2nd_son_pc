// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <boost/container/small_vector.hpp>

#include <array>

#include "common/elf_info.h"
#include "common/perf_stats.h"
#include "shader_recompiler/info.h"
#include "video_core/host_shaders/infamous_particle_sort_comp.h"
#include "video_core/renderer_vulkan/vk_compute_pipeline.h"
#include "video_core/renderer_vulkan/vk_driver_cache.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"

namespace Vulkan {

namespace {
bool IsExactInfamousParticleSort(const Shader::Info& info, u64 uniform_mask) {
    if (Common::ElfInfo::Instance().GameSerial() != "CUSA00309" ||
        Common::ElfInfo::Instance().AppVer() != "01.07" || info.pgm_hash != 0xa16fa93a ||
        info.hw_stage != Shader::HwStage::Compute || info.sw_stage != Shader::SwStage::Compute ||
        info.buffers.size() != 5 || !info.images.empty() || !info.samplers.empty() ||
        !info.fmasks.empty() || info.uses_dma || info.shared_memory_scratch_size != 0 ||
        uniform_mask != 0) {
        return false;
    }
    static constexpr std::array expected_writes{false, true, false, true, true};
    for (u32 i = 0; i < expected_writes.size(); ++i) {
        if (info.buffers[i].IsSpecial() || info.buffers[i].is_written != expected_writes[i]) {
            return false;
        }
    }
    return true;
}
} // namespace

ComputePipeline::ComputePipeline(const Instance& instance, Scheduler& scheduler,
                                 DescriptorHeap& desc_heap, const Shader::Profile& profile,
                                 vk::PipelineCache pipeline_cache, ComputePipelineKey compute_key_,
                                 const Shader::Info& info_, vk::ShaderModule module_,
                                 SerializationSupport& sdata, u64 uniform_mask,
                                 bool preloading /*=false*/,
                                 bool defer_compile /*=false*/,
                                 std::span<const u32> cached_image_bindings /*={}*/)
    : Pipeline{instance, scheduler, desc_heap, profile, pipeline_cache, true},
      restored{preloading}, compute_key{compute_key_}, module{module_},
      vk_pipeline_cache{pipeline_cache} {
    auto& info = stages[int(Shader::SwStage::Compute)];
    info = &info_;
    uniform_buffer_masks[int(Shader::SwStage::Compute)] = uniform_mask;
    const auto debug_str = GetDebugString();
    if (Common::Perf::GetTuning().v3_particle_sort.load(std::memory_order_relaxed)) {
        if (IsExactInfamousParticleSort(*info, uniform_mask)) {
            replacement_module = CompileSPV(INFAMOUS_PARTICLE_SORT_COMP, instance.GetDevice());
            module = replacement_module;
            SetObjectName(instance.GetDevice(), module, "V3 Infamous particle sort");
            LOG_INFO(Render_Vulkan, "V3 guarded particle sort enabled for {:#x}, preloading={}",
                     info->pgm_hash, preloading);
        } else if (info->pgm_hash == 0xa16fa93a) {
            LOG_WARNING(Render_Vulkan,
                        "V3 particle sort skipped: title, version or resource contract mismatch");
        }
    }

    u32 binding{};
    boost::container::small_vector<vk::DescriptorSetLayoutBinding, 32> bindings;
    u32 buffer_index{};
    for (const auto& buffer : info->buffers) {
        // During deserialization, we don't have access to the UD to fetch sharp data. To address
        // this properly we need to track shaprs or portion of them in `sdata`, but since we're
        // interested only in "is storage" flag (which is not even effective atm), we can take a
        // shortcut there.
        const auto sharp = preloading ? AmdGpu::Buffer{} : buffer.GetSharp(*info);
        bindings.push_back({
            .binding = binding++,
            .descriptorType = (uniform_mask >> buffer_index++) & 1
                                  ? vk::DescriptorType::eUniformBuffer
                                  : vk::DescriptorType::eStorageBuffer,
            .descriptorCount = 1,
            .stageFlags = vk::ShaderStageFlagBits::eCompute,
        });
    }
    ASSERT(!preloading || cached_image_bindings.size() == info->images.size());
    for (size_t image_index = 0; image_index < info->images.size(); ++image_index) {
        const auto& image = info->images[image_index];
        // A restored permutation may have a different mip count from the program's first
        // flattened table. Its own specialization owns the exact layout count.
        const u32 num_bindings =
            preloading ? cached_image_bindings[image_index] : image.NumBindings(*info);
        bindings.push_back({
            .binding = binding,
            .descriptorType = image.is_written ? vk::DescriptorType::eStorageImage
                                               : vk::DescriptorType::eSampledImage,
            .descriptorCount = num_bindings,
            .stageFlags = vk::ShaderStageFlagBits::eCompute,
        });
        binding += num_bindings;
    }
    for (const auto& sampler : info->samplers) {
        bindings.push_back({
            .binding = binding++,
            .descriptorType = vk::DescriptorType::eSampler,
            .descriptorCount = 1,
            .stageFlags = vk::ShaderStageFlagBits::eCompute,
        });
    }

    const vk::PushConstantRange push_constants = {
        .stageFlags = vk::ShaderStageFlagBits::eCompute,
        .offset = 0,
        .size = sizeof(Shader::PushData),
    };

    uses_push_descriptors = binding < instance.MaxPushDescriptors();
    const auto flags = uses_push_descriptors
                           ? vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR
                           : vk::DescriptorSetLayoutCreateFlagBits{};
    const vk::DescriptorSetLayoutCreateInfo desc_layout_ci = {
        .flags = flags,
        .bindingCount = static_cast<u32>(bindings.size()),
        .pBindings = bindings.data(),
    };
    const auto device = instance.GetDevice();
    auto [descriptor_set_result, descriptor_set] =
        device.createDescriptorSetLayoutUnique(desc_layout_ci);
    ASSERT_MSG(descriptor_set_result == vk::Result::eSuccess,
               "Failed to create compute descriptor set layout: {}",
               vk::to_string(descriptor_set_result));
    desc_layout = std::move(descriptor_set);

    const vk::DescriptorSetLayout set_layout = *desc_layout;
    const vk::PipelineLayoutCreateInfo layout_info = {
        .setLayoutCount = 1U,
        .pSetLayouts = &set_layout,
        .pushConstantRangeCount = 1U,
        .pPushConstantRanges = &push_constants,
    };
    auto [layout_result, layout] = instance.GetDevice().createPipelineLayoutUnique(layout_info);
    ASSERT_MSG(layout_result == vk::Result::eSuccess,
               "Failed to create compute pipeline layout: {}", vk::to_string(layout_result));
    pipeline_layout = std::move(layout);
    SetObjectName(device, *pipeline_layout, "Compute PipelineLayout {}", debug_str);

    // Everything that reads the guest state is done above, on the command processor thread.
    // Creating the pipeline object only needs the module and the layout, so it can be deferred
    // to a worker thread (it takes the driver up to a few hundred milliseconds).
    if (!defer_compile) {
        Compile();
    }
}

ComputePipeline::~ComputePipeline() {
    if (replacement_module) {
        instance.GetDevice().destroyShaderModule(replacement_module);
    }
}

void ComputePipeline::NoteFirstUse() {
    if (!first_use_recorded) {
        first_use_recorded = true;
        Common::Perf::Add(restored ? Common::Perf::ComputeWarmFirstUses
                                  : Common::Perf::ComputeColdFirstUses);
    }
}

void ComputePipeline::Compile() {
    const u64 start_ns = Common::Perf::NowNs();
    const vk::PipelineShaderStageRequiredSubgroupSizeCreateInfo subgroup_size_ci = {
        .requiredSubgroupSize = 64,
    };
    const vk::PipelineShaderStageCreateInfo shader_ci = {
        .pNext = instance.IsSubgroupSize64Supported() ? &subgroup_size_ci : nullptr,
        .stage = vk::ShaderStageFlagBits::eCompute,
        .module = module,
        .pName = "main",
    };
    const vk::ComputePipelineCreateInfo compute_pipeline_ci = {
        .stage = shader_ci,
        .layout = *pipeline_layout,
    };
    const auto device = instance.GetDevice();
    auto access =
        driver_cache ? driver_cache->Acquire() : DriverPipelineCache::Lease{{}, vk_pipeline_cache};
    auto [pipeline_result, pipe] =
        device.createComputePipelineUnique(access.cache, compute_pipeline_ci);
    ASSERT_MSG(pipeline_result == vk::Result::eSuccess, "Failed to create compute pipeline: {}",
               vk::to_string(pipeline_result));
    pipeline = std::move(pipe);
    if (replacement_module) {
        device.destroyShaderModule(replacement_module);
        replacement_module = nullptr;
    }
    SetObjectName(device, *pipeline, "Compute Pipeline {}", GetDebugString());
    Common::Perf::Add(Common::Perf::PipelineCreates);
    const u64 elapsed = Common::Perf::NowNs() - start_ns;
    Common::Perf::Add(Common::Perf::PipelineCreateNs, elapsed);
    Common::Perf::Add(Common::Perf::ComputeDriverCompiles);
    Common::Perf::Add(Common::Perf::ComputeDriverCompileNs, elapsed);
    if (driver_cache)
        driver_cache->Compiled();
    MarkReady();
}

} // namespace Vulkan
