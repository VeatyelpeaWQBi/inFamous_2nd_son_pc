// SPDX-License-Identifier: GPL-2.0-or-later
#include "core/release_launcher.h"
#include "core/release_config.h"
#include "common/scope_exit.h"
#include <array>
#include <cstddef>
#include <fstream>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string_view>
#include <windows.h>

namespace Core::Release {
namespace {
std::wstring Quote(std::wstring_view value) {
    std::wstring quoted{L"\""};
    std::size_t slashes = 0;
    for (const auto c : value) {
        if (c == L'\\') {
            ++slashes;
            continue;
        }
        quoted.append(c == L'"' ? slashes * 2 + 1 : slashes, L'\\');
        quoted += c;
        slashes = 0;
    }
    quoted.append(slashes * 2, L'\\');
    quoted += L'"';
    return quoted;
}
void CheckLocalPath(const std::filesystem::path& path) {
    const auto root = ProjectRoot();
    const auto relative = path.lexically_relative(root);
    if (relative.empty() || relative.native().starts_with(L"..")) {
        throw std::runtime_error("Launch menu data must stay inside the project.");
    }
    for (auto entry = path; entry != root; entry = entry.parent_path()) {
        const auto attributes = GetFileAttributesW(entry.c_str());
        if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
            throw std::runtime_error("Launch menu paths cannot use external directory links.");
        }
    }
}
} // namespace
std::optional<LaunchSelection> ShowLaunchMenu(const std::filesystem::path& game,
                                             const std::string& mode,
                                             const std::string& display,
                                             const std::optional<std::string>& override_mode) {
    std::array<wchar_t, 32768> image{};
    const auto length = GetModuleFileNameW(nullptr, image.data(), static_cast<DWORD>(image.size()));
    if (!length || length >= image.size()) {
        throw std::runtime_error("Cannot locate the game executable.");
    }
    const auto helper = std::filesystem::path{image.data()}.parent_path() / "support/Ifss.Launcher.exe";
    CheckLocalPath(helper);
    if (!std::filesystem::is_regular_file(helper)) {
        throw std::runtime_error(
            "support/Ifss.Launcher.exe is missing. Rebuild or fully extract the package. "
            "The menu requires Windows .NET Framework 4.8.");
    }
    const auto relative = "state/runtime/launch-menu-" + std::to_string(GetCurrentProcessId()) +
                          "-" + std::to_string(GetTickCount64()) + ".json";
    const auto response = ProjectRoot() / relative;
    CheckLocalPath(response);
    if (std::filesystem::exists(response)) {
        throw std::runtime_error("Launch menu response already exists.");
    }
    SCOPE_EXIT {
        std::error_code ignored;
        std::filesystem::remove(response, ignored);
    };
    std::wstring command = Quote(helper.native());
    for (const auto& value : {ProjectRoot().native(), game.parent_path().native(),
                              std::wstring(mode.begin(), mode.end()),
                              std::wstring(display.begin(), display.end()),
                              override_mode
                                  ? std::wstring(override_mode->begin(), override_mode->end())
                                  : std::wstring{L"auto"},
                              std::wstring(relative.begin(), relative.end())}) {
        command += L" " + Quote(value);
    }
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(helper.c_str(), command.data(), nullptr, nullptr, FALSE, 0, nullptr,
                        ProjectRoot().c_str(), &startup, &process)) {
        throw std::runtime_error(
            "Cannot start the launch menu. Check the complete package and Windows .NET Framework 4.8.");
    }
    SCOPE_EXIT {
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
    };
    DWORD code{};
    if (WaitForSingleObject(process.hProcess, INFINITE) != WAIT_OBJECT_0 ||
        !GetExitCodeProcess(process.hProcess, &code)) {
        throw std::runtime_error("Cannot receive the launch menu result.");
    }
    if (code == 2) {
        return std::nullopt;
    }
    if (code != 0 || !std::filesystem::is_regular_file(response) ||
        std::filesystem::file_size(response) > 65536) {
        throw std::runtime_error("The launch menu failed or returned no valid settings.");
    }
    CheckLocalPath(response);
    std::ifstream input(response);
    const auto settings = nlohmann::json::parse(input);
    if (settings.at("schema") != 3 || !settings.at("console_language").is_number_integer() ||
        !settings.at("is_circle_enter").is_boolean()) {
        throw std::runtime_error("Invalid launch menu response.");
    }
    LaunchSelection result{settings.at("resolution").get<std::string>(),
                           settings.at("display_mode").get<std::string>(),
                           settings.at("console_language").get<int>(),
                           settings.at("is_circle_enter").get<bool>()};
    if ((result.display != "windowed" && result.display != "fullscreen" && result.display != "borderless") ||
        (result.language != 1 && result.language != 9 && result.language != 10)) {
        throw std::runtime_error("Unsupported launch menu settings.");
    }
    return result;
}
} // namespace Core::Release
