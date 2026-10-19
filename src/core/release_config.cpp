// SPDX-License-Identifier: GPL-2.0-or-later
#include "core/release_config.h"
#include "common/logging/log.h"
#include "common/memory_patcher.h"
#include "common/path_util.h"
#include "common/scope_exit.h"
#include "core/emulator_settings.h"
#include "core/file_sys/ifile.h"
#include "core/second_son_compatibility.h"
#include <algorithm>
#include <array>
#include <fstream>
#include <iostream>
#include <cwchar>
#include <nlohmann/json.hpp>
#include <pugixml.hpp>
#include <stdexcept>
#include <vector>
#include <utility>
#include <windows.h>

#include <bcrypt.h>

namespace Core::Release {
namespace {
std::string requested_mode, mode, display;

// Hash the effective mounted program, including an update overlay when present.
std::string HashExecutable(FileSys::IFile& file) {
    const auto original_position = file.Tell();
    if (!file.Seek(0, Common::FS::SeekOrigin::SetOrigin) || file.Size() == 0) {
        throw std::runtime_error("Cannot read the mounted game executable.");
    }
    SCOPE_EXIT { file.Seek(original_position, Common::FS::SeekOrigin::SetOrigin); };
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
    std::array<unsigned char, 65536> buffer{};
    u64 remaining = file.Size();
    while (remaining != 0) {
        const auto wanted = std::min<u64>(remaining, buffer.size());
        const auto count = file.Read(buffer.data(), wanted);
        if (count <= 0 || static_cast<u64>(count) > wanted ||
            BCryptHashData(hash, buffer.data(), static_cast<ULONG>(count), 0) < 0) {
            throw std::runtime_error("Cannot hash the mounted game executable.");
        }
        remaining -= count;
    }
    std::array<unsigned char, 32> digest{};
    if (BCryptFinishHash(hash, digest.data(), digest.size(), 0) < 0) {
        throw std::runtime_error("Cannot finish SHA-256.");
    }
    constexpr std::string_view hex = "0123456789abcdef";
    std::string actual;
    actual.reserve(64);
    for (const auto byte : digest) {
        actual += hex[byte >> 4];
        actual += hex[byte & 15];
    }
    return actual;
}

std::optional<SecondSon::Build> FindBuild(std::string_view serial, std::string_view version,
                                          std::string_view sha256) {
    std::ifstream input(ProjectRoot() / "profiles/second-son-builds.json");
    if (!input) {
        throw std::runtime_error("profiles/second-son-builds.json is missing.");
    }
    const auto registry = nlohmann::json::parse(input);
    if (registry.at("schema") != 1 || registry.at("game") != "inFAMOUS Second Son" ||
        !registry.at("builds").is_array()) {
        throw std::runtime_error("Invalid Second Son compatibility manifest.");
    }
    std::optional<SecondSon::Build> matched;
    for (const auto& entry : registry.at("builds")) {
        SecondSon::Build build{
            .title_ids = entry.at("title_ids").get<std::vector<std::string>>(),
            .version = entry.at("version").get<std::string>(),
            .sha256 = entry.at("sha256").get<std::string>(),
            .patch_file = entry.at("patch_file").get<std::string>(),
            .legacy_cache_title = entry.value("legacy_cache_title", std::string{}),
        };
        if (!build.IsValid()) {
            throw std::runtime_error("Invalid or out-of-scope Second Son build entry.");
        }
        if (build.Matches(serial, version, sha256)) {
            if (matched) {
                throw std::runtime_error("Multiple patch profiles match this executable.");
            }
            matched = std::move(build);
        }
    }
    return matched;
}

void SelectCaches(std::string_view serial, std::string_view version, std::string_view sha256,
                  bool keep_baseline) {
    if (keep_baseline) {
        return; // Preserve the already-warmed, verified HK V2 cache.
    }
    const auto root = ProjectRoot();
    const auto base = root / "state/runtime/versions" / serial / version / sha256;
    for (auto path = base; path != root; path = path.parent_path()) {
        const auto attributes = GetFileAttributesW(path.c_str());
        if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
            throw std::runtime_error("Version-specific cache cannot use external directory links.");
        }
    }
    for (const auto& [type, name] : {std::pair{Common::FS::PathType::CacheDir, "cache"},
                                     std::pair{Common::FS::PathType::ShaderDir, "shader"}}) {
        const auto path = base / name;
        const auto attributes = GetFileAttributesW(path.c_str());
        if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
            throw std::runtime_error("Version-specific cache cannot use external directory links.");
        }
        std::filesystem::create_directories(path);
        Common::FS::SetUserPath(type, path);
    }
}

void PreparePatches(std::string_view serial, std::string_view version,
                    const std::optional<SecondSon::Build>& build) {
    pugi::xml_document patches;
    if (build) {
        const auto source = ProjectRoot() / "profiles" / build->patch_file;
        if (!patches.load_file(source.c_str())) {
            throw std::runtime_error("Cannot read the matching game patch profile.");
        }
        bool title_matches = false;
        for (const auto id : patches.child("Patch").child("TitleID").children("ID")) {
            title_matches |= serial == id.child_value();
        }
        if (!title_matches) {
            throw std::runtime_error("Patch profile does not declare this Second Son title.");
        }
    } else {
        // An empty, explicit selection prevents fallback to old per-game/user patches.
        patches.append_child("Patch").append_child("TitleID").append_child("ID").text().set(
            std::string(serial).c_str());
    }
    bool resolution_found = false;
    for (auto patch : patches.child("Patch").children("Metadata")) {
        const std::string_view name = patch.attribute("Name").as_string();
        if (!patch.attribute("isEnabled")) {
            throw std::runtime_error("Patch profile is missing an enabled-state field.");
        }
        if (name.starts_with("Native ") || name.starts_with("PS4 Pro mode:")) {
            const bool selected =
                mode == "pro4k" && name == "PS4 Pro mode: native 3840x2160 without checkerboard";
            patch.attribute("isEnabled").set_value(selected ? "true" : "false");
            resolution_found |= selected;
        }
        if (name.starts_with("Relaxed readbacks:")) {
            patch.attribute("isEnabled").set_value("false");
        }
        if (patch.attribute("isEnabled").as_bool() &&
            (version != patch.attribute("AppVer").as_string() ||
             std::string_view(patch.attribute("AppElf").as_string()) != "eboot.bin")) {
            throw std::runtime_error("An enabled patch targets a different version/executable.");
        }
    }
    if (mode == "pro4k" && !resolution_found) {
        throw std::runtime_error("The matching profile has no Pro 4K resolution patch.");
    }
    const auto filename = std::string(serial) + "-release.xml";
    const auto target = Common::FS::GetUserPath(Common::FS::PathType::PatchesDir) / filename;
    if (!patches.save_file(target.c_str())) {
        throw std::runtime_error("Cannot save the selected runtime patches.");
    }
    MemoryPatcher::patch_file = "state/play/user/patches/" + filename;
}
} // namespace

std::filesystem::path ProjectRoot() {
    return Common::FS::GetUserPath(Common::FS::PathType::UserDir)
        .parent_path()
        .parent_path()
        .parent_path();
}

std::filesystem::path Prepare(const std::optional<std::filesystem::path>& game,
                              const std::optional<std::string>& requested) {
    std::ifstream input(ProjectRoot() / "launcher.json");
    if (!input) {
        throw std::runtime_error("launcher.json is missing beside the application.");
    }
    const auto config = nlohmann::json::parse(input);
    requested_mode = requested.value_or(config.value("mode", std::string("pro4k")));
    display = config.value("display", std::string("borderless"));
    if (requested_mode != "1080p" && requested_mode != "pro4k") {
        throw std::runtime_error("mode must be 1080p or pro4k.");
    }
    if (display != "window" && display != "fullscreen" && display != "borderless") {
        throw std::runtime_error("display must be window, fullscreen or borderless.");
    }
    mode = "1080p"; // A verified program must be selected before enabling Pro 4K.
    MemoryPatcher::patch_file.clear();
    const auto configured_game_path = config.value("game_path", std::string("game"));
    auto path = game.value_or(std::filesystem::path{
        std::u8string(configured_game_path.begin(), configured_game_path.end())});
    if (path.is_relative()) {
        path = ProjectRoot() / path;
    }
    if (std::filesystem::is_directory(path)) {
        if (std::filesystem::is_regular_file(path / "eboot.bin")) {
            path /= "eboot.bin";
        } else {
            std::optional<std::filesystem::path> found;
            for (const auto title : SecondSon::TitleIds) {
                const auto candidate = path / title / "eboot.bin";
                if (!std::filesystem::is_regular_file(candidate)) {
                    continue;
                }
                if (found) {
                    throw std::runtime_error(
                        "Multiple Second Son game folders found; set game_path to one of them.");
                }
                found = candidate;
            }
            if (!found) {
                throw std::runtime_error("No Second Son eboot.bin found; set game_path.");
            }
            path = *found;
        }
    }
    path = std::filesystem::canonical(path);
    if (!std::filesystem::is_regular_file(path)) {
        throw std::runtime_error("game_path must select a game directory or executable.");
    }
    if (_wcsicmp(path.filename().c_str(), L"eboot.bin") != 0) {
        throw std::runtime_error("Select the Second Son eboot.bin or its game directory.");
    }
    return path;
}

void SelectGame(std::string_view serial, std::string_view version, FileSys::IFile& executable) {
    if (!SecondSon::IsTitle(serial)) {
        throw std::runtime_error(
            "Only inFAMOUS Second Son is supported (not First Light or other PS4 games).");
    }
    if (!SecondSon::IsVersion(version)) {
        throw std::runtime_error("The mounted game has an invalid APP_VER.");
    }
    const auto sha256 = HashExecutable(executable);
    const auto build = FindBuild(serial, version, sha256);
    mode = SecondSon::EffectiveMode(requested_mode, build.has_value());
    SelectCaches(serial, version, sha256, build && build->legacy_cache_title == serial);
    PreparePatches(serial, version, build);
    std::cout << "Second Son " << serial << ' ' << version << ": " << mode
              << (build ? " (verified patch profile)" : " (base compatibility, no XML patches)")
              << std::endl;
    if (!build) {
        LOG_WARNING(
            Loader,
            "Second Son {} {}: no verified patch profile for SHA256 {}. Using base PS4 1080p, "
            "precise readback and SDR without XML gameplay patches (requested {}).",
            serial, version, sha256, requested_mode);
    }
}

void Apply() {
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
