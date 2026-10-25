// SPDX-License-Identifier: GPL-2.0-or-later
#include "core/second_son_cheats.h"
#include "core/devtools/layer.h"
#include "core/emulator_settings.h"
#include "imgui/renderer/imgui_core.h"
#include <atomic>
#include <mutex>
#include <imgui.h>
namespace {
std::atomic<bool> cheats_open{false};
std::mutex menu_mutex;
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
        std::scoped_lock lock{menu_mutex};
        SetCheatsLocked(false);
    }
}
} // namespace
void Core::SecondSon::Cheats::DrawMenu() { DrawCheats(); }
namespace Overlay {
void ToggleCheatWindow() {
    std::scoped_lock lock{menu_mutex};
    SetCheatsLocked(!cheats_open.load(std::memory_order_relaxed));
}
bool IsCheatWindowOpen() { return cheats_open.load(std::memory_order_acquire); }
} // namespace Overlay
