// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string_view>

namespace Core::SecondSon::Cheats {
inline constexpr std::size_t TrampolineSize = 4096;
enum class Status { Unsupported, Pending, Ready, Rejected };
struct Options {
    bool recovery{}, karma{};
    unsigned experience{1};
};
void Configure(std::string_view title, std::string_view version, std::string_view sha256,
               const std::filesystem::path& matched_profile);
void Install(std::uintptr_t base, std::size_t image_size, void* arena, std::size_t arena_size);
Status GetStatus();
Options GetOptions();
void SetRecovery(bool enabled);
void SetKarma(bool enabled);
void CycleExperience();
} // namespace Core::SecondSon::Cheats
