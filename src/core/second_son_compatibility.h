// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <algorithm>
#include <array>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace Core::SecondSon {
// Title IDs listed by shadps4-emu/ps4_cheats/PATCHES/InfamousSecondSon.xml.
// First Light and other PS4 games intentionally have no entry.
inline constexpr std::array<std::string_view, 4> TitleIds{"CUSA00004", "CUSA00223", "CUSA00305",
                                                          "CUSA00309"};

inline bool IsTitle(std::string_view serial) {
    return std::ranges::find(TitleIds, serial) != TitleIds.end();
}
inline bool IsVersion(std::string_view version) {
    return version.size() == 5 && version[2] == '.' &&
           std::ranges::all_of(version.substr(0, 2), [](char c) { return c >= '0' && c <= '9'; }) &&
           std::ranges::all_of(version.substr(3), [](char c) { return c >= '0' && c <= '9'; });
}
inline bool IsSha256(std::string_view value) {
    return value.size() == 64 && std::ranges::all_of(value, [](char c) {
               return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
           });
}
inline bool IsPatchFile(std::string_view name) {
    return name.size() > 4 && name.ends_with(".xml") && name.find("..") == std::string_view::npos &&
           std::ranges::all_of(name, [](char c) {
               return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                      c == '_' || c == '-' || c == '.';
           });
}
struct Build {
    std::vector<std::string> title_ids;
    std::string version;
    std::string sha256;
    std::string patch_file;
    std::string legacy_cache_title;

    bool IsValid() const {
        return !title_ids.empty() &&
               std::ranges::all_of(title_ids, [](const auto& id) { return IsTitle(id); }) &&
               IsVersion(version) && IsSha256(sha256) && IsPatchFile(patch_file) &&
               (legacy_cache_title.empty() ||
                (IsTitle(legacy_cache_title) &&
                 std::ranges::find(title_ids, legacy_cache_title) != title_ids.end()));
    }
    bool Matches(std::string_view serial, std::string_view app_version,
                 std::string_view executable_sha256) const {
        return IsTitle(serial) && version == app_version && sha256 == executable_sha256 &&
               std::ranges::find(title_ids, serial) != title_ids.end();
    }
};
struct Resolution {
    std::string_view key;
    unsigned width, height;
    bool pro;
    std::string_view patch;
};
inline constexpr std::array<Resolution, 6> Resolutions{{
    {"720p", 1280, 720, false, "Native 1280x720 (render and output)"},
    {"900p", 1600, 900, false, "Native 1600x900 (render and output)"},
    {"1080p", 1920, 1080, false, ""},
    {"1440p", 2560, 1440, true, "PS4 Pro mode: native 2560x1440 without checkerboard"},
    {"1800p", 3200, 1800, true, "PS4 Pro mode: native 3200x1800 without checkerboard"},
    {"pro4k", 3840, 2160, true, "PS4 Pro mode: native 3840x2160 without checkerboard"},
}};
inline const Resolution& FindResolution(std::string_view key) {
    for (const auto& resolution : Resolutions) {
        if (resolution.key == key) {
            return resolution;
        }
    }
    throw std::runtime_error("Unsupported resolution: use 720p, 900p, 1080p, 1440p, 1800p or pro4k.");
}
inline std::string_view EffectiveMode(std::string_view requested, bool matched_build) {
    return matched_build ? FindResolution(requested).key : "1080p";
}
} // namespace Core::SecondSon
