// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <cstdio>
#include <cstdlib>
#include <type_traits>

#include "video_core/renderer_vulkan/vk_buffer_bind_cache.h"

// Include the cache first, as a standalone header. Tests must use the engine's Vulkan-Hpp
// configuration, not succeed only because another header happened to set these macros first.
#if !defined(VK_NO_PROTOTYPES) || VULKAN_HPP_DISPATCH_LOADER_DYNAMIC != 1 || \
    !defined(VULKAN_HPP_NO_CONSTRUCTORS) || !defined(VULKAN_HPP_NO_EXCEPTIONS)
#error BufferBindCache must include the project Vulkan configuration before Vulkan-Hpp
#endif
static_assert(std::is_aggregate_v<vk::VertexInputBindingDescription2EXT>);

namespace {
void Require(bool value, const char* message) {
    if (!value) {
        std::fprintf(stderr, "%s\n", message);
        std::exit(1);
    }
}
template <typename T>
T Handle(std::uintptr_t value) {
    if constexpr (std::is_pointer_v<T>) {
        return reinterpret_cast<T>(value);
    } else {
        return static_cast<T>(value);
    }
}
} // namespace

int main() {
    Vulkan::BufferBindCache cache;
    std::array buffers{vk::Buffer{Handle<VkBuffer>(1)}, vk::Buffer{Handle<VkBuffer>(2)}};
    std::array<vk::DeviceSize, 2> offsets{0, 64}, sizes{128, 256}, strides{0, 16};
    const auto legacy = [&] { return cache.NoteVertices(0, 2, buffers.data(), offsets.data()); };
    const auto sized = [&] {
        return cache.NoteVertices(0, 2, buffers.data(), offsets.data(), true, sizes.data(),
                                  strides.data());
    };
    Require(!legacy() && legacy(), "first bind must emit, identical bind may skip");
    offsets[1]++;
    Require(!legacy() && legacy(), "offset mutation must emit; arguments must be copied");
    buffers[1] = buffers[0];
    Require(!legacy(), "buffer handle mutation must emit");
    Require(cache.NoteVertices(1, 1, &buffers[1], &offsets[1]), "partial overlap should match");
    const vk::DeviceSize partial_offset = offsets[1] + 4;
    Require(!cache.NoteVertices(1, 1, &buffers[1], &partial_offset), "partial update must emit");
    Require(!legacy(), "partial change must invalidate equality of the full list");
    Require(!sized() && sized(), "API transition must emit, then sized bindings may skip");
    sizes[0]++;
    Require(!sized(), "size mutation must emit");
    strides[0] = 4;
    Require(!sized(), "stride mutation must emit");
    strides[0] = 0;
    Require(!sized() && sized(), "zero stride is valid state, not unknown state");
    cache.ForgetStrides();
    Require(!sized() && sized(), "external stride state changes must force a bind");
    Require(cache.NoteVertices(0, 2, buffers.data(), offsets.data(), true, sizes.data()),
            "null strides preserve the existing stride");
    Require(sized(), "null stride command must not erase known strides");
    Require(!cache.NoteVertices(0, 2, buffers.data(), offsets.data(), true),
            "absent sizes must not match explicit sizes");
    Require(!sized(), "explicit sizes must not match absent sizes");
    Require(!legacy(), "legacy transition must emit");
    Require(!sized(), "sized transition must emit again");
    cache.ForgetVertices();
    Require(!cache.NoteVertices(31, 1, buffers.data(), offsets.data()), "last slot must bind");
    Require(cache.NoteVertices(31, 1, buffers.data(), offsets.data()), "last slot may match");
    Require(!cache.NoteVertices(31, 2, buffers.data(), offsets.data()), "overflow must fall back");
    Require(!cache.NoteVertices(31, 1, buffers.data(), offsets.data()), "overflow invalidates");
    Require(!cache.NoteVertices(~0U, ~0U, nullptr, nullptr), "integer overflow must fall back");
    Require(!cache.NoteVertices(0, 0, nullptr, nullptr), "zero count must fall back");
    Require(!cache.NoteVertices(0, 1, nullptr, offsets.data()), "null array must fall back");
    Require(!legacy(), "invalid arrays invalidate previous knowledge");
    std::array<vk::Buffer, 32> all_buffers{};
    std::array<vk::DeviceSize, 32> all_offsets{};
    Require(!cache.NoteVertices(0, 32, all_buffers.data(), all_offsets.data()), "full range bind");
    Require(cache.NoteVertices(0, 32, all_buffers.data(), all_offsets.data()),
            "full range, including null handles, may match");
    const auto index = [&](vk::DeviceSize offset, vk::IndexType type) {
        return cache.NoteIndex(buffers[0], offset, type);
    };
    Require(!index(0, vk::IndexType::eUint16) && index(0, vk::IndexType::eUint16),
            "index first bind and repeat");
    Require(!index(2, vk::IndexType::eUint16), "index offset mutation must emit");
    Require(!index(2, vk::IndexType::eUint32), "index type mutation must emit");
    Require(!cache.NoteIndex(vk::Buffer{Handle<VkBuffer>(3)}, 2, vk::IndexType::eUint32),
            "index handle mutation must emit");
    Require(!cache.NoteIndex({}, 0, vk::IndexType::eUint16), "unknown index handle must emit");
    Require(!cache.NoteIndex({}, 0, vk::IndexType::eUint16), "unknown index must never skip");
    cache.Forget();
    Require(!index(0, vk::IndexType::eUint16) && !legacy(),
            "new command buffer/raw access must force all bindings");
    std::puts("Buffer binding cache regression checks passed.");
}
