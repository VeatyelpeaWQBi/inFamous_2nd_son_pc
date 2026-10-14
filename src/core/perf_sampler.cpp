// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "core/perf_sampler.h"
#include "core/benchmark_control.h"

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "common/logging/log.h"
#include "common/path_util.h"
#include "common/perf_stats.h"
#include "common/types.h"

namespace Core::PerfSampler {

void ApplyGameProfile(std::string_view serial) {
    // inFAMOUS Second Son (EU, US, HK): emulator-level settings shared across regions.
    // Game executable patches remain region/version specific. perf_tuning.ini can change
    // any of these settings, SHADPS4_GAME_PROFILE=0 leaves the emulator defaults.
    constexpr std::array<std::string_view, 3> InfamousSecondSon{
        "CUSA00004", "CUSA00223", "CUSA00309"};
    if (std::ranges::find(InfamousSecondSon, serial) == InfamousSecondSon.end()) {
        return;
    }
    if (const char* setting = std::getenv("SHADPS4_GAME_PROFILE");
        setting != nullptr && setting[0] == '0') {
        LOG_INFO(Debug, "Game profile of {} not applied (SHADPS4_GAME_PROFILE=0)", serial);
        return;
    }
    auto& t = Common::Perf::GetTuning();
    constexpr auto relaxed = std::memory_order_relaxed;
    t.readback_write_discard.store(true, relaxed);
    t.readback_write_discard_from_ini.store(true, relaxed);
    t.async_pipelines.store(1, relaxed);
    t.compute_compile_parallel.store(true, relaxed);
    t.readback_queue.store(true, relaxed);
    t.stream_dedup.store(true, relaxed);
    t.stream_static.store(true, relaxed);
    t.gpu_clock_div.store(10, relaxed);
    t.asc_yield_packets.store(16, relaxed);
    t.real_occlusion.store(true, relaxed);
    t.wave_memory_barriers.store(true, relaxed);
    t.threaded_recording.store(true, relaxed);
    t.async_uploads.store(true, relaxed);
    t.staging_budget_auto.store(true, relaxed);
    t.recorder_spin_us.store(200, relaxed);
    t.sync_memo.store(true, relaxed);
    t.spec_memo.store(true, relaxed);
    t.gpu_clock_frame_us.store(16667, relaxed);
    t.region_summary.store(true, relaxed);
    t.tex_memo.store(true, relaxed);
    t.async_written_uploads.store(true, relaxed);
    t.skip_redundant_binds.store(true, relaxed);
    t.shader_params_memo.store(true, relaxed);
    t.runtime_lookup_memo.store(serial == "CUSA00309", relaxed);
    t.skip_empty_draws.store(true, relaxed);
    t.pipeline_key_memo.store(true, relaxed);
    t.bind_prefetch.store(true, relaxed);
    t.gpu_threads_affinity.store(Common::Perf::AffinityAuto, relaxed);
    t.rt_memo.store(true, relaxed);
    t.late_protect.store(true, relaxed);
    t.parallel_protect.store(true, relaxed);
    LOG_INFO(Debug, "Game profile of {} applied (inFAMOUS Second Son settings)", serial);
}

void PollTuning() {
    using Clock = std::chrono::steady_clock;
    static Clock::time_point next_check{};
    static std::filesystem::file_time_type last_write{};
    static bool has_file = false;

    const auto now = Clock::now();
    if (now < next_check) {
        return;
    }
    next_check = now + std::chrono::seconds{1};

    const auto path = Core::Benchmark::Enabled()
                          ? Core::Benchmark::TuningPath()
                          : Common::FS::GetUserPath(Common::FS::PathType::UserDir) / "perf_tuning.ini";
    std::error_code error;
    const auto write_time = std::filesystem::last_write_time(path, error);
    if (error) {
        has_file = false;
        return;
    }
    if (has_file && write_time == last_write) {
        return;
    }
    has_file = true;
    last_write = write_time;

    auto& tuning = Common::Perf::GetTuning();
    std::ifstream file{path};
    std::string line;
    const auto trim = [](std::string text) {
        const auto first = text.find_first_not_of(" \t\r");
        const auto last = text.find_last_not_of(" \t\r");
        return first == std::string::npos ? std::string{} : text.substr(first, last - first + 1);
    };
    while (std::getline(file, line)) {
        const auto comment = line.find('#');
        if (comment != std::string::npos) {
            line.resize(comment);
        }
        const auto equal = line.find('=');
        if (equal == std::string::npos) {
            continue;
        }
        const std::string key = trim(line.substr(0, equal));
        const std::string value = trim(line.substr(equal + 1));
        if (value.empty()) {
            continue;
        }
        if (key == "readback_window_kb") {
            const u64 kilobytes =
                std::clamp<u64>(std::strtoull(value.c_str(), nullptr, 10), 64, 256 * 1024);
            tuning.readback_window_bytes.store(std::bit_floor(kilobytes) * 1024,
                                               std::memory_order_relaxed);
        } else if (key == "readback_write_discard") {
            tuning.readback_write_discard.store(value != "0", std::memory_order_relaxed);
            tuning.readback_write_discard_from_ini.store(true, std::memory_order_relaxed);
        } else if (key == "readback_write_merge") {
            tuning.readback_write_merge.store(value != "0", std::memory_order_relaxed);
        } else if (key == "readback_keep_ranges") {
            tuning.readback_keep_ranges.store(value != "0", std::memory_order_relaxed);
        } else if (key == "threaded_recording") {
            tuning.threaded_recording.store(value != "0", std::memory_order_relaxed);
        } else if (key == "compute_compile_parallel") {
            tuning.compute_compile_parallel.store(value != "0", std::memory_order_relaxed);
        } else if (key == "async_uploads") {
            tuning.async_uploads.store(value != "0", std::memory_order_relaxed);
        } else if (key == "staging_budget_auto") {
            tuning.staging_budget_auto.store(value != "0", std::memory_order_relaxed);
        } else if (key == "sync_memo") {
            tuning.sync_memo.store(value != "0", std::memory_order_relaxed);
        } else if (key == "region_summary") {
            tuning.region_summary.store(value != "0", std::memory_order_relaxed);
        } else if (key == "tex_memo") {
            tuning.tex_memo.store(value != "0", std::memory_order_relaxed);
        } else if (key == "async_written_uploads") {
            tuning.async_written_uploads.store(value != "0", std::memory_order_relaxed);
        } else if (key == "scout") {
            tuning.scout.store(value != "0", std::memory_order_relaxed);
        } else if (key == "skip_redundant_binds") {
            tuning.skip_redundant_binds.store(value != "0", std::memory_order_relaxed);
        } else if (key == "shader_params_memo") {
            tuning.shader_params_memo.store(value != "0", std::memory_order_relaxed);
        } else if (key == "runtime_lookup_memo") {
            tuning.runtime_lookup_memo.store(value != "0", std::memory_order_relaxed);
        } else if (key == "skip_empty_draws") {
            tuning.skip_empty_draws.store(value != "0", std::memory_order_relaxed);
        } else if (key == "scout_fast") {
            tuning.scout_fast.store(value != "0", std::memory_order_relaxed);
        } else if (key == "pipeline_key_memo") {
            tuning.pipeline_key_memo.store(value != "0", std::memory_order_relaxed);
        } else if (key == "bind_prefetch") {
            tuning.bind_prefetch.store(value != "0", std::memory_order_relaxed);
        } else if (key == "gpu_threads_affinity") {
            tuning.gpu_threads_affinity.store(value == "auto"
                                                  ? Common::Perf::AffinityAuto
                                                  : std::strtoull(value.c_str(), nullptr, 16),
                                              std::memory_order_relaxed);
        } else if (key == "perf_log") {
            tuning.perf_log.store(value != "0", std::memory_order_relaxed);
        } else if (key == "rt_memo") {
            tuning.rt_memo.store(value != "0", std::memory_order_relaxed);
        } else if (key == "late_protect") {
            tuning.late_protect.store(value != "0", std::memory_order_relaxed);
        } else if (key == "parallel_protect") {
            tuning.parallel_protect.store(value != "0", std::memory_order_relaxed);
        } else if (key == "readback_relaxed_sites") {
            // "all", or offsets in hex of the game executable ("3f3e00, 3ff600"), up to 8.
            u32 count = 0;
            const bool all = value == "all";
            if (!all) {
                size_t pos = 0;
                while (pos < value.size() && count < Common::Perf::Tuning::MaxRelaxedSites) {
                    const size_t comma = std::min(value.find(',', pos), value.size());
                    const std::string item = value.substr(pos, comma - pos);
                    pos = comma + 1;
                    // "none" or anything not hexadecimal: no site.
                    const char* text = item.c_str() + std::min(item.find_first_not_of(" \t"),
                                                               item.size());
                    char* end = nullptr;
                    const u64 offset = std::strtoull(text, &end, 16);
                    if (end == text || offset == 0) {
                        continue;
                    }
                    tuning.relaxed_sites[count++].store(offset & ~u64{0xFF},
                                                        std::memory_order_relaxed);
                }
            }
            tuning.num_relaxed_sites.store(count, std::memory_order_release);
            tuning.relaxed_all.store(all, std::memory_order_release);
        } else if (key == "asc_queue") {
            tuning.asc_queue.store(value != "0", std::memory_order_relaxed);
        } else if (key == "gpu_profile") {
            tuning.gpu_profile.store(
                static_cast<u32>(std::min<u64>(std::strtoull(value.c_str(), nullptr, 10), 3)),
                std::memory_order_relaxed);
        } else if (key == "gpu_threads_priority") {
            tuning.gpu_threads_priority.store(
                static_cast<u32>(std::min<u64>(std::strtoull(value.c_str(), nullptr, 10), 2)),
                std::memory_order_relaxed);
        } else if (key == "other_threads_affinity") {
            tuning.other_threads_affinity.store(std::strtoull(value.c_str(), nullptr, 16),
                                                std::memory_order_relaxed);
        } else if (key == "cpu_affinity") {
            tuning.cpu_affinity.store(std::strtoull(value.c_str(), nullptr, 16),
                                      std::memory_order_relaxed);
        } else if (key == "gpu_clock_frame_us") {
            tuning.gpu_clock_frame_us.store(
                static_cast<u32>(std::min<u64>(std::strtoull(value.c_str(), nullptr, 10), 1000000)),
                std::memory_order_relaxed);
        } else if (key == "rt_size_learn") {
            tuning.rt_size_learn.store(value != "0", std::memory_order_relaxed);
        } else if (key == "spec_memo") {
            tuning.spec_memo.store(value != "0", std::memory_order_relaxed);
        } else if (key == "probe_host_import") {
            tuning.probe_host_import.store(
                static_cast<u32>(std::strtoul(value.c_str(), nullptr, 10)),
                std::memory_order_relaxed);
        } else if (key == "profile_stacks") {
            tuning.profile_stacks.store(value != "0", std::memory_order_relaxed);
        } else if (key == "recorder_spin_us") {
            tuning.recorder_spin_us.store(
                static_cast<u32>(std::min<u64>(std::strtoull(value.c_str(), nullptr, 10), 5000)),
                std::memory_order_relaxed);
        } else if (key == "gpu_sync_frame_end") {
            tuning.gpu_sync_frame_end.store(
                static_cast<u32>(std::min<u64>(std::strtoull(value.c_str(), nullptr, 10), 2)),
                std::memory_order_relaxed);
        } else if (key == "async_readbacks") {
            tuning.async_readbacks.store(value != "0", std::memory_order_relaxed);
        } else if (key == "readback_prefetch") {
            tuning.readback_prefetch.store(value != "0", std::memory_order_relaxed);
        } else if (key == "async_pipelines") {
            tuning.async_pipelines.store(std::min<u32>(std::strtoul(value.c_str(), nullptr, 10), 2),
                                         std::memory_order_relaxed);
        } else if (key == "trace_reads") {
            // "lo-hi" guest range in hex, empty or 0: off.
            char* end{};
            const u64 lo = std::strtoull(value.c_str(), &end, 16);
            u64 hi = 0;
            if (end != value.c_str() && *end == '-') {
                hi = std::strtoull(end + 1, nullptr, 16);
            }
            tuning.trace_reads_lo.store(hi > lo ? lo : 0, std::memory_order_relaxed);
            tuning.trace_reads_hi.store(hi > lo ? hi : 0, std::memory_order_relaxed);
        } else if (key == "trace_draw") {
            tuning.trace_draw.store(std::strtoull(value.c_str(), nullptr, 16),
                                    std::memory_order_relaxed);
        } else if (key == "trace_shader") {
            size_t index = 0;
            const char* text = value.c_str();
            while (index < tuning.trace_shaders.size() && *text != '\0') {
                char* end{};
                const u64 hash = std::strtoull(text, &end, 16);
                if (end == text) {
                    break;
                }
                tuning.trace_shaders[index++].store(hash, std::memory_order_relaxed);
                text = end;
                while (*text == ',' || *text == ' ') {
                    ++text;
                }
            }
            for (; index < tuning.trace_shaders.size(); ++index) {
                tuning.trace_shaders[index].store(0, std::memory_order_relaxed);
            }
        } else if (key == "readback_queue") {
            tuning.readback_queue.store(value != "0", std::memory_order_relaxed);
        } else if (key == "hot_page_faults") {
            tuning.hot_page_faults.store(
                std::min<u32>(std::strtoul(value.c_str(), nullptr, 10), 255),
                std::memory_order_relaxed);
        } else if (key == "hot_page_cooldown_frames") {
            tuning.hot_page_cooldown_frames.store(std::strtoul(value.c_str(), nullptr, 10),
                                                  std::memory_order_relaxed);
        } else if (key == "hot_page_hash") {
            tuning.hot_page_hash.store(value != "0", std::memory_order_relaxed);
        } else if (key == "stream_dedup") {
            tuning.stream_dedup.store(value != "0", std::memory_order_relaxed);
        } else if (key == "stream_static") {
            tuning.stream_static.store(value != "0", std::memory_order_relaxed);
        } else if (key == "gpu_clock_div") {
            tuning.gpu_clock_div.store(
                std::clamp<u32>(std::strtoul(value.c_str(), nullptr, 10), 1, 1000),
                std::memory_order_relaxed);
        } else if (key == "max_gpu_lag_frames") {
            tuning.max_gpu_lag_frames.store(
                std::min<u32>(std::strtoul(value.c_str(), nullptr, 10), 8),
                std::memory_order_relaxed);
        } else if (key == "asc_yield_packets") {
            tuning.asc_yield_packets.store(
                std::min<u32>(std::strtoul(value.c_str(), nullptr, 10), 1000000),
                std::memory_order_relaxed);
        } else if (key == "real_occlusion") {
            tuning.real_occlusion.store(value != "0", std::memory_order_relaxed);
        } else if (key == "lds_barriers_multi_wave") {
            tuning.lds_barriers_multi_wave.store(value != "0", std::memory_order_relaxed);
        } else if (key == "wave_memory_barriers") {
            tuning.wave_memory_barriers.store(value != "0", std::memory_order_relaxed);
        } else if (key == "dump_images") {
            const char* text = value.c_str();
            size_t index = 0;
            while (index < tuning.dump_images.size() && *text != '\0') {
                char* end = nullptr;
                const u64 address = std::strtoull(text, &end, 16);
                if (end == text) {
                    break;
                }
                text = end;
                u64 size = 0;
                if (*text == '/') {
                    const u64 width = std::strtoul(text + 1, &end, 10);
                    u64 height = 0;
                    if (*end == 'x') {
                        height = std::strtoul(end + 1, &end, 10);
                    }
                    size = (width << 32) | height;
                    text = end;
                }
                tuning.dump_images_size[index].store(size, std::memory_order_relaxed);
                tuning.dump_images[index++].store(address, std::memory_order_relaxed);
                while (*text == ',' || *text == ' ') {
                    ++text;
                }
            }
            for (; index < tuning.dump_images.size(); ++index) {
                tuning.dump_images[index].store(0, std::memory_order_relaxed);
                tuning.dump_images_size[index].store(0, std::memory_order_relaxed);
            }
        } else if (key == "dump_after") {
            // hash:address[/WxH][:n], ...
            const char* text = value.c_str();
            size_t index = 0;
            while (index < tuning.dump_after.size() && *text != '\0') {
                char* end = nullptr;
                const u64 shader = std::strtoull(text, &end, 16);
                if (end == text || *end != ':') {
                    break;
                }
                text = end + 1;
                u64 address = 0;
                if (text[0] == 'v' && text[1] == 'b') {
                    // The vertex buffers of the draw (Rasterizer::DumpAfter).
                    address = 1;
                    end = const_cast<char*>(text + 2);
                } else if (text[0] == 's' && text[1] == 'r' && text[2] == 't') {
                    // The buffers bound to the shaders.
                    address = 2;
                    end = const_cast<char*>(text + 3);
                } else {
                    address = std::strtoull(text, &end, 16);
                    if (end == text) {
                        break;
                    }
                }
                text = end;
                u32 width = 0, height = 0, count = 1;
                if (*text == '/') {
                    width = std::strtoul(text + 1, &end, 10);
                    if (*end == 'x') {
                        height = std::strtoul(end + 1, &end, 10);
                    }
                    text = end;
                }
                if (*text == ':') {
                    count = std::strtoul(text + 1, &end, 10);
                    text = end;
                }
                auto& entry = tuning.dump_after[index++];
                entry.shader.store(shader, std::memory_order_relaxed);
                entry.address.store(address, std::memory_order_relaxed);
                entry.width.store(width, std::memory_order_relaxed);
                entry.height.store(height, std::memory_order_relaxed);
                entry.count.store(count, std::memory_order_relaxed);
                while (*text == ',' || *text == ' ') {
                    ++text;
                }
            }
            for (; index < tuning.dump_after.size(); ++index) {
                tuning.dump_after[index].shader.store(0, std::memory_order_relaxed);
            }
        } else if (key == "dump_frames") {
            tuning.dump_frames.store(std::strtoul(value.c_str(), nullptr, 10),
                                     std::memory_order_relaxed);
        } else if (key == "stream_static_frames") {
            tuning.stream_static_frames.store(
                std::min<u32>(std::strtoul(value.c_str(), nullptr, 10), 1000),
                std::memory_order_relaxed);
        } else if (key == "gpu_checkpoints") {
            tuning.gpu_checkpoints.store(value != "0", std::memory_order_relaxed);
        } else if (key == "async_pipeline_wait_ms") {
            const u64 wait_ms = std::min<u64>(std::strtoull(value.c_str(), nullptr, 10), 1000);
            tuning.async_pipeline_wait_us.store(static_cast<u32>(wait_ms * 1000),
                                                std::memory_order_relaxed);
        }
    }
    LOG_INFO(Debug,
             "Perf tuning applied: readback_window_kb={} readback_write_discard={} "
             "readback_write_merge={} readback_keep_ranges={} async_readbacks={} readback_prefetch={} async_pipelines={} "
             "async_pipeline_wait_ms={} gpu_checkpoints={} hot_page_faults={} "
             "hot_page_cooldown_frames={} hot_page_hash={} readback_queue={} stream_dedup={} "
             "stream_static={} stream_static_frames={} max_gpu_lag_frames={} gpu_clock_div={} "
             "asc_yield_packets={} real_occlusion={} lds_barriers_multi_wave={} (at startup) "
             "wave_memory_barriers={} (at startup) "
             "threaded_recording={} gpu_sync_frame_end={} async_uploads={} recorder_spin_us={} "
             "profile_stacks={} sync_memo={} spec_memo={} rt_size_learn={} gpu_clock_frame_us={} "
             "region_summary={} tex_memo={} async_written_uploads={} scout={} "
             "skip_redundant_binds={} shader_params_memo={} skip_empty_draws={} "
             "cpu_affinity={:#x} scout_fast={} pipeline_key_memo={} bind_prefetch={} "
             "gpu_threads_affinity={:#x} other_threads_affinity={:#x} rt_memo={} "
             "late_protect={} parallel_protect={} gpu_threads_priority={} gpu_profile={} "
             "readback_relaxed_sites={} asc_queue={} runtime_lookup_memo={} "
             "compute_compile_parallel={}",
             tuning.readback_window_bytes.load(std::memory_order_relaxed) / 1024,
             tuning.readback_write_discard.load(std::memory_order_relaxed),
             tuning.readback_write_merge.load(std::memory_order_relaxed),
             tuning.readback_keep_ranges.load(std::memory_order_relaxed),
             tuning.async_readbacks.load(std::memory_order_relaxed),
             tuning.readback_prefetch.load(std::memory_order_relaxed),
             tuning.async_pipelines.load(std::memory_order_relaxed),
             tuning.async_pipeline_wait_us.load(std::memory_order_relaxed) / 1000,
             tuning.gpu_checkpoints.load(std::memory_order_relaxed),
             tuning.hot_page_faults.load(std::memory_order_relaxed),
             tuning.hot_page_cooldown_frames.load(std::memory_order_relaxed),
             tuning.hot_page_hash.load(std::memory_order_relaxed),
             tuning.readback_queue.load(std::memory_order_relaxed),
             tuning.stream_dedup.load(std::memory_order_relaxed),
             tuning.stream_static.load(std::memory_order_relaxed),
             tuning.stream_static_frames.load(std::memory_order_relaxed),
             tuning.max_gpu_lag_frames.load(std::memory_order_relaxed),
             tuning.gpu_clock_div.load(std::memory_order_relaxed),
             tuning.asc_yield_packets.load(std::memory_order_relaxed),
             tuning.real_occlusion.load(std::memory_order_relaxed),
             tuning.lds_barriers_multi_wave.load(std::memory_order_relaxed),
             tuning.wave_memory_barriers.load(std::memory_order_relaxed),
             tuning.threaded_recording.load(std::memory_order_relaxed),
             tuning.gpu_sync_frame_end.load(std::memory_order_relaxed),
             tuning.async_uploads.load(std::memory_order_relaxed),
             tuning.recorder_spin_us.load(std::memory_order_relaxed),
             tuning.profile_stacks.load(std::memory_order_relaxed),
             tuning.sync_memo.load(std::memory_order_relaxed),
             tuning.spec_memo.load(std::memory_order_relaxed),
             tuning.rt_size_learn.load(std::memory_order_relaxed),
             tuning.gpu_clock_frame_us.load(std::memory_order_relaxed),
             tuning.region_summary.load(std::memory_order_relaxed),
             tuning.tex_memo.load(std::memory_order_relaxed),
             tuning.async_written_uploads.load(std::memory_order_relaxed),
             tuning.scout.load(std::memory_order_relaxed),
             tuning.skip_redundant_binds.load(std::memory_order_relaxed),
             tuning.shader_params_memo.load(std::memory_order_relaxed),
             tuning.skip_empty_draws.load(std::memory_order_relaxed),
             tuning.cpu_affinity.load(std::memory_order_relaxed),
             tuning.scout_fast.load(std::memory_order_relaxed),
             tuning.pipeline_key_memo.load(std::memory_order_relaxed),
             tuning.bind_prefetch.load(std::memory_order_relaxed),
             tuning.gpu_threads_affinity.load(std::memory_order_relaxed),
             tuning.other_threads_affinity.load(std::memory_order_relaxed),
             tuning.rt_memo.load(std::memory_order_relaxed),
             tuning.late_protect.load(std::memory_order_relaxed),
             tuning.parallel_protect.load(std::memory_order_relaxed),
             tuning.gpu_threads_priority.load(std::memory_order_relaxed),
             tuning.gpu_profile.load(std::memory_order_relaxed), [&tuning] {
                 if (tuning.relaxed_all.load(std::memory_order_relaxed)) {
                     return std::string{"all"};
                 }
                 std::string sites;
                 const u32 count = tuning.num_relaxed_sites.load(std::memory_order_relaxed);
                 for (u32 i = 0; i < count; ++i) {
                     char text[24];
                     std::snprintf(text, sizeof(text), "%s%llx", i ? "," : "",
                                   static_cast<unsigned long long>(
                                       tuning.relaxed_sites[i].load(std::memory_order_relaxed)));
                     sites += text;
                 }
                 return sites.empty() ? std::string{"none"} : sites;
             }(),
             tuning.asc_queue.load(std::memory_order_relaxed),
             tuning.runtime_lookup_memo.load(std::memory_order_relaxed),
             tuning.compute_compile_parallel.load(std::memory_order_relaxed));
}

namespace {

struct ReadbackSite {
    u64 page;
    u64 rip;
    bool operator==(const ReadbackSite&) const = default;
};

struct ReadbackSiteHash {
    size_t operator()(const ReadbackSite& site) const {
        return std::hash<u64>{}(site.page * 0x9E3779B97F4A7C15ULL ^ site.rip);
    }
};

std::mutex g_readback_mutex;
std::unordered_map<ReadbackSite, u64, ReadbackSiteHash> g_readback_sites;

} // namespace

void RecordReadback(unsigned long long address, unsigned long long rip) {
    std::scoped_lock lock{g_readback_mutex};
    if (g_readback_sites.size() < 65536) {
        ++g_readback_sites[ReadbackSite{address >> 12, rip}];
    }
}

/// Returns and clears the recorded readback sites, busiest first.
static std::vector<std::pair<u64, ReadbackSite>> TakeReadbackSites() {
    std::unordered_map<ReadbackSite, u64, ReadbackSiteHash> sites;
    {
        std::scoped_lock lock{g_readback_mutex};
        sites.swap(g_readback_sites);
    }
    std::vector<std::pair<u64, ReadbackSite>> sorted;
    sorted.reserve(sites.size());
    for (const auto& [site, count] : sites) {
        sorted.emplace_back(count, site);
    }
    std::sort(sorted.begin(), sorted.end(),
              [](const auto& a, const auto& b) { return a.first > b.first; });
    return sorted;
}

} // namespace Core::PerfSampler

#ifdef _WIN32

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>
#include <fmt/format.h>

#include <windows.h>

// Needs the types from windows.h.
#include <tlhelp32.h>

#include "common/logging/log.h"
#include "common/memory_patcher.h"
#include "common/perf_stats.h"
#include "common/singleton.h"
#include "common/thread.h"
#include "common/types.h"
#include "core/linker.h"

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

namespace Core::PerfSampler {

namespace {

using Clock = std::chrono::steady_clock;

/// Threads above this CPU usage (percent of one core) are profiled, busiest first.
constexpr double ProfileMinCpu = 25.0;
constexpr size_t MaxProfiledThreads = 6;
constexpr size_t ThreadsPerReportLine = 8;
constexpr auto ReportInterval = std::chrono::seconds{20};
constexpr size_t TopFunctions = 15;
constexpr size_t TopCallers = 8;
/// Functions outside the emulator (DLLs) whose callers in the emulator are reported.
constexpr size_t TopDllFunctions = 6;
constexpr size_t CallersPerDllFunction = 4;
/// Stack bytes scanned for a return address into the emulator when a sample lands in a DLL.
constexpr size_t StackScanWords = 128;
/// End of the guest address space on Windows.
constexpr u64 GuestAddressEnd = 0x6000'0000'0000ULL;
/// Emulator threads (named shadPS4:...): stack bytes copied at each sample to walk their call
/// stack, zeroed margins around the copy (reads of an unwinding step past the copy stay in the
/// buffer), and the frames walked at most.
constexpr size_t DeepStackBytes = 16 * 1024;
constexpr size_t DeepStackMarginBefore = 4 * 1024;
constexpr size_t DeepStackMarginAfter = 64 * 1024;
constexpr size_t MaxStackFrames = 48;
constexpr size_t TopInclusive = 30;
constexpr size_t TopPaths = 15;
/// Busiest emulator functions whose hottest instructions are reported, and how many each.
constexpr size_t HotFunctions = 8;
constexpr size_t HotOffsets = 8;

std::jthread g_thread;

u64 FileTimeTo100ns(const FILETIME& time) {
    return (static_cast<u64>(time.dwHighDateTime) << 32) | time.dwLowDateTime;
}

std::string ToUtf8(const wchar_t* text) {
    const int size = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
    if (size <= 1) {
        return {};
    }
    std::string result(static_cast<size_t>(size - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text, -1, result.data(), size, nullptr, nullptr);
    return result;
}

struct ThreadInfo {
    HANDLE handle{};
    std::string name;
    u64 last_total{}; // kernel + user, 100 ns units
    u64 last_kernel{};
    bool has_baseline{};
    double cpu{}; // percent of one core
    double kernel{};
    bool seen{};
    u64 affinity{};         // gpu_threads_affinity: the mask set on the thread, 0 if none
    u64 affinity_process{}; // the process mask at that time (a process change resets it)
    u32 priority{};         // gpu_threads_priority: the level set on the thread, 0 if none
};

/**
 * Bytes above the stack pointer (or above the frame register, minus its offset) that unwinding a
 * frame of this function may read, from its unwind codes. Returns false when they cannot be
 * known (then the stack is not walked further: a read outside the copied stack would fault).
 */
bool FrameExtent(u64 image_base, const RUNTIME_FUNCTION* function, u64& extent,
                 u32& frame_register, u32& frame_offset) {
    extent = 8; // Return address.
    frame_register = 0;
    frame_offset = 0;
    for (u32 chain = 0; chain < 8; ++chain) {
        const auto* info = reinterpret_cast<const u8*>(image_base + function->UnwindData);
        const u32 version = info[0] & 7;
        const u32 flags = info[0] >> 3;
        const u32 count = info[2];
        if (version != 1) {
            return false; // Version 2 epilog codes are not parsed here.
        }
        if ((info[3] & 0xF) != 0) {
            frame_register = info[3] & 0xF;
            frame_offset = (info[3] >> 4) * 16;
        }
        const auto* codes = reinterpret_cast<const u16*>(info + 4);
        for (u32 i = 0; i < count;) {
            const u32 op = (codes[i] >> 8) & 0xF;
            const u32 op_info = codes[i] >> 12;
            switch (op) {
            case 0: // UWOP_PUSH_NONVOL
                extent += 8;
                i += 1;
                break;
            case 1: // UWOP_ALLOC_LARGE
                if (op_info == 0) {
                    extent += u64{codes[i + 1]} * 8;
                    i += 2;
                } else {
                    extent += u64{codes[i + 1]} | (u64{codes[i + 2]} << 16);
                    i += 3;
                }
                break;
            case 2: // UWOP_ALLOC_SMALL
                extent += u64{op_info} * 8 + 8;
                i += 1;
                break;
            case 3: // UWOP_SET_FPREG
                i += 1;
                break;
            case 4: // UWOP_SAVE_NONVOL
                extent = std::max<u64>(extent, u64{codes[i + 1]} * 8 + 8);
                i += 2;
                break;
            case 5: // UWOP_SAVE_NONVOL_FAR
                extent = std::max<u64>(extent, (u64{codes[i + 1]} | (u64{codes[i + 2]} << 16)) + 8);
                i += 3;
                break;
            case 8: // UWOP_SAVE_XMM128
                extent = std::max<u64>(extent, u64{codes[i + 1]} * 16 + 16);
                i += 2;
                break;
            case 9: // UWOP_SAVE_XMM128_FAR
                extent =
                    std::max<u64>(extent, (u64{codes[i + 1]} | (u64{codes[i + 2]} << 16)) + 16);
                i += 3;
                break;
            case 10: // UWOP_PUSH_MACHFRAME
                extent += op_info != 0 ? 48 : 40;
                i += 1;
                break;
            default:
                return false;
            }
        }
        if ((flags & UNW_FLAG_CHAININFO) == 0) {
            break;
        }
        function = reinterpret_cast<const RUNTIME_FUNCTION*>(codes + ((count + 1) & ~1U));
    }
    // The saved registers and the return address are above the allocations.
    extent += 64;
    return true;
}

/**
 * Walks the call stack of a sampled thread from its registers and a copy of its stack taken while
 * it was suspended: stack_bytes copied from its stack pointer to copy, with zeroed margins before
 * and after. Every read stays in the buffer (frames that could read outside end the walk).
 * Writes the start address of the function of each frame, innermost (sampled) first, and returns
 * how many.
 */
size_t WalkStackCopy(const CONTEXT& sampled, const u8* copy, size_t stack_bytes, u64* starts,
                     size_t max_frames) {
    CONTEXT ctx = sampled;
    const u64 real_lo = sampled.Rsp;
    const u64 real_hi = real_lo + stack_bytes;
    const u64 copy_lo = reinterpret_cast<u64>(copy);
    const u64 copy_hi = copy_lo + stack_bytes;
    DWORD64* const regs = &ctx.Rax; // Rax..R15 in register number order.
    const auto translate = [&] {
        for (u32 r = 0; r < 16; ++r) {
            if (regs[r] >= real_lo && regs[r] < real_hi) {
                regs[r] = regs[r] - real_lo + copy_lo;
            }
        }
    };
    translate();
    size_t count = 0;
    while (count < max_frames) {
        const u64 pc = ctx.Rip;
        if (pc == 0 || ctx.Rsp < copy_lo || ctx.Rsp + 8 > copy_hi) {
            break;
        }
        DWORD64 image_base = 0;
        const auto* function = RtlLookupFunctionEntry(pc, &image_base, nullptr);
        starts[count++] = function != nullptr ? image_base + function->BeginAddress : pc;
        if (function == nullptr) {
            if (pc < GuestAddressEnd) {
                break; // Guest code: no unwind data.
            }
            // Leaf function: the return address is at the stack pointer.
            ctx.Rip = *reinterpret_cast<const u64*>(ctx.Rsp);
            ctx.Rsp += 8;
            continue;
        }
        u64 extent = 0;
        u32 frame_register = 0;
        u32 frame_offset = 0;
        if (!FrameExtent(image_base, function, extent, frame_register, frame_offset)) {
            break;
        }
        const u64 limit = copy_hi + DeepStackMarginAfter;
        if (ctx.Rsp + extent > limit) {
            break;
        }
        if (frame_register != 0) {
            // The frame register may be where unwinding reads: it must point into the buffer.
            const u64 frame = regs[frame_register];
            if (frame < frame_offset || frame - frame_offset < copy_lo - DeepStackMarginBefore ||
                frame - frame_offset + extent > limit) {
                break;
            }
        }
        PVOID handler_data = nullptr;
        DWORD64 establisher_frame = 0;
        RtlVirtualUnwind(UNW_FLAG_NHANDLER, image_base, pc, const_cast<RUNTIME_FUNCTION*>(function),
                         &ctx, &handler_data, &establisher_frame, nullptr);
        translate();
    }
    return count;
}

struct Profile {
    u64 samples{};
    std::unordered_map<u64, u32> addresses;
    /// Emulator threads: samples in which each emulator function was on the call stack
    /// (inclusive time), and the three innermost emulator functions of each sample.
    u64 stack_samples{};
    std::unordered_map<u64, u32> inclusive;
    std::map<std::array<u64, 3>, u32> paths;
    /// For samples outside the emulator code: first return address into the emulator.
    std::unordered_map<u64, u32> callers;
    /// Same, with the sampled address: which emulator functions call each DLL function.
    std::map<std::pair<u64, u64>, u32> dll_callers;
};

/// Exported functions of a DLL, sorted by address, to name its hot spots.
struct ModuleExports {
    std::vector<std::pair<u32, std::string>> exports;
};

class Sampler {
public:
    Sampler() {
        exe_base = reinterpret_cast<u64>(GetModuleHandleW(nullptr));
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(exe_base);
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(exe_base + dos->e_lfanew);
        const auto* section = IMAGE_FIRST_SECTION(nt);
        for (u32 i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++section) {
            if ((section->Characteristics & IMAGE_SCN_MEM_EXECUTE) != 0) {
                text_begin = exe_base + section->VirtualAddress;
                text_end = text_begin + section->Misc.VirtualSize;
                break;
            }
        }
        self_tid = GetCurrentThreadId();
    }

    ~Sampler() {
        for (auto& [tid, info] : threads) {
            if (info.handle != nullptr) {
                CloseHandle(info.handle);
            }
        }
    }

    void Run(const std::stop_token& stop) {
        Common::SetCurrentThreadName("shadPS4:PerfSampler");
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
        HANDLE timer = CreateWaitableTimerExW(
            nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
        const auto reports = [] {
            return Common::Perf::GetTuning().perf_log.load(std::memory_order_relaxed);
        };
        if (reports()) {
            LOG_INFO(Debug,
                     "Perf sampler started: thread CPU usage every second, profile of threads "
                     "above {:.0f}% CPU every {} s (high resolution timer: {})",
                     ProfileMinCpu, ReportInterval.count(), timer != nullptr);
        }

        auto last_refresh = Clock::now();
        auto last_report = last_refresh;
        auto last_affinity = last_refresh;
        LogCacheTopology();
        auto_gpu_mask = AutoGpuThreadsMask();
        RefreshThreads(0.0);
        while (!stop.stop_requested()) {
            // Without reports, only the thread affinities and priorities are kept up to date.
            const bool reporting = reports();
            if (reporting) {
                WaitOneMillisecond(timer);
                SampleTargets();
            } else {
                Sleep(50);
            }

            const auto now = Clock::now();
            if (now - last_affinity >= std::chrono::milliseconds{200}) {
                ApplyAffinity();
                last_affinity = now;
            }
            if (now - last_refresh >= std::chrono::seconds{1}) {
                RefreshThreads(std::chrono::duration<double>(now - last_refresh).count());
                last_refresh = now;
            }
            if (now - last_report >= ReportInterval) {
                if (reporting) {
                    Report(std::chrono::duration<double>(now - last_report).count());
                }
                last_report = now;
            }
        }
        if (timer != nullptr) {
            CloseHandle(timer);
        }
    }

private:
    /// "0-7,16-23" for the bits of a processor mask.
    static std::string MaskText(u64 mask) {
        std::string text;
        for (u32 bit = 0; bit < 64;) {
            if (((mask >> bit) & 1) == 0) {
                ++bit;
                continue;
            }
            u32 end = bit;
            while (end + 1 < 64 && ((mask >> (end + 1)) & 1) != 0) {
                ++end;
            }
            if (!text.empty()) {
                text += ',';
            }
            text += end == bit ? fmt::format("{}", bit) : fmt::format("{}-{}", bit, end);
            bit = end + 1;
        }
        return text.empty() ? "none" : text;
    }

    /// The logical processors of each L3 cache (on CPUs with two dies, such as a Ryzen with 3D
    /// V-Cache on one of them, the dies have their own L3).
    /// Logical processors for gpu_threads_affinity = auto (see Common::Perf::AffinityAuto): on
    /// a CPU whose L3 caches differ in size, one logical processor of each core sharing the
    /// largest L3, except the first of these cores; 0 (no pinning) elsewhere.
    static u64 AutoGpuThreadsMask() {
        const auto read = [](LOGICAL_PROCESSOR_RELATIONSHIP relation) {
            std::vector<u8> buffer;
            DWORD length = 0;
            GetLogicalProcessorInformationEx(relation, nullptr, &length);
            if (length == 0) {
                return buffer;
            }
            buffer.resize(length);
            if (!GetLogicalProcessorInformationEx(
                    relation,
                    reinterpret_cast<SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(buffer.data()),
                    &length)) {
                buffer.clear();
            }
            buffer.resize(length);
            return buffer;
        };
        const auto for_each = [](const std::vector<u8>& buffer, auto&& visit) {
            for (size_t offset = 0; offset + sizeof(DWORD) * 2 <= buffer.size();) {
                const auto* entry = reinterpret_cast<const SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(
                    buffer.data() + offset);
                if (entry->Size == 0) {
                    break;
                }
                visit(*entry);
                offset += entry->Size;
            }
        };
        // L3 caches of processor group 0 (the emulator does not use more than 64 processors).
        std::vector<std::pair<u64, u64>> l3; // (size, mask)
        for_each(read(RelationCache), [&](const SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX& entry) {
            if (entry.Cache.Level == 3 && entry.Cache.GroupMask.Group == 0) {
                l3.emplace_back(entry.Cache.CacheSize,
                                static_cast<u64>(entry.Cache.GroupMask.Mask));
            }
        });
        if (l3.size() < 2) {
            return 0;
        }
        std::ranges::sort(l3, std::greater<>{});
        if (l3[0].first == l3[1].first) {
            return 0; // Same L3 on every die: nothing to choose.
        }
        const u64 domain = l3[0].second;
        std::vector<u64> core_first; // Lowest logical processor of each core of that L3.
        for_each(read(RelationProcessorCore),
                 [&](const SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX& entry) {
                     if (entry.Processor.GroupCount == 0 ||
                         entry.Processor.GroupMask[0].Group != 0) {
                         return;
                     }
                     const u64 core = static_cast<u64>(entry.Processor.GroupMask[0].Mask);
                     if (core != 0 && (core & domain) == core) {
                         core_first.push_back(core & (~core + 1));
                     }
                 });
        std::ranges::sort(core_first);
        u64 mask = 0;
        for (size_t i = 1; i < core_first.size(); ++i) {
            mask |= core_first[i];
        }
        LOG_INFO(Debug, "Perf CPU: threads translating the graphics commands on logical "
                        "processors {} (largest L3: {} MB)",
                 MaskText(mask), l3[0].first >> 20);
        return mask;
    }

    static void LogCacheTopology() {
        DWORD length = 0;
        GetLogicalProcessorInformationEx(RelationCache, nullptr, &length);
        if (length == 0) {
            return;
        }
        std::vector<u8> buffer(length);
        if (!GetLogicalProcessorInformationEx(
                RelationCache,
                reinterpret_cast<SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(buffer.data()),
                &length)) {
            return;
        }
        for (DWORD offset = 0; offset + sizeof(DWORD) * 2 <= length;) {
            const auto* entry =
                reinterpret_cast<const SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(buffer.data() +
                                                                                 offset);
            if (entry->Size == 0) {
                break;
            }
            if (entry->Relationship == RelationCache && entry->Cache.Level == 3) {
                LOG_INFO(Debug, "Perf CPU: L3 cache of {} MB for logical processors {} (group {})",
                         entry->Cache.CacheSize >> 20,
                         MaskText(static_cast<u64>(entry->Cache.GroupMask.Mask)),
                         entry->Cache.GroupMask.Group);
            }
            offset += entry->Size;
        }
    }

    /// Process affinity from perf_tuning.ini (cpu_affinity, a hexadecimal mask of logical
    /// processors, 0 for all of them), applied when it changes.
    void ApplyAffinity() {
        const u64 wanted = Common::Perf::GetTuning().cpu_affinity.load(std::memory_order_relaxed);
        if (wanted == applied_affinity) {
            return;
        }
        DWORD_PTR process_mask = 0;
        DWORD_PTR system_mask = 0;
        if (!GetProcessAffinityMask(GetCurrentProcess(), &process_mask, &system_mask)) {
            return;
        }
        if (original_affinity == 0) original_affinity = process_mask;
        const DWORD_PTR mask = wanted ? static_cast<DWORD_PTR>(wanted) & system_mask
                                      : original_affinity & system_mask;
        if (mask == 0) {
            LOG_WARNING(Debug, "Perf CPU: affinity {:#x} has no processor of this machine",
                        wanted);
            return;
        }
        if (mask == process_mask) {
            applied_affinity = wanted;
            return;
        }
        if (SetProcessAffinityMask(GetCurrentProcess(), mask)) {
            applied_affinity = wanted;
            // Windows can retain a thread's narrower mask when the process mask expands.
            // Invalidate our own affinity cache so wanted=0 really restores the process mask.
            // Ordinary launches (no explicit process change) keep existing thread affinities.
            for (auto& [tid, info] : threads) {
                info.affinity_process = 0;
                if (info.affinity == 0) info.affinity = Common::Perf::AffinityAuto;
            }
            LOG_INFO(Debug, "Perf CPU: process affinity set to logical processors {}",
                     MaskText(static_cast<u64>(mask)));
        } else {
            LOG_WARNING(Debug, "Perf CPU: process affinity {} refused (error {})",
                        MaskText(static_cast<u64>(mask)), GetLastError());
        }
    }

    u64 applied_affinity{};
    u64 original_affinity{};
    u64 auto_gpu_mask{};       ///< gpu_threads_affinity = auto, see AutoGpuThreadsMask.
    u32 other_threads_moved{}; ///< Threads given other_threads_affinity since the last log.

    /// The emulator threads translating the graphics commands (gpu_threads_affinity).
    static bool IsGpuThread(const std::string& name) {
        return name == "shadPS4:GpuCommandProcessor" || name == "shadPS4:GpuCommandRecorder" ||
               name == "shadPS4:Scout" || name == "shadPS4:GpuUploadWorker";
    }

    /// Priority of the threads translating the graphics commands from perf_tuning.ini
    /// (gpu_threads_priority: 0 unchanged, 1 above normal, 2 highest), so that other threads of
    /// the machine do not take their processor. Applied when it changes and to new threads.
    void ApplyThreadPriority(ThreadInfo& info, DWORD tid) {
        if (!IsGpuThread(info.name)) {
            return;
        }
        const u32 wanted = std::min<u32>(
            Common::Perf::GetTuning().gpu_threads_priority.load(std::memory_order_relaxed), 2);
        if (wanted == info.priority) {
            return;
        }
        const HANDLE handle = OpenThread(
            THREAD_SET_LIMITED_INFORMATION | THREAD_QUERY_LIMITED_INFORMATION, FALSE, tid);
        if (handle == nullptr) {
            return;
        }
        static constexpr std::array<int, 3> Levels{THREAD_PRIORITY_NORMAL,
                                                   THREAD_PRIORITY_ABOVE_NORMAL,
                                                   THREAD_PRIORITY_HIGHEST};
        if (SetThreadPriority(handle, Levels[wanted])) {
            info.priority = wanted;
            LOG_INFO(Debug, "Perf CPU: thread {} priority {}", info.name,
                     wanted == 0 ? "normal" : wanted == 1 ? "above normal" : "highest");
        } else {
            LOG_WARNING(Debug, "Perf CPU: thread {} priority refused (error {})", info.name,
                        GetLastError());
            info.priority = wanted; // Not retried every second.
        }
        CloseHandle(handle);
    }

    /// Thread affinity from perf_tuning.ini (hexadecimal masks of logical processors, 0 for those
    /// of the process): gpu_threads_affinity for the threads translating the graphics commands,
    /// other_threads_affinity for the others (game threads...). Applied when it changes, to new
    /// threads, and again after a process affinity change (which resets them).
    void ApplyThreadAffinity(ThreadInfo& info, DWORD tid) {
        const auto& tuning = Common::Perf::GetTuning();
        u64 wanted = IsGpuThread(info.name)
                         ? tuning.gpu_threads_affinity.load(std::memory_order_relaxed)
                         : tuning.other_threads_affinity.load(std::memory_order_relaxed);
        if (wanted == Common::Perf::AffinityAuto) {
            wanted = IsGpuThread(info.name) ? auto_gpu_mask : 0;
        }
        if (wanted == 0 && info.affinity == 0) {
            return;
        }
        DWORD_PTR process_mask = 0;
        DWORD_PTR system_mask = 0;
        if (!GetProcessAffinityMask(GetCurrentProcess(), &process_mask, &system_mask)) {
            return;
        }
        const DWORD_PTR mask =
            wanted != 0 ? static_cast<DWORD_PTR>(wanted) & process_mask : process_mask;
        const u64 applied = wanted != 0 ? static_cast<u64>(mask) : 0;
        if (applied == info.affinity && static_cast<u64>(process_mask) == info.affinity_process) {
            return;
        }
        info.affinity = applied;
        info.affinity_process = static_cast<u64>(process_mask);
        if (mask == 0) {
            LOG_WARNING(Debug, "Perf CPU: thread affinity {:#x} has no processor of the process",
                        wanted);
            return;
        }
        const HANDLE handle = OpenThread(
            THREAD_SET_LIMITED_INFORMATION | THREAD_QUERY_LIMITED_INFORMATION, FALSE, tid);
        if (handle == nullptr) {
            return;
        }
        if (SetThreadAffinityMask(handle, mask) != 0) {
            if (IsGpuThread(info.name)) {
                LOG_INFO(Debug, "Perf CPU: thread {} on logical processors {}", info.name,
                         MaskText(static_cast<u64>(mask)));
            } else {
                ++other_threads_moved;
            }
        } else {
            LOG_WARNING(Debug, "Perf CPU: thread {} affinity {} refused (error {})", info.name,
                        MaskText(static_cast<u64>(mask)), GetLastError());
        }
        CloseHandle(handle);
    }


    static void WaitOneMillisecond(HANDLE timer) {
        if (timer != nullptr) {
            LARGE_INTEGER due{};
            due.QuadPart = -10'000; // relative, 100 ns units
            if (SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE)) {
                WaitForSingleObject(timer, INFINITE);
                return;
            }
        }
        Sleep(1);
    }

    void RefreshThreads(double interval_s) {
        for (auto& [tid, info] : threads) {
            info.seen = false;
        }
        const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        if (snapshot != INVALID_HANDLE_VALUE) {
            const DWORD pid = GetCurrentProcessId();
            THREADENTRY32 entry{};
            entry.dwSize = sizeof(entry);
            for (BOOL ok = Thread32First(snapshot, &entry); ok;
                 ok = Thread32Next(snapshot, &entry)) {
                if (entry.th32OwnerProcessID != pid || entry.th32ThreadID == self_tid) {
                    continue;
                }
                auto [it, inserted] = threads.try_emplace(entry.th32ThreadID);
                it->second.seen = true;
                if (inserted) {
                    it->second.handle = OpenThread(THREAD_QUERY_LIMITED_INFORMATION |
                                                       THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT,
                                                   FALSE, entry.th32ThreadID);
                }
            }
            CloseHandle(snapshot);
        }
        for (auto it = threads.begin(); it != threads.end();) {
            if (!it->second.seen) {
                if (it->second.handle != nullptr) {
                    CloseHandle(it->second.handle);
                }
                it = threads.erase(it);
            } else {
                ++it;
            }
        }

        std::vector<std::pair<double, DWORD>> by_cpu;
        by_cpu.reserve(threads.size());
        for (auto& [tid, info] : threads) {
            if (info.handle == nullptr) {
                continue;
            }
            FILETIME creation, exit, kernel, user;
            if (!GetThreadTimes(info.handle, &creation, &exit, &kernel, &user)) {
                continue;
            }
            const u64 kernel_time = FileTimeTo100ns(kernel);
            const u64 total_time = kernel_time + FileTimeTo100ns(user);
            if (info.has_baseline && interval_s > 0.0) {
                // 100 ns units to percent of one core.
                info.cpu = static_cast<double>(total_time - info.last_total) / (interval_s * 1e5);
                info.kernel =
                    static_cast<double>(kernel_time - info.last_kernel) / (interval_s * 1e5);
            }
            info.last_total = total_time;
            info.last_kernel = kernel_time;
            info.has_baseline = true;

            PWSTR description = nullptr;
            if (SUCCEEDED(GetThreadDescription(info.handle, &description)) &&
                description != nullptr) {
                info.name = ToUtf8(description);
                LocalFree(description);
            }
            if (info.name.empty()) {
                info.name = fmt::format("tid {}", tid);
            }
            ApplyThreadAffinity(info, tid);
            ApplyThreadPriority(info, tid);
            by_cpu.emplace_back(info.cpu, tid);
        }
        std::sort(by_cpu.begin(), by_cpu.end(), std::greater<>{});

        if (interval_s > 0.0 &&
            Common::Perf::GetTuning().perf_log.load(std::memory_order_relaxed)) {
            std::string line;
            for (size_t i = 0; i < by_cpu.size() && i < ThreadsPerReportLine; ++i) {
                const auto& info = threads[by_cpu[i].second];
                if (info.cpu < 1.0) {
                    break;
                }
                line += fmt::format("{}{} {:.0f}% (kernel {:.0f}%)", line.empty() ? "" : " | ",
                                    info.name, info.cpu, info.kernel);
            }
            LOG_INFO(Debug, "Perf threads: {}", line);
        }
        if (other_threads_moved != 0) {
            LOG_INFO(Debug, "Perf CPU: {} other threads on logical processors {}",
                     other_threads_moved,
                     MaskText(Common::Perf::GetTuning().other_threads_affinity.load(
                         std::memory_order_relaxed)));
            other_threads_moved = 0;
        }

        targets.clear();
        for (const auto& [cpu, tid] : by_cpu) {
            if (targets.size() >= MaxProfiledThreads || cpu < ProfileMinCpu) {
                break;
            }
            targets.push_back(tid);
        }
    }

    void SampleTargets() {
        for (const DWORD tid : targets) {
            const auto it = threads.find(tid);
            if (it == threads.end() || it->second.handle == nullptr) {
                continue;
            }
            const HANDLE handle = it->second.handle;

            // Nothing between suspend and resume may take a user mode lock (heap, loader,
            // logger): the suspended thread could be holding it.
            alignas(16) CONTEXT context{};
            context.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
            std::array<u64, StackScanWords> small_stack;
            // Emulator threads: a larger copy, to walk their call stack.
            const bool deep =
                Common::Perf::GetTuning().profile_stacks.load(std::memory_order_relaxed) &&
                it->second.name.starts_with("shadPS4:");
            if (deep && deep_stack.empty()) {
                deep_stack.assign(DeepStackMarginBefore + DeepStackBytes + DeepStackMarginAfter, 0);
            }
            u8* const copy = deep ? deep_stack.data() + DeepStackMarginBefore
                                  : reinterpret_cast<u8*>(small_stack.data());
            const size_t copy_size = deep ? DeepStackBytes : sizeof(small_stack);
            SIZE_T stack_bytes = 0;
            if (SuspendThread(handle) == static_cast<DWORD>(-1)) {
                continue;
            }
            const BOOL has_context = GetThreadContext(handle, &context);
            if (has_context) {
                if (!ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<LPCVOID>(context.Rsp),
                                       copy, copy_size, &stack_bytes)) {
                    // Probably close to the stack base, retry with smaller windows.
                    stack_bytes = 0;
                    for (size_t size = copy_size / 4; size >= sizeof(u64) * 16 && stack_bytes == 0;
                         size /= 4) {
                        if (!ReadProcessMemory(GetCurrentProcess(),
                                               reinterpret_cast<LPCVOID>(context.Rsp), copy, size,
                                               &stack_bytes)) {
                            stack_bytes = 0;
                        }
                    }
                }
            }
            ResumeThread(handle);
            if (!has_context) {
                continue;
            }
            const u64* const stack = reinterpret_cast<const u64*>(copy);

            auto& profile = profiles[it->second.name];
            ++profile.samples;
            ++profile.addresses[context.Rip];
            if (deep && stack_bytes >= 256) {
                // Bytes past the copy read as zeros (stale data of earlier samples otherwise).
                std::memset(copy + stack_bytes, 0, DeepStackBytes - stack_bytes);
                std::array<u64, MaxStackFrames> starts;
                const size_t num_frames =
                    WalkStackCopy(context, copy, stack_bytes, starts.data(), starts.size());
                std::array<u64, 3> path{};
                size_t path_size = 0;
                std::array<u64, MaxStackFrames> seen;
                size_t num_seen = 0;
                for (size_t f = 0; f < num_frames; ++f) {
                    const u64 start = starts[f];
                    if (start < text_begin || start >= text_end) {
                        continue;
                    }
                    if (path_size < path.size() && (path_size == 0 || path[path_size - 1] != start)) {
                        path[path_size++] = start;
                    }
                    if (std::find(seen.begin(), seen.begin() + num_seen, start) ==
                        seen.begin() + num_seen) {
                        seen[num_seen++] = start;
                        ++profile.inclusive[start];
                    }
                }
                ++profile.stack_samples;
                if (path_size != 0) {
                    ++profile.paths[path];
                }
            }
            if (context.Rip >= text_begin && context.Rip < text_end) {
                continue;
            }
            // In a DLL (driver, OS) or guest code: find which emulator function led there.
            for (size_t i = 0; i < std::min<size_t>(stack_bytes / sizeof(u64), StackScanWords);
                 ++i) {
                if (stack[i] >= text_begin && stack[i] < text_end) {
                    ++profile.callers[stack[i]];
                    if (context.Rip >= GuestAddressEnd) {
                        ++profile.dll_callers[{context.Rip, stack[i]}];
                    }
                    break;
                }
            }
        }
    }

    const std::string& ModuleName(HMODULE module) {
        auto [it, inserted] = module_names.try_emplace(module);
        if (inserted) {
            std::array<wchar_t, MAX_PATH> path{};
            const DWORD length = GetModuleFileNameW(module, path.data(), MAX_PATH);
            std::wstring_view view{path.data(), length};
            const size_t slash = view.find_last_of(L"\\/");
            if (slash != std::wstring_view::npos) {
                view.remove_prefix(slash + 1);
            }
            const std::wstring name{view};
            it->second = ToUtf8(name.c_str());
        }
        return it->second;
    }

    const ModuleExports& Exports(HMODULE module) {
        auto [it, inserted] = module_exports.try_emplace(module);
        if (!inserted) {
            return it->second;
        }
        const u64 base = reinterpret_cast<u64>(module);
        if (base == exe_base) {
            // The emulator is symbolized with its map file.
            return it->second;
        }
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
            return it->second;
        }
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE ||
            nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
            return it->second;
        }
        const auto& directory = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
        if (directory.VirtualAddress == 0 || directory.Size == 0) {
            return it->second;
        }
        const auto* exports =
            reinterpret_cast<const IMAGE_EXPORT_DIRECTORY*>(base + directory.VirtualAddress);
        const auto* functions = reinterpret_cast<const DWORD*>(base + exports->AddressOfFunctions);
        const auto* names = reinterpret_cast<const DWORD*>(base + exports->AddressOfNames);
        const auto* ordinals = reinterpret_cast<const WORD*>(base + exports->AddressOfNameOrdinals);
        auto& list = it->second.exports;
        list.reserve(exports->NumberOfNames);
        for (DWORD i = 0; i < exports->NumberOfNames; ++i) {
            if (ordinals[i] >= exports->NumberOfFunctions) {
                continue;
            }
            const DWORD rva = functions[ordinals[i]];
            if (rva >= directory.VirtualAddress && rva < directory.VirtualAddress + directory.Size) {
                // Forwarded to another DLL.
                continue;
            }
            list.emplace_back(rva, reinterpret_cast<const char*>(base + names[i]));
        }
        std::sort(list.begin(), list.end());
        return it->second;
    }

    /// Name of the exported function at or before the address, if close enough.
    std::string ExportName(HMODULE module, u64 rva) {
        const auto& list = Exports(module).exports;
        auto it = std::upper_bound(
            list.begin(), list.end(), rva,
            [](u64 value, const std::pair<u32, std::string>& entry) { return value < entry.first; });
        if (it == list.begin()) {
            return {};
        }
        --it;
        const u64 distance = rva - it->first;
        if (distance == 0) {
            return fmt::format(" ({})", it->second);
        }
        if (distance < 0x4000) {
            return fmt::format(" ({}+{:#x})", it->second, distance);
        }
        return {};
    }

    /// Groups an address by function for host code (unwind data), by 256 bytes for guest code.
    std::string Describe(u64 address) {
        HMODULE module{};
        if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                   GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               reinterpret_cast<LPCWSTR>(address), &module)) {
            const u64 base = reinterpret_cast<u64>(module);
            DWORD64 image_base = 0;
            const auto* function = RtlLookupFunctionEntry(address, &image_base, nullptr);
            const u64 start = function != nullptr ? image_base + function->BeginAddress : address;
            return fmt::format("{}+{:#x}{}", ModuleName(module), start - base,
                               ExportName(module, start - base));
        }
        if (address < GuestAddressEnd) {
            auto* linker = Common::Singleton<Core::Linker>::Instance();
            if (const auto* guest_module = linker->FindByAddress(address)) {
                return fmt::format("guest {}+{:#x}", guest_module->name,
                                   (address - guest_module->GetBaseAddress()) & ~0xFFULL);
            }
            return fmt::format("guest {:#x}", address & ~0xFFFULL);
        }
        return fmt::format("unknown {:#x}", address & ~0xFFFULL);
    }

    void Report(double seconds) {
        // Addresses repeat across threads and tables: describe each one once.
        std::unordered_map<u64, std::string> descriptions;
        const auto describe = [&](u64 address) -> const std::string& {
            auto [it, inserted] = descriptions.try_emplace(address);
            if (inserted) {
                it->second = Describe(address);
            }
            return it->second;
        };
        for (const auto& [name, profile] : profiles) {
            if (profile.samples < 100) {
                continue;
            }
            std::unordered_map<std::string, u64> functions;
            for (const auto& [address, count] : profile.addresses) {
                functions[describe(address)] += count;
            }
            std::vector<std::pair<u64, std::string>> sorted;
            sorted.reserve(functions.size());
            for (auto& [label, count] : functions) {
                sorted.emplace_back(count, label);
            }
            std::sort(sorted.begin(), sorted.end(), std::greater<>{});
            const double total = static_cast<double>(profile.samples);
            LOG_INFO(Debug, "Perf profile [{}]: {} samples in {:.0f} s", name, profile.samples,
                     seconds);
            // Share of each module (the driver records the Vulkan commands, the kernel
            // protects pages...), to know what part of a thread's time could move elsewhere.
            std::unordered_map<std::string, u64> modules;
            for (const auto& [count, label] : sorted) {
                const size_t plus = label.find('+');
                const size_t space = label.find(' ');
                modules[label.substr(0, std::min(plus, space))] += count;
            }
            std::vector<std::pair<u64, std::string>> sorted_modules;
            for (auto& [label, count] : modules) {
                sorted_modules.emplace_back(count, label);
            }
            std::sort(sorted_modules.begin(), sorted_modules.end(), std::greater<>{});
            std::string module_shares;
            for (size_t i = 0; i < sorted_modules.size() && i < 8; ++i) {
                module_shares += fmt::format(
                    "{}{} {:.1f}%", i != 0 ? ", " : "", sorted_modules[i].second,
                    100.0 * static_cast<double>(sorted_modules[i].first) / total);
            }
            LOG_INFO(Debug, "Perf profile [{}] modules: {}", name, module_shares);
            for (size_t i = 0; i < sorted.size() && i < TopFunctions; ++i) {
                LOG_INFO(Debug, "Perf profile [{}] {:5.1f}% {}", name,
                         100.0 * static_cast<double>(sorted[i].first) / total, sorted[i].second);
            }

            // Hot instructions of the busiest emulator functions (offsets from the function
            // start, for a disassembly): which part of a function costs.
            std::unordered_map<u64, std::vector<std::pair<u32, u64>>> by_function;
            std::unordered_map<u64, u64> function_totals;
            for (const auto& [address, count] : profile.addresses) {
                if (address < text_begin || address >= text_end) {
                    continue;
                }
                DWORD64 image_base = 0;
                const auto* function = RtlLookupFunctionEntry(address, &image_base, nullptr);
                const u64 start = function != nullptr ? image_base + function->BeginAddress : address;
                by_function[start].emplace_back(count, address - start);
                function_totals[start] += count;
            }
            std::vector<std::pair<u64, u64>> hot_functions;
            for (const auto& [start, count] : function_totals) {
                hot_functions.emplace_back(count, start);
            }
            std::sort(hot_functions.begin(), hot_functions.end(), std::greater<>{});
            for (size_t i = 0; i < hot_functions.size() && i < HotFunctions; ++i) {
                const auto [function_count, start] = hot_functions[i];
                auto& offsets = by_function[start];
                std::sort(offsets.begin(), offsets.end(), std::greater<>{});
                std::string line;
                for (size_t j = 0; j < offsets.size() && j < HotOffsets; ++j) {
                    line += fmt::format(" +{:#x} {:.0f}%", offsets[j].second,
                                        100.0 * static_cast<double>(offsets[j].first) /
                                            static_cast<double>(function_count));
                }
                LOG_INFO(Debug, "Perf hot [{}] {:5.1f}% {}:{}", name,
                         100.0 * static_cast<double>(function_count) / total, describe(start),
                         line);
            }

            std::unordered_map<std::string, u64> callers;
            for (const auto& [address, count] : profile.callers) {
                callers[describe(address)] += count;
            }
            std::vector<std::pair<u64, std::string>> sorted_callers;
            for (auto& [label, count] : callers) {
                sorted_callers.emplace_back(count, label);
            }
            std::sort(sorted_callers.begin(), sorted_callers.end(), std::greater<>{});
            for (size_t i = 0; i < sorted_callers.size() && i < TopCallers; ++i) {
                LOG_INFO(Debug, "Perf profile [{}] via {:5.1f}% {}", name,
                         100.0 * static_cast<double>(sorted_callers[i].first) / total,
                         sorted_callers[i].second);
            }

            // Which emulator functions call the busiest DLL functions (memcpy, system calls).
            std::unordered_map<std::string, std::unordered_map<std::string, u64>> dll_functions;
            std::unordered_map<std::string, u64> dll_totals;
            for (const auto& [key, count] : profile.dll_callers) {
                const std::string& callee = describe(key.first);
                dll_functions[callee][describe(key.second)] += count;
                dll_totals[callee] += count;
            }
            std::vector<std::pair<u64, std::string>> sorted_dll;
            for (const auto& [label, count] : dll_totals) {
                sorted_dll.emplace_back(count, label);
            }
            std::sort(sorted_dll.begin(), sorted_dll.end(), std::greater<>{});
            for (size_t i = 0; i < sorted_dll.size() && i < TopDllFunctions; ++i) {
                const auto& callee = sorted_dll[i].second;
                std::vector<std::pair<u64, std::string>> from;
                for (const auto& [label, count] : dll_functions[callee]) {
                    from.emplace_back(count, label);
                }
                std::sort(from.begin(), from.end(), std::greater<>{});
                for (size_t j = 0; j < from.size() && j < CallersPerDllFunction; ++j) {
                    LOG_INFO(Debug, "Perf profile [{}] from {:5.1f}% {} -> {}", name,
                             100.0 * static_cast<double>(from[j].first) / total, from[j].second,
                             callee);
                }
            }

            // Call stacks of emulator threads: time spent in each function including what it
            // calls, and the busiest call paths (innermost function first).
            if (profile.stack_samples >= 100) {
                std::unordered_map<std::string, u64> inclusive;
                for (const auto& [start, count] : profile.inclusive) {
                    auto& entry = inclusive[describe(start)];
                    entry = std::max<u64>(entry, count);
                }
                std::vector<std::pair<u64, std::string>> sorted_inclusive;
                for (auto& [label, count] : inclusive) {
                    sorted_inclusive.emplace_back(count, label);
                }
                std::sort(sorted_inclusive.begin(), sorted_inclusive.end(), std::greater<>{});
                LOG_INFO(Debug, "Perf stacks [{}]: {} call stacks walked", name,
                         profile.stack_samples);
                for (size_t i = 0; i < sorted_inclusive.size() && i < TopInclusive; ++i) {
                    LOG_INFO(Debug, "Perf inclusive [{}] {:5.1f}% {}", name,
                             100.0 * static_cast<double>(sorted_inclusive[i].first) / total,
                             sorted_inclusive[i].second);
                }
                std::vector<std::pair<u64, std::array<u64, 3>>> sorted_paths;
                for (const auto& [path, count] : profile.paths) {
                    sorted_paths.emplace_back(count, path);
                }
                std::sort(sorted_paths.begin(), sorted_paths.end(),
                          [](const auto& a, const auto& b) { return a.first > b.first; });
                for (size_t i = 0; i < sorted_paths.size() && i < TopPaths; ++i) {
                    const auto& path = sorted_paths[i].second;
                    LOG_INFO(Debug, "Perf path [{}] {:5.1f}% {} < {} < {}", name,
                             100.0 * static_cast<double>(sorted_paths[i].first) / total,
                             describe(path[0]), path[1] != 0 ? describe(path[1]) : "-",
                             path[2] != 0 ? describe(path[2]) : "-");
                }
            }
        }
        profiles.clear();

        const auto sites = TakeReadbackSites();
        u64 total_readbacks = 0;
        for (const auto& [count, site] : sites) {
            total_readbacks += count;
        }
        if (total_readbacks != 0) {
            LOG_INFO(Debug, "Perf readback sites: {} readbacks, {} distinct (page, instruction)",
                     total_readbacks, sites.size());
            for (size_t i = 0; i < sites.size() && i < 12; ++i) {
                const auto& [count, site] = sites[i];
                LOG_INFO(Debug, "Perf readback site {:5} x  page {:#x}  read by {}", count,
                         site.page << 12, Describe(site.rip));
            }
            // The same by reading code only (all pages), busiest first.
            std::unordered_map<std::string, std::pair<u64, u64>> by_code;
            for (const auto& [count, site] : sites) {
                auto& [code_count, code_pages] = by_code[Describe(site.rip)];
                code_count += count;
                ++code_pages;
            }
            std::vector<std::pair<u64, std::string>> codes;
            for (const auto& [code, stats] : by_code) {
                codes.emplace_back(stats.first, fmt::format("{} ({} pages)", code, stats.second));
            }
            std::sort(codes.begin(), codes.end(), std::greater<>());
            for (size_t i = 0; i < codes.size() && i < 8; ++i) {
                LOG_INFO(Debug, "Perf readback code {:5} x  {}", codes[i].first, codes[i].second);
            }
        }
    }

    u64 exe_base{};
    u64 text_begin{};
    u64 text_end{};
    DWORD self_tid{};
    std::vector<u8> deep_stack;
    std::unordered_map<DWORD, ThreadInfo> threads;
    std::vector<DWORD> targets;
    std::unordered_map<std::string, Profile> profiles;
    std::unordered_map<HMODULE, std::string> module_names;
    std::unordered_map<HMODULE, ModuleExports> module_exports;
};

} // namespace

void Start() {
    if (const char* setting = std::getenv("SHADPS4_PERF_SAMPLER");
        setting != nullptr && setting[0] == '0') {
        LOG_INFO(Debug, "Perf sampler disabled by SHADPS4_PERF_SAMPLER");
        return;
    }
    if (g_thread.joinable()) {
        return;
    }
    {
        // How much a clock read costs on this system (the counters' timers read it; a slow
        // system clock, HPET for example, makes them expensive).
        constexpr u32 reads = 20000;
        const u64 start = Common::Perf::NowNs();
        u64 last = start;
        for (u32 i = 0; i < reads; ++i) {
            last = Common::Perf::NowNs();
        }
        LOG_INFO(Debug, "Perf: clock read costs {} ns", (last - start) / reads);
    }
    g_thread = std::jthread{[](std::stop_token stop) {
        Sampler sampler;
        sampler.Run(stop);
    }};
}

void Stop() {
    if (g_thread.joinable()) {
        g_thread.request_stop();
        g_thread.join();
    }
}

} // namespace Core::PerfSampler

#else

namespace Core::PerfSampler {

void Start() {}

void Stop() {}

} // namespace Core::PerfSampler

#endif
