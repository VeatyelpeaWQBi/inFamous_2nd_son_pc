// SPDX-FileCopyrightText: Copyright 2025-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
#include "common/key_manager.h"
#include "common/logging/log.h"
#include "common/path_util.h"
#include "common/scope_exit.h"
#include "common/string_util.h"
#include "core/emulator_settings.h"
#include "core/release_config.h"
#include "core/user_settings.h"
#include "emulator.h"
#include <SDL3/SDL_messagebox.h>
#include <filesystem>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>
#include <windows.h>

int wmain(int argc, wchar_t* argv[]) {
    SetConsoleOutputCP(CP_UTF8);
    try {
        const auto executable = Common::FS::PathToUTF8String(std::filesystem::absolute(argv[0]));
        std::optional<std::filesystem::path> game, override_root;
        std::optional<std::string> mode;
        std::vector<std::string> guest_args;
        bool append_log = false;
        for (int i = 1; i < argc; ++i) {
            const std::wstring_view arg{argv[i]};
            const auto value = [&]() -> std::wstring_view {
                if (++i >= argc) {
                    throw std::runtime_error("Missing command-line option value.");
                }
                return argv[i];
            };
            if (arg == L"--game" || arg == L"-g") {
                game = std::filesystem::absolute(value());
            } else if (arg == L"--mode") {
                mode = Common::UTF16ToUTF8(value());
            } else if (arg == L"--override-root") {
                override_root = std::filesystem::absolute(value());
            } else if (arg == L"--log-append") {
                append_log = true;
            } else if (arg == L"--patch") {
                value();
            } // Restart reselects the verified project patch.
            else if (arg == L"--wait-for-pid") {
                const auto pid = std::stoul(std::wstring(value()));
                if (const HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, pid)) {
                    const auto result = WaitForSingleObject(process, 30000);
                    CloseHandle(process);
                    if (result != WAIT_OBJECT_0) {
                        throw std::runtime_error("Previous process did not exit.");
                    }
                }
            } else if (arg == L"--") {
                while (++i < argc) {
                    guest_args.push_back(Common::UTF16ToUTF8(argv[i]));
                }
            } else if (arg == L"--help" || arg == L"-h") {
                std::cout
                    << "inFAMOUS Second Son\n"
                       "Use launcher.json, or --game <directory/eboot.bin> --mode <1080p|pro4k>.\n"
                       "F1: quit confirmation. User files: state/play/.\n";
                return 0;
            } else {
                throw std::runtime_error("Unknown command-line option; use --help.");
            }
        }
        const auto lock_path = Core::Release::ProjectRoot() / "state/runtime/run.lock";
        const HANDLE run_lock = CreateFileW(lock_path.c_str(), GENERIC_READ | GENERIC_WRITE, 0,
                                            nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (run_lock == INVALID_HANDLE_VALUE) {
            throw std::runtime_error("This project is already running, building, or packaging.");
        }
        SCOPE_EXIT { CloseHandle(run_lock); };
        const auto eboot = Core::Release::Prepare(game, mode);
        std::filesystem::current_path(Core::Release::ProjectRoot());
        Common::Log::Setup("engine.log");
        KeyManager::GetInstance()->LoadFromFile();
        EmulatorSettings.Load();
        UserSettings.Load();
        Core::Release::Apply("CUSA00309", "01.07");
        auto* emulator = Common::Singleton<Core::Emulator>::Instance();
        emulator->executableName = executable.c_str();
        emulator->Run(eboot, guest_args, override_root, {}, {}, append_log);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "inFAMOUS Second Son", error.what(),
                                 nullptr);
        return 1;
    }
}
