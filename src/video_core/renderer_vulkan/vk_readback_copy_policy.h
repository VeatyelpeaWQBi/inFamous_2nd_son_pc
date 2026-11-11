// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <optional>
#include "common/types.h"
#include "video_core/renderer_vulkan/vk_common.h"

namespace Vulkan {

// The source is only read by the copy, so earlier readers do not conflict.
// The caller retains the pending scope and its full dependency for later consumers.
inline std::optional<vk::MemoryBarrier2> NarrowReadbackCopyBarrier(
    const vk::MemoryBarrier2& pending, vk::PipelineStageFlags2 write_stages,
    bool source_conflict, bool destination_conflict, bool pending_images) {
    using Stage = vk::PipelineStageFlagBits2;
    using Access = vk::AccessFlagBits2;
    if (!source_conflict || destination_conflict || pending_images ||
        pending.srcStageMask != (Stage::eComputeShader | Stage::eAllGraphics) ||
        pending.srcAccessMask != Access::eShaderWrite || write_stages != Stage::eComputeShader) {
        return std::nullopt;
    }
    return vk::MemoryBarrier2{
        .srcStageMask = Stage::eComputeShader,
        .srcAccessMask = Access::eShaderWrite,
        .dstStageMask = Stage::eCopy,
        .dstAccessMask = Access::eTransferRead,
    };
}

struct ReadbackCopyPolicyStatus {
    bool enabled;
    u64 narrowed_copies;
};

ReadbackCopyPolicyStatus GetReadbackCopyPolicyStatus() noexcept;

} // namespace Vulkan
