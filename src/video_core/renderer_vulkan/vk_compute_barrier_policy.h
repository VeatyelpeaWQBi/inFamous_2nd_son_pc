// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "video_core/renderer_vulkan/vk_common.h"

namespace Vulkan {
// A narrower destination does NOT establish visibility for future graphics/transfer users.
// The caller must retain BOTH source masks and the complete buffer access scope until a
// normal all-command barrier is emitted. Narrowing never consumes that outstanding scope.
inline bool CanNarrowComputeBarrier(const vk::MemoryBarrier2& pending, bool has_images) {
    return !has_images && pending.srcStageMask == vk::PipelineStageFlagBits2::eComputeShader &&
           !(pending.srcAccessMask & ~(vk::AccessFlagBits2::eShaderWrite));
}

inline vk::MemoryBarrier2 ComputeBarrier(const vk::MemoryBarrier2& pending) {
    auto barrier = pending;
    barrier.dstStageMask = vk::PipelineStageFlagBits2::eComputeShader;
    barrier.dstAccessMask = vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eShaderWrite |
                            vk::AccessFlagBits2::eUniformRead;
    return barrier;
}
} // namespace Vulkan
