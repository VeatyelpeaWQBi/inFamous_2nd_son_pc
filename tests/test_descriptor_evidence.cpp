// SPDX-License-Identifier: GPL-2.0-or-later
// Diagnostic interpretation tests, no Vulkan device/loader/game.
#include <array>
#include <cstdio>
#include <cstdlib>
#include <type_traits>
#include "video_core/renderer_vulkan/vk_descriptor_evidence.h"

static void Require(bool ok, const char* message) {
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", message); std::exit(1); }
}
template <typename T> T Handle(std::uintptr_t value) {
    if constexpr (std::is_pointer_v<T>)return reinterpret_cast<T>(value);
    else return static_cast<T>(value);
}
int main() {
    using namespace Vulkan;
    using Reason=DescriptorEvidence::Reason;
    const vk::PipelineLayout layout{Handle<VkPipelineLayout>(1)}, other{Handle<VkPipelineLayout>(2)};
    std::array<vk::DescriptorBufferInfo,2> info{{
        {.buffer=vk::Buffer{Handle<VkBuffer>(3)},.offset=4,.range=32},
        {.buffer=vk::Buffer{Handle<VkBuffer>(3)},.offset=8,.range=64}}};
    std::array<vk::WriteDescriptorSet,1> writes{{
        {.dstBinding=2,.descriptorCount=2,.descriptorType=vk::DescriptorType::eStorageBuffer,
         .pBufferInfo=info.data()}}};
    DescriptorSnapshot old{};
    Require(InspectDescriptors(old,layout,0,writes).reason==Reason::Unknown, "unknown cache is not a hit");
    old.layout=layout;old.num_writes=1;old.num_infos=2;
    old.writes[0]={2,0,2,vk::DescriptorType::eStorageBuffer};
    old.infos[0]={3,4,32};old.infos[1]={3,8,64};
    Require(InspectDescriptors(old,layout,0,writes).reason==Reason::Hit, "identical buffer snapshot");
    auto e=InspectDescriptors(old,other,0,writes);
    Require(e.reason==Reason::Layout && e.layout_only && e.changed_infos==0, "layout-only miss separated");
    info[0].offset++;info[1].range++;
    e=InspectDescriptors(old,layout,0,writes);
    Require(e.reason==Reason::Payload && e.changed_infos==2 && e.changed_segments==1 &&
            e.offsets==1 && e.ranges==1 && e.buffers==0, "array changes count one write segment");
    info[0].buffer=vk::Buffer{Handle<VkBuffer>(9)};
    Require(InspectDescriptors(old,layout,0,writes).buffers==1, "buffer identity differences separated");
    Require(old.infos[0].a==3 && old.infos[0].b==4 && old.num_infos==2, "diagnostics never mutate cache");
    writes[0].dstBinding=3;
    Require(InspectDescriptors(old,layout,0,writes).reason==Reason::Shape, "shape change cannot compare payload");
    writes[0].dstBinding=2;writes[0].pNext=&old;
    Require(InspectDescriptors(old,layout,0,writes).reason==Reason::Uncacheable, "pNext remains conservative");
    writes[0].pNext=nullptr;writes[0].pBufferInfo=nullptr;
    Require(InspectDescriptors(old,layout,0,writes).reason==Reason::Uncacheable, "missing info rejected");
    writes[0].pBufferInfo=info.data();writes[0].descriptorCount=129;
    Require(InspectDescriptors(old,layout,0,writes).reason==Reason::Uncacheable, "bounded comparison");
    writes[0].descriptorCount=1;
    vk::DescriptorImageInfo image{.sampler=vk::Sampler{Handle<VkSampler>(7)},
        .imageView=vk::ImageView{Handle<VkImageView>(8)},.imageLayout=vk::ImageLayout::eGeneral};
    writes[0].descriptorType=vk::DescriptorType::eCombinedImageSampler;writes[0].pImageInfo=&image;
    old.num_infos=1;old.writes[0]={2,0,1,vk::DescriptorType::eCombinedImageSampler};
    old.infos[0]={7,8,static_cast<std::uint64_t>(vk::ImageLayout::eGeneral)};
    Require(InspectDescriptors(old,layout,0,writes).reason==Reason::Hit, "image snapshot matches");
    image.sampler=vk::Sampler{Handle<VkSampler>(9)};image.imageView=vk::ImageView{Handle<VkImageView>(10)};
    image.imageLayout=vk::ImageLayout::eShaderReadOnlyOptimal;
    e=InspectDescriptors(old,layout,0,writes);
    Require(e.changed_infos==1 && e.samplers==1 && e.views==1 && e.image_layouts==1,
            "one info may change several fields, do not count as three misses");
    vk::BufferView view{Handle<VkBufferView>(11)};
    writes[0].descriptorType=vk::DescriptorType::eStorageTexelBuffer;writes[0].pTexelBufferView=&view;
    old.writes[0].type=vk::DescriptorType::eStorageTexelBuffer;old.infos[0]={12,0,0};
    Require(InspectDescriptors(old,layout,0,writes).texel_views==1, "texel view difference");
    Require(InspectDescriptors(old,layout,1,writes).reason==Reason::Uncacheable, "other sets are not compared");
}
