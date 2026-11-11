// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

namespace Core::FileSys {
class IFile;
}
namespace Core::SecondSon {
inline constexpr std::size_t ControlsTrampolineSize = 4096;
inline constexpr std::size_t ParticlePollTrampolineSize = 256;
inline constexpr std::size_t FeedbackArenaTrampolineSize = 1024;
inline constexpr std::string_view ControlsFingerprint =
    "2d1ca79630d7bbe6fa29575f59aa7996d43071d74ac8e4f7d40137967d9039ab";

// Fixed-address hooks require this exact program, independently of the XML registry.
// Options allow automatic Xbox activation; PS/unknown devices never force-enable the hooks.
void ConfigureControls(std::string_view title, std::string_view version,
                       std::string_view sha256, bool xbox_prompts, bool touchpad_chords);
// Mounted executable identity, independent of controller family and optional input hooks.
bool IsVerifiedBuild();
void InstallControls(std::uintptr_t base, std::size_t image_size, void* arena,
                     std::size_t arena_size);
void InstallParticlePollPause(std::uintptr_t base, std::size_t image_size, void* arena);
void InstallFeedbackArena(std::uintptr_t base, std::size_t image_size, void* arena);
std::array<std::uint64_t, 2> FeedbackArenaMoves();
struct FeedbackPoolCounts {
    std::array<std::uint64_t, 2> moves{}, overflows{}, busy{}, used{}, resets{};
    std::uint32_t capacity{}, stride{}, group_records{};
};
FeedbackPoolCounts FeedbackArenaPoolCounts();
// Full SHA-256 of a mounted immutable view, preserving its read position.
std::string HashFile(FileSys::IFile& file);
bool XboxPromptsEnabled();
bool TouchpadChordsEnabled();
std::unique_ptr<FileSys::IFile> XboxPromptFile(std::string_view guest_path,
                                              std::unique_ptr<FileSys::IFile> file);
} // namespace Core::SecondSon
