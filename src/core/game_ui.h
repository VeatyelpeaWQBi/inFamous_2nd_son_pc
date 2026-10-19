// SPDX-FileCopyrightText: Copyright 2025 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "imgui/imgui_layer.h"
#include <string>
namespace Core::GameUI {
class Layer final : public ImGui::Layer {
  public:
    void Draw() override;
    bool ShouldKeepDrawing() override;
    static void DrawNullGpuNotice();
};
} // namespace Core::GameUI
namespace Overlay {
void ToggleSimpleFps();
void SetSimpleFps(bool enabled);
void ToggleQuitWindow();
void ShowVolume();
void TextCentered(const std::string& text);
} // namespace Overlay
