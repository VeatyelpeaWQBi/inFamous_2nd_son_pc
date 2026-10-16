// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "common/assert.h"
#include "common/logging/formatter.h"
#include "core/emulator_settings.h"
#include "video_core/renderdoc.h"

#include <array>
#include <atomic>
#include <renderdoc_app.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

#include <filesystem>
#include <string>

namespace VideoCore {

enum class CaptureState {
    Idle,
    Triggered,
    InProgress,
};
static std::atomic<CaptureState> capture_state{CaptureState::Idle};
static std::atomic<u32> screenshot_game_only_count{0};
static std::atomic<u32> screenshot_with_overlays_count{0};

RENDERDOC_API_1_6_0* rdoc_api{};

void LoadRenderDoc() {
#ifdef _WIN32
    if (EmulatorSettings.IsRenderdocEnabled() && GetModuleHandleW(L"WarpVizTarget.dll")) {
        LOG_ERROR(Render, "RenderDoc cannot initialize in this Nsight-injected process; "
                          "select one GPU capture backend");
        return;
    }
    HMODULE mod = GetModuleHandleW(L"renderdoc.dll");
    if (!mod && EmulatorSettings.IsRenderdocEnabled()) {
        // RegGetValue uses bytes, terminates strings, expands REG_EXPAND_SZ and closes its key.
        std::array<wchar_t, 32768> key_str{};
        DWORD bytes = static_cast<DWORD>(sizeof(key_str));
        const LSTATUS result = RegGetValueW(
            HKEY_LOCAL_MACHINE, L"SOFTWARE\\Classes\\RenderDoc.RDCCapture.1\\DefaultIcon",
            nullptr, RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ, nullptr, key_str.data(), &bytes);
        if (result != ERROR_SUCCESS) {
            LOG_ERROR(Render, "Cannot locate installed RenderDoc (registry error {})", result);
            return;
        }
        std::wstring icon_path{key_str.data()};
        if (!icon_path.empty() && icon_path.front() == L'"') {
            const auto end = icon_path.find(L'"', 1);
            if (end == std::wstring::npos) {
                LOG_ERROR(Render, "Invalid quoted RenderDoc registry path");
                return;
            }
            icon_path = icon_path.substr(1, end - 1);
        } else if (const auto comma = icon_path.rfind(L',');
                   comma != std::wstring::npos &&
                   icon_path.find_first_not_of(L" -0123456789", comma + 1) == std::wstring::npos) {
            icon_path.resize(comma);
        }
        const std::filesystem::path executable{icon_path};
        if (!executable.is_absolute() || !executable.has_filename()) {
            LOG_ERROR(Render, "Installed RenderDoc registry path is not an absolute file path");
            return;
        }
        const auto library = executable.parent_path() / L"renderdoc.dll";
        mod = LoadLibraryExW(library.c_str(), nullptr,
                             LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        if (!mod) {
            LOG_ERROR(Render, "Cannot load installed RenderDoc DLL (Windows error {})",
                      GetLastError());
            return;
        }
    }

    if (mod) {
        const auto get_api =
            reinterpret_cast<pRENDERDOC_GetAPI>(GetProcAddress(mod, "RENDERDOC_GetAPI"));
        if (!get_api || get_api(eRENDERDOC_API_Version_1_6_0,
                               reinterpret_cast<void**>(&rdoc_api)) != 1 || !rdoc_api) {
            rdoc_api = nullptr;
            LOG_ERROR(Render, "Installed RenderDoc does not provide the requested capture API");
            return;
        }
        LOG_INFO(Render, "RenderDoc API ready (1.6.0)");
    }
#else
#ifdef ANDROID
    static constexpr const char RENDERDOC_LIB[] = "libVkLayer_GLES_RenderDoc.so";
#else
    static constexpr const char RENDERDOC_LIB[] = "librenderdoc.so";
#endif
    // Check if we are running by RDoc GUI
    void* mod = dlopen(RENDERDOC_LIB, RTLD_NOW | RTLD_NOLOAD);
    if (!mod && EmulatorSettings.IsRenderdocEnabled()) {
        // If enabled in config, try to load RDoc runtime in offline mode
        if ((mod = dlopen(RENDERDOC_LIB, RTLD_NOW))) {
            const auto RENDERDOC_GetAPI =
                reinterpret_cast<pRENDERDOC_GetAPI>(dlsym(mod, "RENDERDOC_GetAPI"));
            const s32 ret = RENDERDOC_GetAPI(eRENDERDOC_API_Version_1_6_0, (void**)&rdoc_api);
            ASSERT(ret == 1);
        } else {
            LOG_ERROR(Render, "Cannot load RenderDoc: {}", dlerror());
        }
    }
#endif
    if (rdoc_api) {
        // Disable default capture keys as they suppose to trigger present-to-present capturing
        // and it is not what we want
        rdoc_api->SetCaptureKeys(nullptr, 0);

        // Also remove rdoc crash handler
        rdoc_api->UnloadCrashHandler();
    }
}

void StartCapture() {
    if (!rdoc_api) {
        return;
    }

    auto expected = CaptureState::Triggered;
    if (capture_state.compare_exchange_strong(expected, CaptureState::InProgress)) {
        rdoc_api->StartFrameCapture(nullptr, nullptr);
    }
}

void EndCapture() {
    if (!rdoc_api) {
        return;
    }

    if (capture_state.load() == CaptureState::InProgress) {
        const auto saved = rdoc_api->EndFrameCapture(nullptr, nullptr);
        LOG_INFO(Render, "RenderDoc capture finished: saved={}, count={}",
                 saved, rdoc_api->GetNumCaptures());
        capture_state.store(CaptureState::Idle);
    }
}

void TriggerCapture() {
    if (!rdoc_api) return;
    auto expected = CaptureState::Idle;
    capture_state.compare_exchange_strong(expected, CaptureState::Triggered);
}

void SetOutputDir(const std::filesystem::path& path, const std::string& prefix) {
    if (!rdoc_api) {
        return;
    }
    LOG_WARNING(Common, "RenderDoc capture path: {}", (path / prefix).string());
    rdoc_api->SetCaptureFilePathTemplate(fmt::UTF((path / prefix).u8string()).data.data());
}

bool IsRenderDocLoaded() {
    return rdoc_api != nullptr;
}

void RequestScreenshot(const ScreenshotRequest request) {
    switch (request) {
    case ScreenshotRequest::GameOnly:
        screenshot_game_only_count.fetch_add(1, std::memory_order_relaxed);
        break;
    case ScreenshotRequest::WithOverlays:
        screenshot_with_overlays_count.fetch_add(1, std::memory_order_relaxed);
        break;
    case ScreenshotRequest::None:
    default:
        break;
    }
}

u32 ConsumeGameOnlyScreenshotRequests() {
    return screenshot_game_only_count.exchange(0, std::memory_order_acq_rel);
}

u32 ConsumeWithOverlaysScreenshotRequests() {
    return screenshot_with_overlays_count.exchange(0, std::memory_order_acq_rel);
}

ScreenshotRequests ConsumeScreenshotRequests() {
    return ScreenshotRequests{
        .game_only_count = ConsumeGameOnlyScreenshotRequests(),
        .with_overlays_count = ConsumeWithOverlaysScreenshotRequests(),
    };
}

} // namespace VideoCore
