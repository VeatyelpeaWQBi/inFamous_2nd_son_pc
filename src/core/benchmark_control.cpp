// SPDX-FileCopyrightText: Copyright 2026 shadPS4 contributors
// SPDX-License-Identifier: GPL-2.0-or-later

#include "core/benchmark_control.h"

#include <chrono>
#include <array>
#include <algorithm>
#include <charconv>
#include <cstdlib>
#include <cstdio>
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
#include "common/path_trace.h"
#include "common/path_util.h"
#include "core/emulator_settings.h"
#include "core/debug_state.h"
#include "input/controller.h"
#include "video_core/renderdoc.h"
#ifdef IFSS_HAVE_ITT
#include <ittnotify.h>
#endif
#ifdef IFSS_HAVE_NSIGHT_SDK
#include <NGFX_GPUTrace_Vulkan.h>
#endif

namespace Core::Benchmark {
namespace {
using Clock = std::chrono::steady_clock;
std::filesystem::path root;
Clock::time_point next_poll{}, release_time{};
u64 last_sequence{};
std::atomic<bool> quit_requested{};
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
struct ClockStamp {
    s64 unix_ns;
    s64 qpc_before;
    s64 qpc_after;
};
ClockStamp SampleClock() {
#ifdef _WIN32
    LARGE_INTEGER before{}, after{};
    FILETIME utc{};
    QueryPerformanceCounter(&before);
    GetSystemTimePreciseAsFileTime(&utc);
    QueryPerformanceCounter(&after);
    const u64 ticks = (u64{utc.dwHighDateTime} << 32) | utc.dwLowDateTime;
    return {static_cast<s64>((ticks - 116444736000000000ULL) * 100),
            before.QuadPart, after.QuadPart};
#else
    const auto before = Clock::now().time_since_epoch().count();
    const auto utc = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    return {utc, before, Clock::now().time_since_epoch().count()};
#endif
}
void PublishStatus(const nlohmann::json& status) {
    const auto path = root / "status.json";
    const auto pending = root / "status.json.pending";
    try {
        {
            std::ofstream output{pending, std::ios::binary | std::ios::trunc};
            output << status.dump(2);
            output.close();
            if (!output) throw std::runtime_error("Could not persist native status snapshot");
        }
        // Readers allow delete sharing and keep either the old or the new complete file.
#ifdef _WIN32
        if (!MoveFileExW(pending.c_str(), path.c_str(),
                         MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            throw std::runtime_error("Could not publish native status snapshot: Windows error " +
                                     std::to_string(GetLastError()));
        }
#else
        std::filesystem::rename(pending, path);
#endif
    } catch (...) {
        std::error_code ignored;
        std::filesystem::remove(pending, ignored);
        throw;
    }
}
void Acknowledge(u64 sequence, std::string_view action, bool accepted,
                 std::string_view error = {}) {
    const auto directory = root / "control-acks";
    std::filesystem::create_directories(directory);
    auto name = std::to_string(sequence);
    if (name.size() < 16) name.insert(0, 16 - name.size(), '0');
    const auto path = directory / (name + ".json");
    const auto pending = path.string() + ".pending";
    const auto stamp = SampleClock();
    const auto now = stamp.unix_ns / 1000000;
    const nlohmann::json acknowledgement{
        {"sequence", sequence}, {"action", action}, {"accepted", accepted},
        {"error", error}, {"applied_unix_ms", now},
        {"applied_unix_ns", stamp.unix_ns}, {"qpc_before", stamp.qpc_before},
        {"qpc_after", stamp.qpc_after},
        {"renderdoc_ready", VideoCore::IsRenderDocLoaded()}};
    {
        std::ofstream output{pending};
        output << acknowledgement.dump();
        output.close();
        if (!output) throw std::runtime_error("Could not persist control acknowledgement");
    }
    std::filesystem::rename(pending, path);
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

void PrepareGpuDiagnostics() {
#ifdef _WIN32
    if (!Enabled()) return;
    const auto environment_path = [](const wchar_t* name) {
        std::wstring value(32768, L'\0');
        const DWORD count = GetEnvironmentVariableW(name, value.data(), DWORD(value.size()));
        if (count >= value.size()) throw std::runtime_error("Diagnostic library path is too long");
        value.resize(count);
        return std::filesystem::path{value};
    };
    const auto install = environment_path(L"IFSS_NSIGHT_INSTALL");
    if (install.empty()) return;
#ifdef IFSS_HAVE_NSIGHT_SDK
    if (!install.is_absolute()) throw std::runtime_error("Nsight installation path must be absolute");
    const auto validation = environment_path(L"IFSS_VALIDATION_LIBRARY");
    if (!validation.empty()) {
        // Nsight calls Vulkan during its DLL teardown. Load validation first so
        // its dispatch records remain alive until after Nsight has detached.
        if (!validation.is_absolute() ||
            !LoadLibraryExW(validation.c_str(), nullptr,
                            LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS))
            throw std::runtime_error("Could not preload the installed Vulkan validation library");
    }
    NGFX_SetLibraryLoadFn([](const NGFX_PathChar* path) -> void* {
        return LoadLibraryExW(path, nullptr,
                              LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    });
    NGFX_GPUTrace_InjectionSettings settings{};
    auto result = NGFX_GPUTrace_InjectionSettings_SetDefaults(&settings);
    if (result != NGFX_Result_Success)
        throw std::runtime_error("Nsight injection defaults failed: " + std::to_string(result));
    settings.startEvent = NGFX_GPUTrace_StartEvent_NGFXSDK;
    settings.stopEvent = NGFX_GPUTrace_StopEvent_Frame;
    settings.stopParams.frameCount = 3;
    const auto duration = environment_path(L"IFSS_NSIGHT_DURATION_MS").wstring();
    size_t consumed{};
    const auto duration_ms = std::stoul(duration, &consumed);
    if (consumed != duration.size() || duration_ms < 1000 || duration_ms > 9000)
        throw std::runtime_error("Invalid Nsight diagnostic duration");
    settings.maxDurationMs = static_cast<uint32_t>(duration_ms);
    settings.eventBufferSizeKB = 32768;
    settings.timestampCount = 250000;
    settings.gpuClockMode = NGFX_GPUTrace_GPUClockMode_Unaltered;
    settings.vsyncMode = NGFX_GPUTrace_VSyncMode_ApplicationControlled;
    NGFX_GPUTrace_Inject_Vulkan_Params injection{
        NGFX_GPUTrace_Inject_Vulkan_Params_VER, install.c_str(), &settings};
    result = NGFX_GPUTrace_Inject_Vulkan(&injection);
    if (result != NGFX_Result_Success)
        throw std::runtime_error("Nsight SDK injection failed: " + std::to_string(result));
    LOG_INFO(Debug, "Nsight SDK injected before Vulkan initialization; validation preloaded: {}",
             !validation.empty());
#else
    throw std::runtime_error("This engine was built without the installed Nsight SDK");
#endif
#endif
}

#ifdef _WIN32
namespace {
HANDLE shutdown_trace = INVALID_HANDLE_VALUE;
LONG shutdown_faults{};
void WriteShutdownPc(const char* label, const void* pc) noexcept {
    HMODULE module{};
    char path[MAX_PATH]{}, line[512]{};
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                          GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                      static_cast<LPCSTR>(pc), &module);
    if (module) GetModuleFileNameA(module, path, MAX_PATH);
    const int length = std::snprintf(line, sizeof(line), "%s %p %s+0x%llx\r\n", label, pc, path,
        static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(pc) - reinterpret_cast<uintptr_t>(module)));
    DWORD written{};
    if (length > 0) WriteFile(shutdown_trace, line, static_cast<DWORD>(std::min<int>(length, sizeof(line) - 1)), &written, nullptr);
}
LONG WINAPI RecordShutdownException(EXCEPTION_POINTERS* info) noexcept {
    if (!info || info->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION)
        return EXCEPTION_CONTINUE_SEARCH;
    const auto last_error = GetLastError();
    HMODULE source{};
    const bool host_module = GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
        GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        static_cast<LPCSTR>(info->ExceptionRecord->ExceptionAddress), &source);
    if (!host_module || source == GetModuleHandleW(nullptr) || InterlockedIncrement(&shutdown_faults) > 16) {
        SetLastError(last_error);
        return EXCEPTION_CONTINUE_SEARCH;
    }
    // Guest and emulator readback page faults are excluded from this DLL teardown trace.
    WriteShutdownPc("fault", info->ExceptionRecord->ExceptionAddress);
    if (info->ExceptionRecord->NumberParameters >= 2)
        WriteShutdownPc("target", reinterpret_cast<void*>(info->ExceptionRecord->ExceptionInformation[1]));
    void* frames[24]{};
    const auto count = CaptureStackBackTrace(0, 24, frames, nullptr);
    for (USHORT index = 0; index < count; ++index) WriteShutdownPc("stack", frames[index]);
    SetLastError(last_error);
    return EXCEPTION_CONTINUE_SEARCH;
}
} // namespace
#endif

void ArmShutdownTrace() {
#ifdef _WIN32
    if (!Enabled() || shutdown_trace != INVALID_HANDLE_VALUE || !std::getenv("IFSS_NSIGHT_INSTALL")) return;
    const auto path = root / "diagnostics" / "shutdown-exceptions.txt";
    shutdown_trace = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                                 CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (shutdown_trace == INVALID_HANDLE_VALUE) throw std::runtime_error("Cannot open shutdown exception evidence");
    // Retain the file and handler until process termination, including DLL detach callbacks.
    if (!AddVectoredExceptionHandler(1, RecordShutdownException))
        throw std::runtime_error("Cannot register shutdown exception evidence");
#endif
}

bool ProfilerFastExitReady() {
#ifdef _WIN32
    const char* policy = std::getenv("IFSS_NSIGHT_SAFE_EXIT");
    if (!Enabled() || !policy || std::string_view{policy} != "1" ||
        !quit_requested.load(std::memory_order_acquire)) return false;
    std::ifstream input{root / "diagnostics" / "exit-ready.json"};
    if (!input) return false;
    const auto ready = nlohmann::json::parse(input);
    return ready.value("pid", 0UL) == GetCurrentProcessId() &&
           ready.value("policy", "") == "engine-shutdown-self-terminate" &&
           ready.value("nsight_exit_code", -1) == 0 && ready.value("vtune_completed", false);
#else
    return false;
#endif
}

[[noreturn]] void ExitAfterProfilerShutdown() {
#ifdef _WIN32
    // Engine Shutdown and log flush have already succeeded. The collector host
    // has exported its data. Avoid the reproduced nvperf_grfx_target DllMain AV;
    // no fault handler turns an exception into a successful exit.
    const nlohmann::json receipt{{"pid", GetCurrentProcessId()}, {"shutdown_completed", true},
        {"policy", "engine-shutdown-self-terminate"}, {"exit_code", 0},
        {"control_sequence", last_sequence}, {"exit_unix_ns", SampleClock().unix_ns}};
    std::ofstream output{root / "diagnostics" / "exit-completed.json"};
    output << receipt.dump(2);
    output.close();
    if (!output) throw std::runtime_error("Cannot persist completed diagnostic shutdown");
    if (!TerminateProcess(GetCurrentProcess(), 0))
        throw std::runtime_error("Cannot complete profiler-safe diagnostic exit");
#endif
    std::abort();
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
            file << "frame,elapsed_ns,interval_ns,utc_ns,qpc_before,qpc_after\n";
#ifdef _WIN32
            LARGE_INTEGER frequency{};
            if (!QueryPerformanceFrequency(&frequency))
                throw std::runtime_error("Could not query the diagnostic clock frequency");
            std::ofstream anchor{root / "clock.json"};
            anchor << nlohmann::json{{"schema", 1}, {"qpc_frequency", frequency.QuadPart},
                {"utc_epoch", "unix"}, {"utc_unit", "nanoseconds"},
                {"qpc_source", "QueryPerformanceCounter"},
                {"utc_source", "GetSystemTimePreciseAsFileTime"}}.dump();
            anchor.close();
            if (!anchor) throw std::runtime_error("Could not persist diagnostic clock metadata");
#endif
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
    const auto stamp = SampleClock();
    // Exact join to frames.csv: both use this index and the same steady-clock sample.
    Common::PathTrace::Emit(Common::PathTrace::Kind::FrameSample,
                            Common::PathTrace::NextId(), frames.index,
                            ns(now.time_since_epoch()), ns(now - frames.previous),
                            ns(now - frames.start));
    frames.file << frames.index++ << ',' << ns(now - frames.start) << ','
                << ns(now - frames.previous) << ',' << stamp.unix_ns << ','
                << stamp.qpc_before << ',' << stamp.qpc_after << '\n';
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
    if (now < next_poll ||
        held_button != Libraries::Pad::OrbisPadButtonDataOffset::None) return;
    next_poll = now + std::chrono::milliseconds{100};
    u64 command_sequence{};
    std::string command_action;
    try {
        auto path = root / "command.json";
        std::error_code error;
        const auto queue = root / "control-queue";
        if (std::filesystem::is_directory(queue, error)) {
            std::filesystem::path next;
            for (const auto& entry : std::filesystem::directory_iterator(queue)) {
                const auto name = entry.path().stem().string();
                if (!entry.is_regular_file() || entry.path().extension() != ".json" ||
                    name.size() != 16) continue;
                u64 value{};
                const auto result = std::from_chars(name.data(), name.data() + name.size(), value);
                if (result.ec != std::errc{} || result.ptr != name.data() + name.size() ||
                    value <= last_sequence) continue;
                if (next.empty() || entry.path().filename() < next.filename())
                    next = entry.path();
            }
            if (next.empty()) return;
            path = std::move(next);
        }
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
        command_sequence = sequence;
        command_action = action;
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
        } else if (action == "capture") {
            if (!VideoCore::IsRenderDocLoaded())
                throw std::runtime_error("RenderDoc capture API is not ready");
            VideoCore::TriggerCapture();
        } else if (action == "nsight") {
#ifdef IFSS_HAVE_NSIGHT_SDK
            NGFX_GPUTrace_InitializeActivity_Vulkan_Params init{
                NGFX_GPUTrace_InitializeActivity_Vulkan_Params_VER};
            auto result = NGFX_GPUTrace_InitializeActivity_Vulkan(&init);
            if (result != NGFX_Result_Success)
                throw std::runtime_error("Nsight SDK initialization failed: " + std::to_string(result));
            NGFX_GPUTrace_StartTrace_Vulkan_Params start{
                NGFX_GPUTrace_StartTrace_Vulkan_Params_VER};
            result = NGFX_GPUTrace_StartTrace_Vulkan(&start);
            if (result != NGFX_Result_Success)
                throw std::runtime_error("Nsight SDK trace start failed: " + std::to_string(result));
#else
            throw std::runtime_error("This engine was built without the installed Nsight SDK");
#endif
        } else if (action == "vtune-detach") {
#ifdef IFSS_HAVE_ITT
            const char* enabled = std::getenv("IFSS_VTUNE_CONTROL");
            if (!enabled || std::string_view{enabled} != "1")
                throw std::runtime_error("VTune control is disabled for this session");
            // Initialize the installed collector's control API only for this request.
            // Detach is documented to stop every collector while keeping the target alive.
            if (!__itt_domain_createA("IFSS.Diagnostics") || !__itt_detach_ptr)
                throw std::runtime_error("The active VTune collector has no ITT detach API");
            __itt_detach();
#else
            throw std::runtime_error("This engine was built without installed VTune ITT support");
#endif
        } else if (action == "nsight-stop") {
#ifdef IFSS_HAVE_NSIGHT_SDK
            NGFX_GPUTrace_StopTrace_Vulkan_Params stop{
                NGFX_GPUTrace_StopTrace_Vulkan_Params_VER};
            stop.flags = NGFX_GPUTrace_StopTraceFlag_NextFrameBoundary;
            const auto result = NGFX_GPUTrace_StopTrace_Vulkan(&stop);
            if (result != NGFX_Result_Success)
                throw std::runtime_error("Nsight SDK trace stop failed: " + std::to_string(result));
#else
            throw std::runtime_error("This engine was built without the installed Nsight SDK");
#endif
        } else if (action == "status") {
            const auto& tuning = Common::Perf::GetTuning();
            nlohmann::json status{
                {"schema", 2},
                {"sequence", sequence}, {"game_serial", Common::ElfInfo::Instance().GameSerial()},
                {"observed_unix_ms", SampleClock().unix_ns / 1000000},
                {"draws_per_frame", DebugState.GetDrawCallsAvg()},
                {"dispatches_per_frame", DebugState.GetDispatchesAvg()},
                {"renderdoc_ready", VideoCore::IsRenderDocLoaded()},
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
            PublishStatus(status);
        } else if (action == "quit") {
            quit_requested.store(true, std::memory_order_release);
            SDL_Event event{};
            event.type = SDL_EVENT_QUIT;
            SDL_PushEvent(&event);
        } else {
            throw std::runtime_error("Unsupported benchmark action");
        }
        Acknowledge(sequence, action, true);
        last_sequence = sequence;
        LOG_INFO(Debug, "Benchmark command {}: {}", sequence, action);
    } catch (const std::exception& error) {
        if (command_sequence > last_sequence) {
            last_sequence = command_sequence;
            try { Acknowledge(command_sequence, command_action, false, error.what()); }
            catch (const std::exception& output_error) {
                LOG_ERROR(Debug, "Control acknowledgement failed: {}", output_error.what());
            }
        }
        LOG_WARNING(Debug, "Benchmark command rejected: {}", error.what());
    }
}
} // namespace Core::Benchmark
