// SPDX-License-Identifier: GPL-2.0-or-later
#include <cstdlib>
#include <iostream>
#include "video_core/buffer_access_tracker.h"
#include "video_core/renderer_vulkan/vk_compute_barrier_policy.h"
#include "video_core/renderer_vulkan/vk_image_read_transition.h"
#include "video_core/renderer_vulkan/vk_readback_copy_policy.h"

void Check(bool v) {
    if (!v) {
        std::cerr << "Compute barrier policy failed\n";
        std::exit(1);
    }
}
int main() {
    vk::MemoryBarrier2 pending{
        .srcStageMask = vk::PipelineStageFlagBits2::eComputeShader,
        .srcAccessMask = vk::AccessFlagBits2::eShaderWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eAllCommands,
        .dstAccessMask = vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite,
    };
    VideoCore::BufferAccessTracker<int> tracker;
    tracker.Get(1).write_ranges.Add({16, 64});
    tracker.Get(2).read_ranges.Add({0, 128});
    for (int n = 0; n < 128; ++n) {
        Check(Vulkan::CanNarrowComputeBarrier(pending, false));
        const auto narrow = Vulkan::ComputeBarrier(pending);
        Check(narrow.srcStageMask == pending.srcStageMask &&
              narrow.srcAccessMask == pending.srcAccessMask);
        Check(narrow.dstStageMask == vk::PipelineStageFlagBits2::eComputeShader);
        Check(bool(narrow.dstAccessMask & vk::AccessFlagBits2::eShaderRead));
        Check(bool(narrow.dstAccessMask & vk::AccessFlagBits2::eShaderWrite));
        // A subsequent transfer/vertex consumer must still observe the old hazard.
        Check(tracker.Get(1).write_ranges.Overlaps(32, 48));
        Check(tracker.Get(2).read_ranges.Overlaps(64, 96));
        Check(pending.dstStageMask == vk::PipelineStageFlagBits2::eAllCommands);
    }
    Check(!Vulkan::CanNarrowComputeBarrier(pending, true));
    for (auto stage :
         {vk::PipelineStageFlagBits2::eCopy, vk::PipelineStageFlagBits2::eAllGraphics,
          vk::PipelineStageFlagBits2::eDrawIndirect, vk::PipelineStageFlagBits2::eAllCommands}) {
        auto mixed = pending;
        mixed.srcStageMask |= stage;
        Check(!Vulkan::CanNarrowComputeBarrier(mixed, false));
    }
    auto invalid = pending;
    invalid.srcAccessMask |= vk::AccessFlagBits2::eTransferWrite;
    Check(!Vulkan::CanNarrowComputeBarrier(invalid, false));
    invalid = pending;
    invalid.srcStageMask = vk::PipelineStageFlagBits2::eNone;
    Check(!Vulkan::CanNarrowComputeBarrier(invalid, false));
    invalid = pending;
    invalid.srcAccessMask = vk::AccessFlagBits2::eNone;
    Check(Vulkan::CanNarrowComputeBarrier(invalid, false)); // write-after-read: execution only
    tracker.ResetScope(); // Only an ordinary all-command barrier may retire these records.
    Check(!tracker.Get(1).write_ranges.Overlaps(32, 48));
    using Stage = vk::PipelineStageFlagBits2;
    using Access = vk::AccessFlagBits2;
    const auto shader_writes = Stage::eComputeShader;
    pending.srcStageMask |= Stage::eAllGraphics;
    const auto copy = Vulkan::NarrowReadbackCopyBarrier(pending, shader_writes, true, false, false);
    Check(copy && copy->srcStageMask == shader_writes && copy->srcAccessMask == Access::eShaderWrite &&
          copy->dstStageMask == Stage::eCopy && copy->dstAccessMask == Access::eTransferRead);
    Check(pending.srcStageMask == (shader_writes | Stage::eAllGraphics) &&
          pending.dstStageMask == Stage::eAllCommands);
    Check(!Vulkan::NarrowReadbackCopyBarrier(pending, shader_writes, false, false, false));
    Check(!Vulkan::NarrowReadbackCopyBarrier(pending, shader_writes, true, true, false));
    Check(!Vulkan::NarrowReadbackCopyBarrier(pending, shader_writes, true, false, true));
    Check(!Vulkan::NarrowReadbackCopyBarrier(
        pending, shader_writes | Stage::eAllGraphics, true, false, false));
    pending.srcAccessMask |= Access::eTransferWrite;
    Check(!Vulkan::NarrowReadbackCopyBarrier(pending, shader_writes, true, false, false));
    using Layout = vk::ImageLayout;
    vk::PipelineStageFlags2 readers = Stage::eFragmentShader;
    const auto layout = Layout::eShaderReadOnlyOptimal;
    Check(Vulkan::MergeUniformImageRead(layout, Access::eShaderRead, readers, layout,
                                        Stage::eComputeShader, Access::eShaderRead));
    Check(readers == (Stage::eFragmentShader | Stage::eComputeShader));
    Check(!Vulkan::MergeUniformImageRead(layout, Access::eShaderRead, readers, Layout::eGeneral,
                                         Stage::eCopy, Access::eShaderRead));
    Check(!Vulkan::MergeUniformImageRead(layout, Access::eShaderWrite, readers, layout,
                                         Stage::eCopy, Access::eShaderRead));
    Check(!Vulkan::MergeUniformImageRead(layout, Access::eShaderWrite, readers, layout,
                                         Stage::eCopy, Access::eShaderWrite));
    Check(readers == (Stage::eFragmentShader | Stage::eComputeShader));
    std::cout << "PASS: compute barrier masks, conservative fallback and retained hazards.\n";
}
