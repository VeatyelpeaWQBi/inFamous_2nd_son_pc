// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Include the vulkan platform specific header
#if defined(ANDROID)
#define VK_USE_PLATFORM_ANDROID_KHR
#elif defined(_WIN64)
#define VK_USE_PLATFORM_WIN32_KHR
#elif defined(__APPLE__)
#define VK_USE_PLATFORM_METAL_EXT
#else
#define VK_USE_PLATFORM_WAYLAND_KHR
#define VK_USE_PLATFORM_XLIB_KHR
#endif

#include <atomic>
#include <cstring>
#include <filesystem>
#include <fmt/ranges.h>
#include <vector>

#include "common/assert.h"
#include "common/logging/log.h"
#include "common/path_util.h"
#include "core/emulator_settings.h"
#include "sdl_window.h"
#include "video_core/renderer_vulkan/vk_platform.h"

#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif
#ifdef _WIN32
#include <windows.h>
#endif

namespace Vulkan {

#ifdef _WIN32
// Attribute layer-generated calls without requiring matching third-party PDBs.

#endif

vk::SurfaceKHR CreateSurface(vk::Instance instance, const Frontend::WindowSDL& emu_window) {
    const auto& window_info = emu_window.GetWindowInfo();
    vk::SurfaceKHR surface{};

#if defined(VK_USE_PLATFORM_WIN32_KHR)
    if (window_info.type == Frontend::WindowSystemType::Windows) {
        const vk::Win32SurfaceCreateInfoKHR win32_ci = {
            .hinstance = nullptr,
            .hwnd = static_cast<HWND>(window_info.render_surface),
        };

        if (instance.createWin32SurfaceKHR(&win32_ci, nullptr, &surface) != vk::Result::eSuccess) {
            LOG_CRITICAL(Render_Vulkan, "Failed to initialize Win32 surface");
            UNREACHABLE();
        }
    }
#elif defined(VK_USE_PLATFORM_XLIB_KHR) || defined(VK_USE_PLATFORM_WAYLAND_KHR)
    if (window_info.type == Frontend::WindowSystemType::X11) {
        const vk::XlibSurfaceCreateInfoKHR xlib_ci = {
            .dpy = static_cast<Display*>(window_info.display_connection),
            .window = reinterpret_cast<Window>(window_info.render_surface),
        };

        if (instance.createXlibSurfaceKHR(&xlib_ci, nullptr, &surface) != vk::Result::eSuccess) {
            LOG_ERROR(Render_Vulkan, "Failed to initialize Xlib surface");
            UNREACHABLE();
        }
    } else if (window_info.type == Frontend::WindowSystemType::Wayland) {
        if (EmulatorSettings.IsRenderdocEnabled()) {
            LOG_ERROR(Render_Vulkan,
                      "RenderDoc is not compatible with Wayland, use an X11 window instead.");
        }

        const vk::WaylandSurfaceCreateInfoKHR wayland_ci = {
            .display = static_cast<wl_display*>(window_info.display_connection),
            .surface = static_cast<wl_surface*>(window_info.render_surface),
        };

        if (instance.createWaylandSurfaceKHR(&wayland_ci, nullptr, &surface) !=
            vk::Result::eSuccess) {
            LOG_ERROR(Render_Vulkan, "Failed to initialize Wayland surface");
            UNREACHABLE();
        }
    }
#elif defined(VK_USE_PLATFORM_METAL_EXT)
    if (window_info.type == Frontend::WindowSystemType::Metal) {
        const vk::MetalSurfaceCreateInfoEXT macos_ci = {
            .pLayer = static_cast<const CAMetalLayer*>(window_info.render_surface),
        };

        if (instance.createMetalSurfaceEXT(&macos_ci, nullptr, &surface) != vk::Result::eSuccess) {
            LOG_CRITICAL(Render_Vulkan, "Failed to initialize MacOS surface");
            UNREACHABLE();
        }
    }
#endif

    if (!surface) {
        LOG_CRITICAL(Render_Vulkan, "Presentation not supported on this platform");
        UNREACHABLE();
    }

    return surface;
}

std::vector<const char*> GetInstanceExtensions(Frontend::WindowSystemType window_type,
                                               bool enable_debug_utils) {
    const auto [properties_result, properties] = vk::enumerateInstanceExtensionProperties();
    if (properties_result != vk::Result::eSuccess || properties.empty()) {
        LOG_ERROR(Render_Vulkan, "Failed to query extension properties: {}",
                  vk::to_string(properties_result));
        return {};
    }

    // Add the windowing system specific extension
    std::vector<const char*> extensions;
    extensions.reserve(7);

    switch (window_type) {
    case Frontend::WindowSystemType::Headless:
        break;
#if defined(VK_USE_PLATFORM_WIN32_KHR)
    case Frontend::WindowSystemType::Windows:
        extensions.push_back(VK_KHR_WIN32_SURFACE_EXTENSION_NAME);
        break;
#elif defined(VK_USE_PLATFORM_XLIB_KHR) || defined(VK_USE_PLATFORM_WAYLAND_KHR)
    case Frontend::WindowSystemType::X11:
        extensions.push_back(VK_KHR_XLIB_SURFACE_EXTENSION_NAME);
        break;
    case Frontend::WindowSystemType::Wayland:
        extensions.push_back(VK_KHR_WAYLAND_SURFACE_EXTENSION_NAME);
        break;
#elif defined(VK_USE_PLATFORM_METAL_EXT)
    case Frontend::WindowSystemType::Metal:
        extensions.push_back(VK_EXT_METAL_SURFACE_EXTENSION_NAME);
        break;
#endif
    default:
        LOG_ERROR(Render_Vulkan, "Presentation not supported on this platform");
        break;
    }

    if (window_type != Frontend::WindowSystemType::Headless) {
        extensions.push_back(VK_KHR_SURFACE_EXTENSION_NAME);
    }

    if (EmulatorSettings.IsHdrAllowed()) {
        extensions.push_back(VK_EXT_SWAPCHAIN_COLOR_SPACE_EXTENSION_NAME);
    }

    if (enable_debug_utils) {
        extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    }

    // Sanitize extension list
    std::erase_if(extensions, [&](const char* extension) -> bool {
        const auto it =
            std::find_if(properties.begin(), properties.end(), [extension](const auto& prop) {
                return std::strcmp(extension, prop.extensionName) == 0;
            });

        if (it == properties.end()) {
            return true;
        }
        return false;
    });

    return extensions;
}

vk::UniqueInstance CreateInstance(Frontend::WindowSystemType window_type, bool, bool) {
    static vk::detail::DynamicLoader loader;
    const auto get_proc = loader.getProcAddress<PFN_vkGetInstanceProcAddr>("vkGetInstanceProcAddr");
    ASSERT_MSG(get_proc != nullptr, "The system Vulkan loader is unavailable");
    VULKAN_HPP_DEFAULT_DISPATCHER.init(get_proc);
    const auto [result, version] = VULKAN_HPP_DEFAULT_DISPATCHER.vkEnumerateInstanceVersion
                                       ? vk::enumerateInstanceVersion()
                                       : vk::ResultValue(vk::Result::eSuccess, VK_API_VERSION_1_0);
    ASSERT_MSG(result == vk::Result::eSuccess && version >= TargetVulkanApiVersion,
               "Vulkan 1.3 or newer is required");
    const auto extensions = GetInstanceExtensions(window_type, false);
    const vk::ApplicationInfo app = {
        .pApplicationName = "inFAMOUS Second Son",
        .applicationVersion = VK_MAKE_VERSION(1, 0, 0),
        .pEngineName = "inFAMOUS V2",
        .engineVersion = VK_MAKE_VERSION(1, 0, 0),
        .apiVersion = version,
    };
    const vk::InstanceCreateInfo info = {
        .pApplicationInfo = &app,
        .enabledExtensionCount = static_cast<u32>(extensions.size()),
        .ppEnabledExtensionNames = extensions.data(),
    };
    auto [created, instance] = vk::createInstanceUnique(info);
    ASSERT_MSG(created == vk::Result::eSuccess, "Failed to create Vulkan instance: {}",
               vk::to_string(created));
    VULKAN_HPP_DEFAULT_DISPATCHER.init(*instance);
    return std::move(instance);
}

} // namespace Vulkan
