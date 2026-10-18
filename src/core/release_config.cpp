// SPDX-License-Identifier: GPL-2.0-or-later
#include "core/release_config.h"
#include "common/memory_patcher.h"
#include "common/path_util.h"
#include "common/scope_exit.h"
#include "core/emulator_settings.h"
#include <array>
#include <fstream>
#include <nlohmann/json.hpp>
#include <pugixml.hpp>
#include <stdexcept>
#include <vector>
#include <windows.h>

#include <bcrypt.h>

namespace Core::Release {
namespace {
std::string mode;
std::string display;

// Verify the entire dumped executable before applying region/version-specific patches.
void VerifyExecutable(const std::filesystem::path& file) {
    BCRYPT_ALG_HANDLE algorithm{};
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0) {
        throw std::runtime_error("Cannot initialize SHA-256.");
    }
    SCOPE_EXIT { BCryptCloseAlgorithmProvider(algorithm, 0); };
    DWORD size{}, returned{};
    if (BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&size),
                          sizeof(size), &returned, 0) < 0) {
        throw std::runtime_error("Cannot query SHA-256 storage.");
    }
    std::vector<unsigned char> object(size);
    BCRYPT_HASH_HANDLE hash{};
    if (BCryptCreateHash(algorithm, &hash, object.data(), size, nullptr, 0, 0) < 0) {
        throw std::runtime_error("Cannot create SHA-256.");
    }
    SCOPE_EXIT { BCryptDestroyHash(hash); };
    std::ifstream input(file, std::ios::binary);
    if (!input) {
        throw std::runtime_error("Cannot read the game executable.");
    }
    std::array<unsigned char, 65536> buffer{};
    while (input) {
        input.read(reinterpret_cast<char*>(buffer.data()), buffer.size());
        const auto count = input.gcount();
        if (count > 0 && BCryptHashData(hash, buffer.data(), static_cast<ULONG>(count), 0) < 0) {
            throw std::runtime_error("Cannot hash the game executable.");
        }
    }
    if (!input.eof()) {
        throw std::runtime_error("Game executable read failed.");
    }
    std::array<unsigned char, 32> digest{};
    if (BCryptFinishHash(hash, digest.data(), digest.size(), 0) < 0) {
        throw std::runtime_error("Cannot finish SHA-256.");
    }
    constexpr std::string_view expected =
        "2d1ca79630d7bbe6fa29575f59aa7996d43071d74ac8e4f7d40137967d9039ab";
    constexpr std::string_view hex = "0123456789abcdef";
    std::string actual;
    for (const auto byte : digest) {
        actual += hex[byte >> 4];
        actual += hex[byte & 15];
    }
    if (actual != expected) {
        throw std::runtime_error("Unsupported game executable. CUSA00309 01.07 is required.");
    }
}

void PreparePatches(const std::filesystem::path& root) {
    pugi::xml_document patches;
    const auto source = root / "profiles" / "CUSA00309.xml";
    if (!patches.load_file(source.c_str())) {
        throw std::runtime_error("Cannot read profiles/CUSA00309.xml.");
    }
    if (std::string_view(patches.child("Patch").child("TitleID").child_value("ID")) !=
        "CUSA00309") {
        throw std::runtime_error("Patch title does not match CUSA00309.");
    }
    bool found = false;
    for (auto patch : patches.child("Patch").children("Metadata")) {
        const std::string_view name = patch.attribute("Name").as_string();
        if (name.starts_with("Native ") || name.starts_with("PS4 Pro mode:")) {
            const bool selected =
                mode == "pro4k" && name == "PS4 Pro mode: native 3840x2160 without checkerboard";
            patch.attribute("isEnabled").set_value(selected ? "true" : "false");
            found |= selected;
        }
        // Precise readback is required for both exposure and complete particle effects.
        if (name.starts_with("Relaxed readbacks:")) {
            patch.attribute("isEnabled").set_value("false");
        }
    }
    if (mode == "pro4k" && !found) {
        throw std::runtime_error("The Pro 4K resolution patch is missing.");
    }
    const auto target =
        Common::FS::GetUserPath(Common::FS::PathType::PatchesDir) / "CUSA00309-release.xml";
    if (!patches.save_file(target.c_str())) {
        throw std::runtime_error("Cannot save the selected runtime patches.");
    }
    MemoryPatcher::patch_file = "state/play/user/patches/CUSA00309-release.xml";
}
} // namespace

std::filesystem::path ProjectRoot() {
    return Common::FS::GetUserPath(Common::FS::PathType::UserDir)
        .parent_path()
        .parent_path()
        .parent_path();
}

std::filesystem::path Prepare(const std::optional<std::filesystem::path>& game,
                              const std::optional<std::string>& requested_mode) {
    const auto root = ProjectRoot();
    std::ifstream input(root / "launcher.json");
    if (!input) {
        throw std::runtime_error("launcher.json is missing beside the application.");
    }
    const auto config = nlohmann::json::parse(input);
    mode = requested_mode.value_or(config.value("mode", std::string("pro4k")));
    display = config.value("display", std::string("borderless"));
    if (mode != "1080p" && mode != "pro4k") {
        throw std::runtime_error("mode must be 1080p or pro4k.");
    }
    if (display != "window" && display != "fullscreen" && display != "borderless") {
        throw std::runtime_error("display must be window, fullscreen or borderless.");
    }
    auto path =
        game.value_or(std::filesystem::u8path(config.value("game_path", std::string("game"))));
    if (path.is_relative()) {
        path = root / path;
    }
    if (std::filesystem::is_directory(path)) {
        if (std::filesystem::is_regular_file(path / "eboot.bin")) {
            path /= "eboot.bin";
        } else {
            path /= "CUSA00309/eboot.bin";
        }
    }
    path = std::filesystem::canonical(path);
    VerifyExecutable(path);
    PreparePatches(root);
    return path;
}

void Apply(std::string_view serial, std::string_view version) {
    if (serial != "CUSA00309" || version != "01.07") {
        throw std::runtime_error("Only CUSA00309 01.07 is supported by this release.");
    }
    // These were effective per-game overrides of the verified V2 launcher.
    EmulatorSettings.SetExtraDmemInMBytes(512, true);
    EmulatorSettings.SetRedZonePatchingEnabled(true, true);
    EmulatorSettings.SetHomeDir("state/play/home");
    EmulatorSettings.SetSysModulesDir("state/play/user/sys_modules");
    EmulatorSettings.SetFontsDir("state/play/user/fonts");
    EmulatorSettings.SetAddonInstallDir("state/play/user/addcont");
    EmulatorSettings.SetNeo(mode == "pro4k", true);
    EmulatorSettings.SetWindowWidth(mode == "pro4k" ? 3840 : 1920, true);
    EmulatorSettings.SetWindowHeight(mode == "pro4k" ? 2160 : 1080, true);
    EmulatorSettings.SetFullScreen(display != "window", true);
    EmulatorSettings.SetFullScreenMode(display == "fullscreen" ? "Fullscreen" : "Borderless", true);
    EmulatorSettings.SetReadbacksMode(static_cast<u32>(GpuReadbacksMode::Precise), true);
    EmulatorSettings.SetHdrAllowed(false, true);
}
} // namespace Core::Release
