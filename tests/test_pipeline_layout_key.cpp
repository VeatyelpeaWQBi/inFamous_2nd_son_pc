// SPDX-License-Identifier: GPL-2.0-or-later
// Semantic layout identity only: no device, framework, Vulkan loader or game.
#include <array>
#include <cstdio>
#include <cstdlib>
#include <unordered_map>
#include "video_core/renderer_vulkan/vk_pipeline_layout_cache.h"

#if !defined(VK_NO_PROTOTYPES) || VULKAN_HPP_DISPATCH_LOADER_DYNAMIC != 1 || \
    !defined(VULKAN_HPP_NO_CONSTRUCTORS) || !defined(VULKAN_HPP_NO_EXCEPTIONS)
#error Layout identity must use the same Vulkan-Hpp configuration as the engine
#endif

static void Require(bool ok, const char* message) {
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", message); std::exit(1); }
}
int main() {
    using Key = Vulkan::PipelineLayoutKey;
    std::array<vk::DescriptorSetLayoutBinding, 2> bindings{{
        {.binding=0, .descriptorType=vk::DescriptorType::eStorageBuffer,
         .descriptorCount=1, .stageFlags=vk::ShaderStageFlagBits::eCompute},
        {.binding=2, .descriptorType=vk::DescriptorType::eSampledImage,
         .descriptorCount=3, .stageFlags=vk::ShaderStageFlagBits::eCompute},
    }};
    const std::array<vk::PushConstantRange, 1> pushes{{
        {.stageFlags=vk::ShaderStageFlagBits::eCompute, .offset=0, .size=32},
    }};
    vk::DescriptorSetLayoutCreateInfo info{
        .flags=vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR,
        .bindingCount=static_cast<std::uint32_t>(bindings.size()), .pBindings=bindings.data(),
    };
    const auto original=Key::Make(info,pushes);
    Require(original.has_value(), "ordinary layout must be cacheable");
    auto owned=*original;
    auto reordered=bindings;
    std::swap(reordered[0],reordered[1]);
    auto equivalent=info;equivalent.pBindings=reordered.data();
    Require(Key::Make(equivalent,pushes)==original, "binding array order is not semantic identity");
    Require(Vulkan::PipelineLayoutKeyHash{}(*Key::Make(equivalent,pushes))==
            Vulkan::PipelineLayoutKeyHash{}(*original), "equivalent layouts must hash equally");
    for (unsigned field=0;field<4;++field) {
        auto changed=bindings;
        if(field==0)changed[0].binding=1;
        if(field==1)changed[0].descriptorType=vk::DescriptorType::eUniformBuffer;
        if(field==2)changed[0].descriptorCount=2;
        if(field==3)changed[0].stageFlags=vk::ShaderStageFlagBits::eFragment;
        auto ci=info;ci.pBindings=changed.data();
        Require(Key::Make(ci,pushes)!=original, "every descriptor identity field must separate layouts");
    }
    for (unsigned field=0;field<3;++field) {
        auto changed=pushes;
        if(field==0)changed[0].stageFlags=vk::ShaderStageFlagBits::eFragment;
        if(field==1)changed[0].offset=4;
        if(field==2)changed[0].size=16;
        Require(Key::Make(info,changed)!=original, "all push constant range fields are semantic");
    }
    auto regular=info;regular.flags={};
    Require(Key::Make(regular,pushes)!=original, "push descriptors cannot alias ordinary sets");
    auto fewer=info;fewer.bindingCount=1;
    Require(Key::Make(fewer,pushes)!=original, "binding count cannot alias");
    Require(Key::Make(info,{})!=original, "push range count cannot alias");
    vk::Sampler sampler;
    auto immutable=bindings;immutable[0].pImmutableSamplers=&sampler;
    auto ci=info;ci.pBindings=immutable.data();
    Require(!Key::Make(ci,pushes), "immutable sampler arrays take private path");
    vk::DescriptorSetLayoutBindingFlagsCreateInfo extension;
    ci=info;ci.pNext=&extension;
    Require(!Key::Make(ci,pushes), "unhandled extension chains take private path");
    ci=info;ci.bindingCount=129;
    Require(!Key::Make(ci,pushes), "oversized layouts take private path before dereference");
    ci=info;ci.pBindings=nullptr;
    Require(!Key::Make(ci,pushes), "null binding storage rejected");
    auto duplicates=bindings;duplicates[1].binding=duplicates[0].binding;
    ci=info;ci.pBindings=duplicates.data();
    Require(!Key::Make(ci,pushes), "duplicate bindings are never interned");
    struct Collision { std::size_t operator()(const Key&) const { return 0; } };
    std::unordered_map<Key,int,Collision> collision;
    collision.emplace(*original,1);collision.emplace(*Key::Make(regular,pushes),2);
    Require(collision.size()==2, "hash collisions must not merge different layouts");
    bindings[0].descriptorCount=7;
    Require(*original==owned, "key owns values independently of source array lifetime");
    std::puts("Pipeline layout identity policies passed.");
}
