// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "video_core/renderer_vulkan/vk_common.h"

namespace Vulkan {

// This applies to both a whole-image read and a read of a view within a uniform image.
// The previous readers still need to be included in the next write dependency.
inline bool MergeUniformImageRead(vk::ImageLayout current_layout, vk::AccessFlags2 current_access,
                                  vk::PipelineStageFlags2& reader_stages,
                                  vk::ImageLayout requested_layout,
                                  vk::PipelineStageFlags2 requested_stage,
                                  vk::AccessFlags2 requested_access) {
    if (requested_access != vk::AccessFlagBits2::eShaderRead ||
        current_layout != requested_layout || current_access != requested_access) {
        return false;
    }
    reader_stages |= requested_stage;
    return true;
}

struct ImageReadTransitionStatus {
    bool enabled;
    bool used;
};
ImageReadTransitionStatus GetImageReadTransitionStatus() noexcept;

} // namespace Vulkan
