// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <bitset>
#include <cstdint>

#include "video_core/renderer_vulkan/vk_common.h"

namespace Vulkan {

/// Predicted state of one command buffer, owned by its command-producing thread. This only
/// removes identical bindings; buffer uploads, barriers and draw commands still run normally.
class BufferBindCache {
public:
    static constexpr std::uint32_t Capacity = 32;

    void ForgetVertices() noexcept {
        vertices_known.reset();
        strides_known.reset();
    }

    void ForgetIndex() noexcept {
        index_known = false;
    }

    void Forget() noexcept {
        ForgetVertices();
        ForgetIndex();
    }

    /// Graphics pipelines and vkCmdSetVertexInputEXT can change the effective stride.
    void ForgetStrides() noexcept {
        strides_known.reset();
    }

    bool NoteIndex(vk::Buffer buffer, vk::DeviceSize offset, vk::IndexType type) noexcept {
        if (!buffer) {
            ForgetIndex();
            return false;
        }
        const bool same = index_known && index_buffer == buffer && index_offset == offset &&
                          index_type == type;
        index_buffer = buffer;
        index_offset = offset;
        index_type = type;
        index_known = true;
        return same;
    }

    /// True only if every binding the command would update already has the same state.
    /// Keep the two APIs and absent/explicit sizes distinct rather than assuming equivalence.
    bool NoteVertices(std::uint32_t first, std::uint32_t count, const vk::Buffer* buffers,
                      const vk::DeviceSize* offsets, bool sized_api = false,
                      const vk::DeviceSize* sizes = nullptr,
                      const vk::DeviceSize* strides = nullptr) noexcept {
        if (count == 0 || first >= Capacity || count > Capacity - first || !buffers || !offsets) {
            ForgetVertices();
            return false;
        }
        bool same = true;
        for (std::uint32_t i = 0; i < count; ++i) {
            const auto slot = first + i;
            auto& v = vertices[slot];
            const bool has_size = sized_api && sizes != nullptr;
            same &= vertices_known[slot] && v.buffer == buffers[i] && v.offset == offsets[i] &&
                    v.sized_api == sized_api && v.has_size == has_size &&
                    (!has_size || v.size == sizes[i]) &&
                    (!strides || (strides_known[slot] && v.stride == strides[i]));
            v.buffer = buffers[i];
            v.offset = offsets[i];
            v.sized_api = sized_api;
            v.has_size = has_size;
            if (has_size) {
                v.size = sizes[i];
            }
            if (strides) {
                v.stride = strides[i];
                strides_known.set(slot);
            }
            // A null pStrides leaves the dynamic stride unchanged, including zero strides.
            vertices_known.set(slot);
        }
        return same;
    }

private:
    struct Vertex {
        vk::Buffer buffer{};
        vk::DeviceSize offset{}, size{}, stride{};
        bool sized_api{}, has_size{};
    };
    std::array<Vertex, Capacity> vertices{};
    std::bitset<Capacity> vertices_known{}, strides_known{};
    vk::Buffer index_buffer{};
    vk::DeviceSize index_offset{};
    vk::IndexType index_type{};
    bool index_known{};
};

} // namespace Vulkan
