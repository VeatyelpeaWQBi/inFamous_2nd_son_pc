// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
namespace Core::Release {
std::filesystem::path ProjectRoot();
std::filesystem::path Prepare(const std::optional<std::filesystem::path>& game,
                              const std::optional<std::string>& mode);
void Apply(std::string_view serial, std::string_view version);
} // namespace Core::Release
