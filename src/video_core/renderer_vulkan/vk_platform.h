// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <fmt/format.h>

#include "common/assert.h"
#include "common/logging/log.h"
#include "common/string_literal.h"
#include "common/types.h"
#include "video_core/renderer_vulkan/vk_common.h"

namespace Frontend {
enum class WindowSystemType : u8;
class WindowSDL;
} // namespace Frontend

namespace Vulkan {

constexpr u32 TargetVulkanApiVersion = VK_API_VERSION_1_3;

vk::SurfaceKHR CreateSurface(vk::Instance instance, const Frontend::WindowSDL& emu_window);

vk::UniqueInstance CreateInstance(Frontend::WindowSystemType window_type, bool enable_validation,
                                  bool enable_crash_diagnostic);

template <StringLiteral msg = ""> static void Check(vk::Result r) {
    if constexpr (msg.len <= 1) {
        ASSERT_MSG(r == vk::Result::eSuccess, "vk::Result={}", vk::to_string(r));
    } else {
        ASSERT_MSG(r == vk::Result::eSuccess, "Failed to {}: vk::Result={}", msg.value,
                   vk::to_string(r));
    }
}

template <StringLiteral msg = "", typename T> static T Check(vk::ResultValue<T> r) {
    if constexpr (msg.len <= 1) {
        ASSERT_MSG(r.result == vk::Result::eSuccess, "vk::Result={}", vk::to_string(r.result));
    } else {
        ASSERT_MSG(r.result == vk::Result::eSuccess, "Failed to {}: vk::Result={}", msg.value,
                   vk::to_string(r.result));
    }
    return std::move(r.value);
}

} // namespace Vulkan
