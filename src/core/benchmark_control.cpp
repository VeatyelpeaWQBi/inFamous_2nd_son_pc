// SPDX-FileCopyrightText: Copyright 2026 shadPS4-AYOUB1080p contributors
// SPDX-License-Identifier: GPL-2.0-or-later

#include "core/benchmark_control.h"

#include <chrono>
#include <array>
#include <fstream>
#include <locale>
#include <stdexcept>
#include <string_view>
#ifdef _WIN32
#include <windows.h>
#endif
#include <nlohmann/json.hpp>
#include <SDL3/SDL_events.h>
#include <SDL3/SDL_video.h>

#include "common/logging/log.h"
#include "common/elf_info.h"
#include "common/perf_stats.h"
#include "common/path_util.h"
#include "core/emulator_settings.h"
#include "input/controller.h"
#include "video_core/renderdoc.h"

namespace Core::Benchmark {
namespace {
using Clock = std::chrono::steady_clock;
std::filesystem::path root;
Clock::time_point next_poll{}, release_time{};
u64 last_sequence{};
std::atomic<u64> native_compute{}, native_graphics_compute{};
Libraries::Pad::OrbisPadButtonDataOffset held_button{};
Libraries::Pad::OrbisPadButtonDataOffset Button(std::string_view name) {
    using B = Libraries::Pad::OrbisPadButtonDataOffset;
    if (name == "cross") return B::Cross;
    if (name == "circle") return B::Circle;
    if (name == "options") return B::Options;
    if (name == "up") return B::Up;
    if (name == "down") return B::Down;
    if (name == "left") return B::Left;
    if (name == "right") return B::Right;
    if (name == "square") return B::Square;
    if (name == "triangle") return B::Triangle;
    throw std::runtime_error("Unsupported benchmark button");
}
} // namespace

void Configure(const std::filesystem::path& directory) {
    root = std::filesystem::canonical(directory);
    const auto user = std::filesystem::weakly_canonical(
        Common::FS::GetUserPath(Common::FS::PathType::UserDir));
    if (root != user.parent_path()) {
        throw std::runtime_error("Benchmark directory must own the selected project user directory");
    }
    for (const char* child : {"log", "screenshots", "home", "custom_configs", "patches"}) {
        std::filesystem::create_directories(root / child);
    }
    Common::FS::SetUserPath(Common::FS::PathType::LogDir, root / "log");
    Common::FS::SetUserPath(Common::FS::PathType::ScreenshotsDir, root / "screenshots");
    Common::FS::SetUserPath(Common::FS::PathType::CustomConfigs, root / "custom_configs");

}

bool Enabled() {
    return !root.empty();
}

void NoteNativeCompute(bool graphics) {
    if (!Enabled()) return;
    native_compute.fetch_add(1, std::memory_order_relaxed);
    if (graphics) native_graphics_compute.fetch_add(1, std::memory_order_relaxed);
}

std::filesystem::path TuningPath() {
    return root / "perf_tuning.ini";
}

void PrepareGame(std::string_view serial) {
    if (!Enabled()) return;
    if (serial != "CUSA00309") {
        throw std::runtime_error("The benchmark control mode is restricted to CUSA00309");
    }
    EmulatorSettings.SetHomeDir(root / "home");
    Common::FS::SetUserPath(Common::FS::PathType::HomeDir, root / "home");
    LOG_INFO(Debug, "Benchmark mode: isolated home {}, control file {}",
             (root / "home").string(), (root / "command.json").string());
}

void OnFlip() {
    if (!Enabled()) return;
    struct Frames {
        std::ofstream file{root / "frames.csv"};
        Clock::time_point start{}, previous{}, flushed{};
        u64 index{};
        Frames() {
            file.imbue(std::locale::classic());
            file << "frame,elapsed_ns,interval_ns\n";
        }
    };
    static Frames frames;
    const auto now = Clock::now();
    if (frames.index == 0) {
        frames.start = frames.previous = frames.flushed = now;
    }
    const auto ns = [](auto duration) {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(duration).count();
    };
    frames.file << frames.index++ << ',' << ns(now - frames.start) << ','
                << ns(now - frames.previous) << '\n';
    frames.previous = now;
    if (now - frames.flushed >= std::chrono::seconds{1}) {
        frames.file.flush();
        frames.flushed = now;
    }
}

void Poll(Input::GameController& controller) {
    if (!Enabled()) return;
    const auto now = Clock::now();
    if (held_button != Libraries::Pad::OrbisPadButtonDataOffset::None && now >= release_time) {
        controller.Button(held_button, false);
        held_button = Libraries::Pad::OrbisPadButtonDataOffset::None;
    }
    if (now < next_poll) return;
    next_poll = now + std::chrono::milliseconds{100};
    try {
        const auto path = root / "command.json";
        std::error_code error;
        const auto size = std::filesystem::file_size(path, error);
        if (error || size == 0 || size > 4096) return;
        std::ifstream input{path};
        std::array<char, 4096> bytes{};
        input.read(bytes.data(), static_cast<std::streamsize>(size));
        const auto command = nlohmann::json::parse(bytes.data(), bytes.data() + input.gcount(),
                                                 nullptr, false);
        if (command.is_discarded()) return; // The producer can be replacing the small file.
        const u64 sequence = command.at("sequence").get<u64>();
        if (sequence <= last_sequence) return;
        const auto action = command.at("action").get<std::string>();
        if (action == "button") {
            const auto button = Button(command.at("button").get<std::string>());
            const int duration = command.value("duration_ms", 200);
            if (duration < 50 || duration > 2000) throw std::runtime_error("Invalid pulse duration");
            if (held_button != Libraries::Pad::OrbisPadButtonDataOffset::None)
                controller.Button(held_button, false);
            held_button = button;
            controller.Button(button, true);
            release_time = now + std::chrono::milliseconds{duration};
        } else if (action == "axis") {
            const int axis = command.at("axis").get<int>();
            const int value = command.at("value").get<int>();
            if (axis < 0 || axis > 3 || value < 0 || value > 255)
                throw std::runtime_error("Invalid benchmark axis");
            controller.Axis(static_cast<Input::Axis>(axis), value, false);
        } else if (action == "screenshot") {
            VideoCore::RequestScreenshot(VideoCore::ScreenshotRequest::GameOnly);
        } else if (action == "status") {
            const auto& tuning = Common::Perf::GetTuning();
            nlohmann::json status{
                {"sequence", sequence}, {"game_serial", Common::ElfInfo::Instance().GameSerial()},
                {"cpu_affinity", tuning.cpu_affinity.load()},
                {"asc_queue", tuning.asc_queue.load()},
                {"perf_log", tuning.perf_log.load()}, {"gpu_profile", tuning.gpu_profile.load()},
                {"readback_window_kb", tuning.readback_window_bytes.load() / 1024},
                {"readback_prefetch", tuning.readback_prefetch.load()},
                {"recorder_spin_us", tuning.recorder_spin_us.load()},
                {"gpu_threads_affinity", tuning.gpu_threads_affinity.load()},
                {"other_threads_affinity", tuning.other_threads_affinity.load()},
                {"asc_dispatches", Common::Perf::Total(Common::Perf::AscQueueDispatches)},
                {"native_compute", native_compute.load(std::memory_order_relaxed)},
                {"native_graphics_compute", native_graphics_compute.load(std::memory_order_relaxed)},
            };
            if (tuning.gpu_threads_affinity.load() == Common::Perf::AffinityAuto) {
                status["gpu_threads_affinity"] = "auto";
            }
#ifdef _WIN32
            DWORD_PTR process_mask{}, system_mask{};
            if (GetProcessAffinityMask(GetCurrentProcess(), &process_mask, &system_mask)) {
                status["process_affinity"] = static_cast<u64>(process_mask);
            }
#endif
            int window_count{};
            if (auto** windows = SDL_GetWindows(&window_count)) {
                status["display_window_count"] = window_count;
                // The benchmark owns one SDL game window. Its Win32 Vulkan surface
                // does not require SDL_WINDOW_VULKAN; do not filter it by that flag.
                if (window_count == 1) {
                    constexpr int i = 0;
                    int width{}, height{};
                    SDL_GetWindowSizeInPixels(windows[i], &width, &height);
                    status["display_width"] = width;
                    status["display_height"] = height;
                    status["display_fullscreen"] =
                        bool(SDL_GetWindowFlags(windows[i]) & SDL_WINDOW_FULLSCREEN);
                    status["display_mode"] = SDL_GetWindowFullscreenMode(windows[i])
                        ? "Fullscreen" : "Fullscreen (Borderless)";
                }
                SDL_free(windows);
            }
            std::ofstream{root / "status.json"} << status.dump(2);
        } else if (action == "quit") {
            SDL_Event event{};
            event.type = SDL_EVENT_QUIT;
            SDL_PushEvent(&event);
        } else {
            throw std::runtime_error("Unsupported benchmark action");
        }
        last_sequence = sequence;
        LOG_INFO(Debug, "Benchmark command {}: {}", sequence, action);
    } catch (const std::exception& error) {
        LOG_WARNING(Debug, "Benchmark command rejected: {}", error.what());
    }
}
} // namespace Core::Benchmark
