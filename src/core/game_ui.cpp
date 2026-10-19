// SPDX-FileCopyrightText: Copyright 2025 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
#include "core/game_ui.h"
#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <SDL3/SDL_events.h>
#include <imgui.h>
#include "core/emulator_settings.h"
#include "imgui/renderer/imgui_core.h"
namespace {
std::atomic<bool> quit_open{false}, volume_open{false};
std::atomic<std::int64_t> volume_started{};
std::mutex quit_mutex;
std::int64_t NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}
void SetQuitLocked(bool open) {
    if (quit_open.load(std::memory_order_relaxed) == open) {
        return;
    }
    if (open) {
        ImGui::Core::AcquireGamepadInputCapture();
    } else {
        ImGui::Core::ReleaseGamepadInputCapture();
    }
    quit_open.store(open, std::memory_order_release);
}
void CloseQuit() {
    std::scoped_lock lock{quit_mutex};
    SetQuitLocked(false);
}
} // namespace
bool Core::GameUI::Layer::ShouldKeepDrawing() {
    return quit_open.load(std::memory_order_acquire) || volume_open.load(std::memory_order_acquire);
}
void Core::GameUI::Layer::DrawNullGpuNotice() { ImGui::TextUnformatted("GPU output is disabled."); }
void Core::GameUI::Layer::Draw() {
    using namespace ImGui;
    if (quit_open.load(std::memory_order_acquire)) {
        SetNextWindowPos(GetMainViewport()->GetCenter(), ImGuiCond_Appearing, {0.5f, 0.5f});
        SetNextWindowFocus();
        if (Begin("Quit", nullptr,
                  ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                      ImGuiWindowFlags_NoDocking)) {
            TextUnformatted("Quit inFAMOUS Second Son?");
            TextUnformatted("Enter / A: quit    Esc / B: cancel");
            if (IsKeyPressed(ImGuiKey_Escape, false) ||
                IsKeyPressed(ImGuiKey_GamepadFaceRight, false)) {
                CloseQuit();
            } else if (IsKeyPressed(ImGuiKey_Enter, false) ||
                       IsKeyPressed(ImGuiKey_GamepadFaceDown, false)) {
                SDL_Event event{};
                event.type = SDL_EVENT_QUIT;
                SDL_PushEvent(&event);
            }
        }
        End();
    }
    if (volume_open.load(std::memory_order_acquire)) {
        if (NowMs() - volume_started.load(std::memory_order_relaxed) >= 3000) {
            volume_open.store(false, std::memory_order_release);
        } else {
            SetNextWindowPos({GetMainViewport()->WorkPos.x + GetMainViewport()->WorkSize.x - 10,
                              GetMainViewport()->WorkPos.y + 10},
                             ImGuiCond_Always, {1, 0});
            if (Begin("Volume", nullptr,
                      ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                          ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoDocking)) {
                Text("Volume: %d", EmulatorSettings.GetVolumeSlider());
            }
            End();
        }
    }
}
namespace Overlay {
void ToggleQuitWindow() {
    std::scoped_lock lock{quit_mutex};
    SetQuitLocked(!quit_open.load(std::memory_order_relaxed));
}
void ShowVolume() {
    volume_started.store(NowMs(), std::memory_order_relaxed);
    volume_open.store(true, std::memory_order_release);
}
void TextCentered(const std::string& text) {
    ImGui::SetCursorPosX((ImGui::GetContentRegionAvail().x - ImGui::CalcTextSize(text.c_str()).x) *
                         0.5f);
    ImGui::TextUnformatted(text.c_str());
}
} // namespace Overlay
