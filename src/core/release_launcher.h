// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <filesystem>
#include <optional>
#include <string>

namespace Core::Release {
struct LaunchSelection {
    std::string resolution;
    std::string display;
    int language;
    bool circle_enter;
    bool show_fps;
};
std::optional<LaunchSelection> ShowLaunchMenu(const std::filesystem::path& game,
                                             const std::string& mode,
                                             const std::string& display,
                                             const std::optional<std::string>& override_mode);
} // namespace Core::Release
