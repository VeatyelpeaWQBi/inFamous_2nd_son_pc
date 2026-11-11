// SPDX-FileCopyrightText: Copyright 2026 inFamous_2nd_son_pc contributors
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <filesystem>

#include "common/types.h"

namespace VideoCore::FeedbackLink {

inline constexpr u32 kSchema = 1;
inline constexpr u32 kRingCap = 256;
inline constexpr u32 kMaxPerLateWindow = 64;

inline constexpr u32 FlagExactPage = 1u << 0;
inline constexpr u32 FlagWriterHit = 1u << 1;
inline constexpr u32 FlagFallback = 1u << 2;
inline constexpr u32 FlagReduction = 1u << 3;
inline constexpr u32 FlagMissingGpuEnd = 1u << 4;

enum class Label : u32 {
    Inconclusive = 0,
    O01Candidate,
    O02Candidate,
    O03Redirect,
    O08Candidate,
    PublishWake,
};

struct CopyInfo {
    u64 id{};
    u64 prefetch_batch_tick{};
    u64 grouped_max_tick{}; // Display only; queue-domain values are compared separately.
    bool has_extra_dependency{};
};

struct Sample {
    u32 schema{kSchema};
    u64 trace_id{};
    u64 fault_id{};
    u64 demand_addr{};
    u64 demand_size{};
    u64 window_start{};
    u64 window_end{};
    u64 copy_bytes{};
    u32 writer_domain{};
    u64 tick_max{};
    std::array<u64, 4> tick_components{};
    u32 submitted{};
    u32 done{};
    u32 current{};
    u32 queued{};
    u64 dispatch_id{};
    u64 shader_hash{};
    u64 command_tick{};
    u64 gpu_end_qpc_or_ts{};
    u64 submit_tick_end{};
    u32 tail_commands_after_producer{};
    u64 prefetch_job_id{};
    u64 grouped_max_tick{};
    u64 copy_id{};
    bool prefetch_extra_dependency{};
    u64 finish_readback_qpc{};
    u64 wake_qpc{};
    u64 cpu_wait_ms_attr{};
    u32 flags{};
    Label label{Label::Inconclusive};
    bool open{};
    bool sealed{};
};

/// Default false: hot paths must early-out with no alloc / file / lock.
[[nodiscard]] bool IsActive() noexcept;

/// Remember session root for the fixed diagnostics jsonl path. No file created until export.
void Configure(const std::filesystem::path& session_root) noexcept;

[[nodiscard]] u64 NextCopyId() noexcept;
void OnBeginReadback(u64 demand_addr, u64 demand_size, u64 window_start, u64 window_end,
                     u64 copy_bytes, u64 tick0, u64 tick1, bool submitted, bool done, bool current,
                     u64 wait_tick, u64 trace_id, const CopyInfo& copy) noexcept;
void OnWriteTickSample(u64 address, u64 size, u64 tick, u32 queue, u64 shader_hash) noexcept;
/// CPU observation that this exact copy has completed on its timeline.
void OnGpuEndObserved(u64 copy_id, u64 gpu_end_qpc) noexcept;
void OnFinishReadback(u64 copy_id, u64 finish_qpc) noexcept;

/// Cold path: writes sealed in-memory samples to path, or to the configured
/// state/sessions/<id>/diagnostics/feedback-link-samples.jsonl when path is empty.
/// Must not be called from FinishReadback or other per-hook hot paths.
void ExportJsonl(const std::filesystem::path& path = {}) noexcept;

[[nodiscard]] Label Classify(const Sample& sample) noexcept;

} // namespace VideoCore::FeedbackLink