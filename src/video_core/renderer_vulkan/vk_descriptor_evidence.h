// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <array>
#include <cstdint>
#include <span>
#include "video_core/renderer_vulkan/vk_descriptor_state.h"

namespace Vulkan {
struct DescriptorEvidence {
    enum class Reason { Hit, Unknown, Uncacheable, Layout, Shape, Payload };
    Reason reason{Reason::Uncacheable};
    bool shape_same{}, layout_only{};
    std::uint32_t segments{}, changed_segments{}, infos{}, changed_infos{};
    std::uint32_t buffers{}, offsets{}, ranges{}, samplers{}, views{}, image_layouts{}, texel_views{};
};

// Diagnostic comparison only: never mutates the binding cache or edits descriptor writes.
inline DescriptorEvidence InspectDescriptors(const DescriptorSnapshot& old,
    vk::PipelineLayout layout, std::uint32_t set,
    std::span<const vk::WriteDescriptorSet> writes) noexcept {
    DescriptorEvidence out;
    if (!CountDescriptorInfos(set, writes, out.infos)) return out;
    out.segments = static_cast<std::uint32_t>(writes.size());
    if (old.num_writes == ~0U) { out.reason=DescriptorEvidence::Reason::Unknown; return out; }
    out.shape_same = old.num_writes == writes.size() && old.num_infos == out.infos;
    for (std::uint32_t w=0; out.shape_same && w<writes.size(); ++w) {
        const auto& a=old.writes[w];const auto& b=writes[w];
        out.shape_same = a.binding==b.dstBinding && a.array_element==b.dstArrayElement &&
                         a.count==b.descriptorCount && a.type==b.descriptorType;
    }
    const bool same_layout=old.layout==layout;
    if (!out.shape_same) {
        out.reason=same_layout ? DescriptorEvidence::Reason::Shape : DescriptorEvidence::Reason::Layout;
        return out;
    }
    std::uint32_t index=0;
    for (const auto& w:writes) {
        bool changed=false;
        for (std::uint32_t i=0; i<w.descriptorCount; ++i,++index) {
            const auto& before=old.infos[index]; DescriptorSnapshot::Info now{};
            switch(w.descriptorType) {
            case vk::DescriptorType::eSampler:
            case vk::DescriptorType::eCombinedImageSampler:
            case vk::DescriptorType::eSampledImage:
            case vk::DescriptorType::eStorageImage:
            case vk::DescriptorType::eInputAttachment: {
                const auto& v=w.pImageInfo[i];
                now={reinterpret_cast<std::uint64_t>(static_cast<VkSampler>(v.sampler)),
                     reinterpret_cast<std::uint64_t>(static_cast<VkImageView>(v.imageView)),
                     static_cast<std::uint64_t>(v.imageLayout)};
                out.samplers+=before.a!=now.a;out.views+=before.b!=now.b;
                out.image_layouts+=before.c!=now.c;break;
            }
            case vk::DescriptorType::eUniformTexelBuffer:
            case vk::DescriptorType::eStorageTexelBuffer:
                now={reinterpret_cast<std::uint64_t>(static_cast<VkBufferView>(w.pTexelBufferView[i])),0,0};
                out.texel_views+=before.a!=now.a;break;
            default: {
                const auto& v=w.pBufferInfo[i];
                now={reinterpret_cast<std::uint64_t>(static_cast<VkBuffer>(v.buffer)),v.offset,v.range};
                out.buffers+=before.a!=now.a;out.offsets+=before.b!=now.b;out.ranges+=before.c!=now.c;
            }
            }
            const bool differs=before!=now;out.changed_infos+=differs;changed|=differs;
        }
        out.changed_segments+=changed;
    }
    out.layout_only=!same_layout && out.changed_infos==0;
    out.reason=!same_layout ? DescriptorEvidence::Reason::Layout :
               out.changed_infos ? DescriptorEvidence::Reason::Payload : DescriptorEvidence::Reason::Hit;
    return out;
}
} // namespace Vulkan
