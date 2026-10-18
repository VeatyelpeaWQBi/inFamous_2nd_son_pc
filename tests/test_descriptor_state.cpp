// SPDX-License-Identifier: GPL-2.0-or-later
// Descriptor state transition/equivalence tests. No Vulkan device, loader, game or disk writes.
#include <array>
#include <cstdio>
#include <cstdlib>
#include <type_traits>
#include "video_core/renderer_vulkan/vk_descriptor_state.h"

using namespace Vulkan;
using Mode = DescriptorUpdate::Mode;
static void Require(bool ok, const char *message) {
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        std::exit(1);
    }
}
template <typename T> T Handle(std::uintptr_t value) {
    if constexpr (std::is_pointer_v<T>)
        return reinterpret_cast<T>(value);
    else
        return static_cast<T>(value);
}
static vk::PipelineLayout Layout(std::uintptr_t n) {
    return vk::PipelineLayout{Handle<VkPipelineLayout>(n)};
}
static vk::Buffer Buffer(std::uintptr_t n) { return vk::Buffer{Handle<VkBuffer>(n)}; }

struct Buffers {
    std::array<vk::DescriptorBufferInfo, 4> info{};
    std::array<vk::WriteDescriptorSet, 4> writes{};
    Buffers() {
        for (std::uint32_t n = 0; n < writes.size(); ++n) {
            info[n] = {.buffer = Buffer(100 + n), .offset = n * 64, .range = 32};
            writes[n] = {.dstBinding = n,
                         .descriptorCount = 1,
                         .descriptorType = vk::DescriptorType::eStorageBuffer,
                         .pBufferInfo = &info[n]};
        }
    }
    DescriptorUpdate Update(DescriptorSnapshot &cache, bool candidate = true, bool skip = true,
                            vk::PipelineLayout layout = Layout(1)) {
        return UpdateDescriptorState(cache, layout, 0, writes, skip, candidate);
    }
};

static void Transitions() {
    Buffers b;
    DescriptorSnapshot cache{};
    Require(b.Update(cache).mode == Mode::Full, "unknown state must push all writes");
    Require(b.Update(cache).mode == Mode::Skip, "unchanged state retains existing whole-call skip");
    b.info[2].offset += 16;
    Require(cache.infos[2].b != b.info[2].offset,
            "shadow must own values independently of source pointers");
    auto update = b.Update(cache);
    Require(update.mode == Mode::Partial && update.changed_writes == 4 &&
                update.EmittedWrites(4) == 1,
            "one changed offset selects exactly its whole write");
    Require(b.Update(cache).mode == Mode::Skip,
            "partial updates maintain a complete current shadow");
    b.info[0].range++;
    b.info[3].buffer = Buffer(600);
    update = b.Update(cache);
    Require(update.mode == Mode::Partial && update.changed_writes == 9,
            "nonadjacent changes keep original order");
    for (auto &info : b.info)
        info.offset++;
    Require(b.Update(cache).mode == Mode::Full, "all changed writes avoid subset gathering");
    Require(b.Update(cache).mode == Mode::Skip, "all-changed full path also advances the shadow");
    Require(b.Update(cache, true, true, Layout(2)).mode == Mode::Full,
            "new layout requires full initialization");
    Require(b.Update(cache, true, true, Layout(1)).mode == Mode::Full,
            "returning to an older layout is also full");
    b.writes[3].dstBinding = 7;
    Require(b.Update(cache).mode == Mode::Full,
            "new binding shape cannot inherit a partial shadow");
    cache.num_writes = ~0U; // New command buffer / regular binding / direct command access.
    Require(b.Update(cache).mode == Mode::Full,
            "invalidation forces full push even if payload unchanged");
    Require(b.Update(cache, true, false).mode == Mode::Full,
            "disabled skip mode never omits writes");
    b.info[1].offset++;
    Require(b.Update(cache, false).mode == Mode::Full, "V2 control never emits a partial update");
    b.info[2].offset++;
    Require(b.Update(cache).mode == Mode::Partial,
            "safe policy toggle uses the maintained full shadow");

    DescriptorSnapshot graphics{}, compute{};
    b.Update(graphics);
    b.Update(compute);
    b.info[1].offset++;
    Require(b.Update(compute).mode == Mode::Partial && b.Update(graphics).mode == Mode::Partial,
            "graphics and compute state must not overwrite each other");
}

static void ConservativeFallbacks() {
    Buffers b;
    DescriptorSnapshot cache{};
    b.Update(cache);
    auto invalid = b.writes;
    invalid[0].pNext = &cache;
    Require(UpdateDescriptorState(cache, Layout(1), 0, invalid, true, true).mode == Mode::Full &&
                cache.num_writes == ~0U,
            "extension writes invalidate the shadow");
    Require(b.Update(cache).mode == Mode::Full,
            "next ordinary push after extension must initialize all");
    invalid = b.writes;
    invalid[0].descriptorType = vk::DescriptorType::eInlineUniformBlock;
    Require(UpdateDescriptorState(cache, Layout(1), 0, invalid, true, true).mode == Mode::Full &&
                cache.num_writes == ~0U,
            "untracked descriptor type is never read as buffer info");
    b.Update(cache);
    invalid = b.writes;
    invalid[0].pBufferInfo = nullptr;
    Require(UpdateDescriptorState(cache, Layout(1), 0, invalid, true, true).mode == Mode::Full &&
                cache.num_writes == ~0U,
            "missing payload cannot be cached");
    b.Update(cache);
    Require(UpdateDescriptorState(cache, Layout(1), 1, b.writes, true, true).mode == Mode::Full &&
                cache.num_writes == ~0U,
            "a different set conservatively disturbs set zero");
    Require(b.Update(cache).mode == Mode::Full, "set zero must recover using full push");
    b.writes[0].descriptorType = vk::DescriptorType::eUniformBuffer;
    Require(b.Update(cache).mode == Mode::Full, "descriptor type change is a shape change");
    b.writes[0].dstArrayElement = 1;
    b.Update(cache);
    b.info[1].offset++;
    Require(b.Update(cache).mode == Mode::Full, "nonzero array element uses full fallback");
    b.writes[0].dstArrayElement = 0;
    b.writes[1].dstBinding = 0;
    b.Update(cache);
    b.info[0].offset++;
    Require(b.Update(cache).mode == Mode::Full, "duplicate bindings cannot be partially reordered");
    b.writes[1].dstBinding = 1;
    b.writes[0].dstBinding = 8;
    b.Update(cache);
    b.info[3].offset++;
    Require(b.Update(cache).mode == Mode::Full, "nonincreasing bindings use full fallback");
    b.writes[0].dstBinding = 0;
    b.writes[0].descriptorType = vk::DescriptorType::eUniformBufferDynamic;
    b.Update(cache);
    b.info[1].offset++;
    Require(b.Update(cache).mode == Mode::Full,
            "dynamic descriptors are not partial push candidates");

    std::array<vk::DescriptorBufferInfo, DescriptorSnapshot::MaxInfos + 1> infos{};
    for (auto &info : infos)
        info = {.buffer = Buffer(1), .offset = 0, .range = 32};
    std::array<vk::WriteDescriptorSet, 1> array{
        {{.dstBinding = 0,
          .descriptorCount = DescriptorSnapshot::MaxInfos,
          .descriptorType = vk::DescriptorType::eStorageBuffer,
          .pBufferInfo = infos.data()}}};
    Require(UpdateDescriptorState(cache, Layout(1), 0, array, true, true).mode == Mode::Full,
            "bounded descriptor array initially uses full push");
    infos.back().offset++;
    infos[127].offset++;
    Require(UpdateDescriptorState(cache, Layout(1), 0, array, true, true).mode == Mode::Full,
            "descriptor array payload changes retain full fallback");
    Require(UpdateDescriptorState(cache, Layout(1), 0, array, true, true).mode == Mode::Skip,
            "unchanged arrays still permit existing whole-call skip");
    array[0].descriptorCount++;
    Require(UpdateDescriptorState(cache, Layout(1), 0, array, true, true).mode == Mode::Full &&
                cache.num_writes == ~0U,
            "oversized info arrays cannot overflow the shadow");
    array[0].descriptorCount = ~0U;
    Require(UpdateDescriptorState(cache, Layout(1), 0, array, true, true).mode == Mode::Full &&
                cache.num_writes == ~0U,
            "count validation must not overflow before rejecting");
}

static void ImagesAndTexels() {
    Buffers b;
    vk::DescriptorImageInfo image{.sampler = vk::Sampler{Handle<VkSampler>(500)},
                                  .imageView = vk::ImageView{Handle<VkImageView>(600)},
                                  .imageLayout = vk::ImageLayout::eGeneral};
    b.writes[0].descriptorType = vk::DescriptorType::eCombinedImageSampler;
    b.writes[0].pImageInfo = &image;
    DescriptorSnapshot cache{};
    b.Update(cache);
    image.imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal;
    Require(b.Update(cache).changed_writes == 1, "image layout change must be emitted");
    image.sampler = vk::Sampler{Handle<VkSampler>(501)};
    Require(b.Update(cache).changed_writes == 1, "sampler identity change must be emitted");
    image.imageView = vk::ImageView{Handle<VkImageView>(601)};
    Require(b.Update(cache).changed_writes == 1, "image view identity change must be emitted");
    vk::BufferView texel{Handle<VkBufferView>(700)};
    b.writes[0].descriptorType = vk::DescriptorType::eStorageTexelBuffer;
    b.writes[0].pTexelBufferView = &texel;
    b.Update(cache);
    texel = vk::BufferView{Handle<VkBufferView>(701)};
    Require(b.Update(cache).changed_writes == 1, "texel view identity change must be emitted");
}

static void MaskBoundary() {
    DescriptorSnapshot cache{};
    vk::DescriptorBufferInfo info{.buffer = Buffer(1), .offset = 0, .range = 32};
    std::array<vk::WriteDescriptorSet, DescriptorSnapshot::MaxWrites + 1> writes{};
    for (std::uint32_t n = 0; n < writes.size(); ++n) {
        writes[n] = {.dstBinding = n,
                     .descriptorCount = 1,
                     .descriptorType = vk::DescriptorType::eStorageBuffer,
                     .pBufferInfo = &info};
    }
    auto bounded = std::span{writes}.first(DescriptorSnapshot::MaxWrites);
    UpdateDescriptorState(cache, Layout(1), 0, bounded, true, true);
    vk::DescriptorBufferInfo other = info;
    other.offset = 64;
    writes[63].pBufferInfo = &other;
    auto update = UpdateDescriptorState(cache, Layout(1), 0, bounded, true, true);
    Require(update.mode == Mode::Partial && update.changed_writes == (std::uint64_t{1} << 63),
            "the 64th write must not wrap a 32-bit mask or shift by 64");
    Require(UpdateDescriptorState(cache, Layout(1), 0, writes, true, true).mode == Mode::Full &&
                cache.num_writes == ~0U,
            "65 writes require full fallback");
    Require(UpdateDescriptorState(cache, Layout(1), 0, bounded, true, true).mode == Mode::Full,
            "overflow fallback cannot leave a partial shadow known");
}

static void SequenceEquivalence() {
    Buffers b;
    DescriptorSnapshot candidate{}, control{};
    std::array<DescriptorSnapshot::Info, 4> actual{}, expected{};
    std::uint32_t random = 0x12345678;
    auto next = [&] {
        random ^= random << 13;
        random ^= random >> 17;
        random ^= random << 5;
        return random;
    };
    for (std::uint32_t step = 0; step < 4096; ++step) {
        const auto layout = Layout(1 + (step / 127) % 2);
        if (step % 127 == 0 || step % 83 == 0) {
            // Simulate a new command buffer / external regular bind with undefined state.
            actual = {};
            expected = {};
            candidate.num_writes = control.num_writes = ~0U;
        }
        for (std::uint32_t i = 0, changes = next() % 5; i < changes; ++i) {
            auto &info = b.info[next() % b.info.size()];
            switch (next() % 3) {
            case 0:
                info.offset = next() % 1024;
                break;
            case 1:
                info.range = 16 + next() % 256;
                break;
            default:
                info.buffer = Buffer(1 + next() % 32);
                break;
            }
        }
        const bool skip = step % 19 != 0;
        const bool enabled = step % 31 != 0;
        auto a = b.Update(candidate, enabled, skip, layout);
        auto c = b.Update(control, false, skip, layout);
        Require(c.mode != Mode::Partial, "control path can only emit full updates or skip");
        for (std::uint32_t n = 0; n < b.writes.size(); ++n) {
            if (c.mode == Mode::Full)
                expected[n] = DescriptorInfoAt(b.writes[n], 0);
            if (a.mode == Mode::Full ||
                (a.mode == Mode::Partial && (a.changed_writes & (std::uint64_t{1} << n)))) {
                actual[n] = DescriptorInfoAt(b.writes[n], 0);
            }
            Require(candidate.infos[n] == DescriptorInfoAt(b.writes[n], 0),
                    "complete shadow follows every emitted state");
        }
        Require(
            actual == expected,
            "candidate and V2 control must produce identical descriptor state after every command");
    }
}

int main() {
    Transitions();
    ConservativeFallbacks();
    ImagesAndTexels();
    MaskBoundary();
    SequenceEquivalence();
    std::puts("A1 descriptor state and 4096-transition equivalence checks passed.");
}
