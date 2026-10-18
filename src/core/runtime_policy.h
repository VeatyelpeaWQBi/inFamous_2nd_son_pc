// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "common/types.h"
#include <atomic>
#include <chrono>

namespace Core::Runtime {
struct Policy {
    static constexpr u64 readback_window_bytes = 512 * 1024;
    static constexpr bool readback_write_discard = true;
    static constexpr bool readback_write_merge = false;
    static constexpr bool readback_keep_ranges = false;
    static constexpr bool async_readbacks = true;
    static constexpr bool readback_prefetch = true;
    static constexpr u32 async_pipelines = 1;
    static constexpr u32 async_pipeline_wait_us = 3000;
    static constexpr bool compute_compile_parallel = true;
    static constexpr u32 hot_page_faults = 0;
    static constexpr u32 hot_page_cooldown_frames = 240;
    static constexpr bool hot_page_hash = true;
    static constexpr bool readback_queue = true;
    static constexpr bool stream_dedup = true;
    static constexpr bool stream_static = true;
    static constexpr u32 stream_static_frames = 8;
    static constexpr u32 max_gpu_lag_frames = 0;
    static constexpr u32 gpu_clock_div = 10;
    static constexpr u32 asc_yield_packets = 16;
    static constexpr bool real_occlusion = true;
    static constexpr bool lds_barriers_multi_wave = false;
    static constexpr bool wave_memory_barriers = true;
    static constexpr bool threaded_recording = true;
    static constexpr u32 gpu_sync_frame_end = 0;
    static constexpr bool async_uploads = true;
    static constexpr bool staging_budget_auto = true;
    static constexpr u32 recorder_spin_us = 200;
    static constexpr bool sync_memo = true;
    static constexpr bool spec_memo = true;
    static constexpr bool rt_size_learn = false;
    static constexpr u32 gpu_clock_frame_us = 16667;
    static constexpr bool region_summary = true;
    static constexpr bool tex_memo = true;
    static constexpr bool async_written_uploads = true;
    static constexpr bool scout = false;
    static constexpr bool skip_redundant_binds = true;
    static constexpr bool shader_params_memo = true;
    static constexpr bool runtime_lookup_memo = true;
    static constexpr bool skip_empty_draws = true;
    static constexpr bool scout_fast = false;
    static constexpr bool pipeline_key_memo = true;
    static constexpr bool bind_prefetch = true;
    static constexpr bool rt_memo = true;
    static constexpr bool late_protect = true;
    static constexpr bool parallel_protect = true;
    static constexpr bool asc_queue = false;
};
inline constexpr Policy policy{};
inline constexpr const Policy& GetPolicy() { return policy; }
// Guest-visible frame clocks depend on this counter; it is not a performance statistic.
inline std::atomic<u64>& GuestFrameCounter() {
    static std::atomic<u64> frame{0};
    return frame;
}
inline u64 NowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}
} // namespace Core::Runtime
