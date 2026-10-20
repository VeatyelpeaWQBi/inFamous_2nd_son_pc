// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace Core::FileSys {
class IFile;
}

namespace Core::Release {
std::filesystem::path ProjectRoot();
std::filesystem::path Prepare(const std::optional<std::filesystem::path>& game,
                              const std::optional<std::string>& mode);
// Called after mounting base/update/mod layers, before the engine loads guest code.
void SelectGame(std::string_view serial, std::string_view version, FileSys::IFile& executable);
// SHA-256 of a mounted file, preserving its read position.
std::string HashFile(FileSys::IFile& file);
void Apply();
} // namespace Core::Release
