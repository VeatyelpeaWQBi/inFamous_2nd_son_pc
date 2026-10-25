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
#include "core/second_son_cheats.h"
#include "imgui/renderer/imgui_core.h"
#include "imgui/renderer/imgui_impl_sdl3.h"
namespace {
std::atomic<bool> quit_open{false}, volume_open{false}, show_simple_fps{false};
std::atomic<bool> cheats_open{false};
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
void SetCheatsLocked(bool open) {
    if (cheats_open.load(std::memory_order_relaxed) == open) return;
    if (open) {
        ImGui::Core::AcquireGamepadInputCapture();
    } else {
        ImGui::Core::ReleaseGamepadInputCapture();
    }
    cheats_open.store(open, std::memory_order_release);
}
void DrawCheats() {
    namespace Cheats = Core::SecondSon::Cheats;
    if (!cheats_open.load(std::memory_order_acquire)) return;
    const auto language = EmulatorSettings.GetConsoleLanguage();
    const bool chinese = language == 10 || language == 11;
    const auto text = [chinese](const char* zh, const char* en) { return chinese ? zh : en; };
    bool open = true;
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing,
                            {0.5f, 0.5f});
    ImGui::SetNextWindowFocus();
    if (ImGui::Begin(text("辅助菜单###SecondSonCheats", "Assist menu###SecondSonCheats"), &open,
                     ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoDocking |
                         ImGuiWindowFlags_NoSavedSettings)) {
        ImGui::TextUnformatted(text("Insert：显示 / 隐藏", "Insert: show / hide"));
        ImGui::Separator();
        const auto status = Cheats::GetStatus();
        ImGui::BeginDisabled(status != Cheats::Status::Ready);
        auto options = Cheats::GetOptions();
        if (ImGui::Checkbox(text("能量快速恢复", "Fast energy recovery"), &options.recovery)) {
            Cheats::SetRecovery(options.recovery);
        }
        ImGui::TextDisabled("%s", text("每秒恢复 5% 能量；每 10 秒恢复一枚炸弹槽",
                                      "+5% energy per second; +1 bomb slot every 10 seconds"));
        if (ImGui::Checkbox(text("快速积攒业力（大招能量）", "Fast karma streak (super move)"),
                            &options.karma)) {
            Cheats::SetKarma(options.karma);
        }
        ImGui::TextDisabled("%s", text("每次有效业力行动增加三分之一；三次充满",
                                      "Each qualifying karma action adds 1/3; three fill the meter"));
        if (ImGui::Button(text("快速积攒业力经验", "Cycle karma XP multiplier"))) {
            Cheats::CycleExperience();
        }
        ImGui::SameLine();
        options = Cheats::GetOptions();
        if (options.experience == 1) {
            ImGui::TextUnformatted(text("关闭", "Off"));
        } else {
            ImGui::Text("%ux", options.experience);
        }
        ImGui::TextDisabled("%s", text("点击循环：5 倍 / 10 倍 / 关闭",
                                      "Click to cycle: 5x -> 10x -> off"));
        ImGui::EndDisabled();
        if (status == Cheats::Status::Unsupported) {
            ImGui::TextWrapped("%s", text("当前游戏构建没有匹配的作弊补丁。",
                                          "No matching cheat patches for this game build."));
        } else if (status == Cheats::Status::Rejected) {
            ImGui::TextWrapped("%s", text("补丁预检未通过，作弊功能已禁用。",
                                          "Patch preflight failed. Cheats are disabled."));
        } else if (status == Cheats::Status::Pending) {
            ImGui::TextUnformatted(text("等待游戏程序加载。", "Waiting for the game program."));
        }
    }
    ImGui::End();
    if (!open) {
        std::scoped_lock lock{quit_mutex};
        SetCheatsLocked(false);
    }
}
} // namespace
bool Core::GameUI::Layer::ShouldKeepDrawing() {
    return quit_open.load(std::memory_order_acquire) || volume_open.load(std::memory_order_acquire) ||
           cheats_open.load(std::memory_order_acquire);
}
void Core::GameUI::Layer::DrawNullGpuNotice() { ImGui::TextUnformatted("GPU output is disabled."); }
void Core::GameUI::Layer::Draw() {
    using namespace ImGui;
    DrawCheats();
    if (show_simple_fps.load(std::memory_order_relaxed)) {
        const auto* viewport = GetMainViewport();
        SetNextWindowPos({viewport->WorkPos.x + viewport->WorkSize.x - 10,
                          viewport->WorkPos.y + 10},
                         ImGuiCond_Always, {1, 0});
        if (Begin("Video Info", nullptr,
                  ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoInputs |
                      ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                      ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoSavedSettings)) {
            const double frame_rate = ImGui::Sdl::GetFrameRate();
            ImVec4 color{1, 1, 1, 1};
            if (frame_rate < 10.0) {
                color = {1, 0, 0, 1};
            } else if (frame_rate < 20.0) {
                color = {1, 0.5f, 0, 1};
            }
            PushStyleColor(ImGuiCol_Text, color);
            if (frame_rate > 0.0) {
                Text("%.0f FPS (%.1f ms)", frame_rate, 1000.0 / frame_rate);
            } else {
                TextUnformatted("-- FPS (-- ms)");
            }
            PopStyleColor();
        }
        End();
    }
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
void ToggleCheatWindow() {
    std::scoped_lock lock{quit_mutex};
    SetCheatsLocked(!cheats_open.load(std::memory_order_relaxed));
}
bool IsCheatWindowOpen() { return cheats_open.load(std::memory_order_acquire); }
void ToggleSimpleFps() {
    bool current = show_simple_fps.load(std::memory_order_relaxed);
    while (!show_simple_fps.compare_exchange_weak(current, !current,
                                                std::memory_order_relaxed)) {
    }
}
void SetSimpleFps(bool enabled) {
    show_simple_fps.store(enabled, std::memory_order_relaxed);
}
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
