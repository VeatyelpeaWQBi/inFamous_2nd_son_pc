// SPDX-FileCopyrightText: Copyright 2026 inFamous_2nd_son_pc contributors
// SPDX-License-Identifier: GPL-2.0-or-later

#include "video_core/buffer_cache/feedback_link_sample.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "common/perf_stats.h"

namespace VideoCore::FeedbackLink {
namespace {

struct WriteNote {
    u64 address{};
    u64 size{};
    u64 tick{};
    u64 shader_hash{};
    u32 queue{};
};

struct PrefetchNote {
    u64 job_id{};
    u64 grouped_max_tick{};
    u64 window_start{};
    u64 window_end{};
};

struct State {
    std::array<Sample, kRingCap> ring{};
    u32 next{};
    u32 sealed_count{};
    u32 window_count{};
    std::atomic<u64> next_trace_id{1};
    std::array<WriteNote, 64> write_notes{};
    u32 write_note_next{};
    std::array<PrefetchNote, 64> prefetch_notes{};
    u32 prefetch_note_next{};
    std::filesystem::path export_path;
    std::mutex mutex;
};

[[nodiscard]] bool RangesOverlap(u64 a, u64 a_size, u64 start, u64 end) noexcept {
    return a_size != 0 && a < end && start < a + a_size;
}

void ApplyWriteNote(Sample& sample, const WriteNote& note) noexcept {
    sample.flags |= FlagWriterHit;
    if (note.queue < sample.tick_components.size()) {
        sample.tick_components[note.queue] = std::max(sample.tick_components[note.queue], note.tick);
        sample.tick_max = std::max(sample.tick_max, note.tick);
    }
    if (sample.shader_hash == 0) {
        sample.shader_hash = note.shader_hash;
    }
}

void ApplyPrefetchNote(Sample& sample, const PrefetchNote& note) noexcept {
    if (note.job_id != 0) {
        sample.prefetch_job_id = note.job_id;
    }
    sample.grouped_max_tick = std::max(sample.grouped_max_tick, note.grouped_max_tick);
}

void ApplyGpuEnd(Sample& sample, u64 gpu_end_qpc) noexcept {
    if (gpu_end_qpc == 0) {
        return;
    }
    if (sample.gpu_end_qpc_or_ts == 0 || gpu_end_qpc < sample.gpu_end_qpc_or_ts) {
        sample.gpu_end_qpc_or_ts = gpu_end_qpc;
    }
    sample.flags &= ~FlagMissingGpuEnd;
}

std::atomic<State*> g_state{nullptr};

State* EnsureState() noexcept {
    State* state = g_state.load(std::memory_order_acquire);
    if (state) {
        return state;
    }
    try {
        auto owned = std::make_unique<State>();
        State* expected = nullptr;
        if (g_state.compare_exchange_strong(expected, owned.get(), std::memory_order_acq_rel,
                                            std::memory_order_acquire)) {
            return owned.release();
        }
        return expected;
    } catch (...) {
        return nullptr;
    }
}

const char* LabelName(Label label) noexcept {
    switch (label) {
    case Label::O01Candidate:
        return "O01_candidate";
    case Label::O02Candidate:
        return "O02_candidate";
    case Label::O03Redirect:
        return "O03_redirect";
    case Label::O08Candidate:
        return "O08_candidate";
    case Label::PublishWake:
        return "publish_wake";
    case Label::Inconclusive:
    default:
        return "inconclusive";
    }
}

Sample* FindOpen(State& state, u64 window_start, u64 window_end) noexcept {
    for (auto& sample : state.ring) {
        if (sample.open && !sample.sealed && sample.window_start == window_start &&
            sample.window_end == window_end) {
            return &sample;
        }
    }
    return nullptr;
}

} // namespace

bool IsActive() noexcept {
    return Common::Perf::GetTuning().feedback_link_sample.load(std::memory_order_relaxed);
}

void Configure(const std::filesystem::path& session_root) noexcept {
    if (session_root.empty() || !IsActive()) {
        return;
    }
    State* state = EnsureState();
    if (!state) {
        return;
    }
    std::scoped_lock lock{state->mutex};
    // Fixed contract: state/sessions/<session_id>/diagnostics/feedback-link-samples.jsonl
    state->export_path = session_root / "diagnostics" / "feedback-link-samples.jsonl";
}

void OnBeginReadback(u64 demand_addr, u64 demand_size, u64 window_start, u64 window_end,
                     u64 copy_bytes, u64 tick0, u64 tick1, bool submitted, bool done, bool current,
                     u64 wait_tick, u64 trace_id) noexcept {
    if (!IsActive()) {
        return;
    }
    State* state = EnsureState();
    if (!state) {
        return;
    }
    std::scoped_lock lock{state->mutex};
    if (state->window_count >= kMaxPerLateWindow) {
        return;
    }
    Sample& sample = state->ring[state->next % kRingCap];
    state->next = (state->next + 1) % kRingCap;
    sample = Sample{};
    sample.schema = kSchema;
    // PathTrace::NextId is zero when PathTrace is off. Keep that id when armed so samples
    // correlate with the path-trace log; otherwise issue an independent monotonic id.
    sample.trace_id = trace_id != 0
                          ? trace_id
                          : state->next_trace_id.fetch_add(1, std::memory_order_relaxed);
    sample.fault_id = sample.trace_id;
    sample.demand_addr = demand_addr;
    sample.demand_size = demand_size;
    sample.window_start = window_start;
    sample.window_end = window_end;
    sample.copy_bytes = copy_bytes;
    sample.tick_components[0] = tick0;
    sample.tick_components[1] = tick1;
    sample.tick_max = std::max(tick0, tick1);
    sample.submitted = submitted ? 1u : 0u;
    sample.done = done ? 1u : 0u;
    sample.current = current ? 1u : 0u;
    sample.queued = (!done && !submitted && !current) ? 1u : 0u;
    sample.command_tick = wait_tick;
    sample.writer_domain = (tick0 ? 1u : 0u) | (tick1 ? 2u : 0u);
    // Missing until a timeline/fence observation fills gpu_end_qpc_or_ts (doc/149 §10.2).
    sample.flags = FlagMissingGpuEnd;
    sample.gpu_end_qpc_or_ts = 0;
    if (tick0 != 0 || tick1 != 0) {
        sample.flags |= FlagWriterHit;
    }
    for (const auto& note : state->write_notes) {
        if (note.size != 0 &&
            RangesOverlap(note.address, note.size, sample.window_start, sample.window_end)) {
            ApplyWriteNote(sample, note);
        }
    }
    for (const auto& note : state->prefetch_notes) {
        if (note.window_end > note.window_start && note.window_start == sample.window_start &&
            note.window_end == sample.window_end) {
            ApplyPrefetchNote(sample, note);
        }
    }
    sample.open = true;
    sample.sealed = false;
    ++state->window_count;
}

void OnPrefetchBatch(u64 job_id, u64 grouped_max_tick, u64 window_start,
                     u64 window_end) noexcept {
    if (!IsActive()) {
        return;
    }
    State* state = g_state.load(std::memory_order_acquire);
    if (!state) {
        return;
    }
    std::scoped_lock lock{state->mutex};
    PrefetchNote note{
        .job_id = job_id,
        .grouped_max_tick = grouped_max_tick,
        .window_start = window_start,
        .window_end = window_end,
    };
    state->prefetch_notes[state->prefetch_note_next++ % state->prefetch_notes.size()] = note;
    if (window_end > window_start) {
        if (Sample* sample = FindOpen(*state, window_start, window_end)) {
            ApplyPrefetchNote(*sample, note);
        }
        return;
    }
    // Window identity unavailable (legacy 0,0): attach only to the newest open sample.
    for (u32 i = 0; i < kRingCap; ++i) {
        const u32 idx = (state->next + kRingCap - 1 - i) % kRingCap;
        Sample& sample = state->ring[idx];
        if (sample.open && !sample.sealed) {
            ApplyPrefetchNote(sample, note);
            break;
        }
    }
}

void OnWriteTickSample(u64 address, u64 size, u64 tick, u32 queue, u64 shader_hash) noexcept {
    if (!IsActive()) {
        return;
    }
    State* state = g_state.load(std::memory_order_acquire);
    if (!state || address == 0 || size == 0) {
        return;
    }
    std::scoped_lock lock{state->mutex};
    const WriteNote note{
        .address = address,
        .size = size,
        .tick = tick,
        .shader_hash = shader_hash,
        .queue = queue,
    };
    state->write_notes[state->write_note_next++ % state->write_notes.size()] = note;
    for (u32 i = 0; i < kRingCap; ++i) {
        const u32 idx = (state->next + kRingCap - 1 - i) % kRingCap;
        Sample& sample = state->ring[idx];
        if (!sample.open || sample.sealed) {
            continue;
        }
        if (!RangesOverlap(address, size, sample.window_start, sample.window_end)) {
            continue;
        }
        ApplyWriteNote(sample, note);
        break;
    }
}

void OnDispatchOrBind(u64 shader_hash, u64 command_tick, u64 /*address*/,
                      u64 /*size*/) noexcept {
    if (!IsActive()) {
        return;
    }
    State* state = g_state.load(std::memory_order_acquire);
    if (!state || shader_hash == 0) {
        return;
    }
    std::scoped_lock lock{state->mutex};
    for (u32 i = 0; i < kRingCap; ++i) {
        const u32 idx = (state->next + kRingCap - 1 - i) % kRingCap;
        Sample& sample = state->ring[idx];
        if (!sample.open || sample.sealed) {
            continue;
        }
        if (sample.shader_hash == 0) {
            sample.shader_hash = shader_hash;
        }
        if (sample.dispatch_id == 0) {
            sample.dispatch_id = command_tick;
        }
        if (sample.command_tick == 0) {
            sample.command_tick = command_tick;
        }
        break;
    }
}

void OnSchedulerFlush(u64 submit_tick_end) noexcept {
    if (!IsActive()) {
        return;
    }
    State* state = g_state.load(std::memory_order_acquire);
    if (!state || submit_tick_end == 0) {
        return;
    }
    std::scoped_lock lock{state->mutex};
    for (auto& sample : state->ring) {
        if (!sample.open || sample.sealed) {
            continue;
        }
        sample.submit_tick_end = std::max(sample.submit_tick_end, submit_tick_end);
        if (sample.command_tick != 0 && submit_tick_end > sample.command_tick) {
            const u64 gap = submit_tick_end - sample.command_tick;
            sample.tail_commands_after_producer =
                static_cast<u32>(std::min<u64>(gap, 0xffffffffu));
        }
    }
}

void OnGpuEndObserved(u64 window_start, u64 window_end, u64 gpu_end_qpc) noexcept {
    if (!IsActive() || gpu_end_qpc == 0) {
        return;
    }
    State* state = g_state.load(std::memory_order_acquire);
    if (!state) {
        return;
    }
    std::scoped_lock lock{state->mutex};
    if (Sample* sample = FindOpen(*state, window_start, window_end)) {
        ApplyGpuEnd(*sample, gpu_end_qpc);
    }
}

void OnGpuEndObservedTick(u64 demand_addr, u64 tick, u64 gpu_end_qpc) noexcept {
    if (!IsActive() || gpu_end_qpc == 0 || tick == 0) {
        return;
    }
    State* state = g_state.load(std::memory_order_acquire);
    if (!state) {
        return;
    }
    std::scoped_lock lock{state->mutex};
    bool matched_tick = false;
    for (auto& sample : state->ring) {
        if (!sample.open || sample.sealed) {
            continue;
        }
        if (sample.command_tick == tick) {
            ApplyGpuEnd(sample, gpu_end_qpc);
            matched_tick = true;
        }
    }
    if (matched_tick || demand_addr == 0) {
        return;
    }
    // No sample stored this wait tick (PathTrace-off ids, or prefetch reuse):
    // fall back to the open window that contains the demand address.
    for (auto& sample : state->ring) {
        if (!sample.open || sample.sealed) {
            continue;
        }
        if (RangesOverlap(demand_addr, 1, sample.window_start, sample.window_end) ||
            RangesOverlap(demand_addr, 1, sample.demand_addr,
                          sample.demand_addr + sample.demand_size)) {
            ApplyGpuEnd(sample, gpu_end_qpc);
        }
    }
}

void OnFinishReadback(u64 window_start, u64 window_end, u64 finish_qpc, u64 wake_qpc,
                      u64 cpu_wait_ms, u64 gpu_end_qpc) noexcept {
    if (!IsActive()) {
        return;
    }
    State* state = g_state.load(std::memory_order_acquire);
    if (!state) {
        return;
    }
    std::scoped_lock lock{state->mutex};
    Sample* sample = FindOpen(*state, window_start, window_end);
    if (!sample) {
        return;
    }
    sample->finish_readback_qpc = finish_qpc;
    sample->wake_qpc = wake_qpc ? wake_qpc : finish_qpc;
    sample->cpu_wait_ms_attr = cpu_wait_ms;
    ApplyGpuEnd(*sample, gpu_end_qpc);
    sample->label = Classify(*sample);
    sample->open = false;
    sample->sealed = true;
    ++state->sealed_count;
}

Label Classify(const Sample& sample) noexcept {
    const bool missing_gpu_end =
        (sample.flags & FlagMissingGpuEnd) != 0 || sample.gpu_end_qpc_or_ts == 0;

    // Broken chain: no demand/window/copy identity.
    if (sample.demand_addr == 0 || sample.window_end <= sample.window_start) {
        return Label::Inconclusive;
    }

    // doc/149 §10.2: never promote O01 from coarse ReadbackProducer* counters alone when
    // GPU-end timeline is missing.
    if (missing_gpu_end) {
        if (sample.grouped_max_tick != 0 && sample.tick_max != 0 &&
            sample.grouped_max_tick > sample.tick_max) {
            return Label::O08Candidate;
        }
        if (sample.finish_readback_qpc != 0 && sample.wake_qpc > sample.finish_readback_qpc) {
            return Label::PublishWake;
        }
        // Independent batch-tail evidence without producer GPU end is still insufficient for O01.
        return Label::Inconclusive;
    }

    if (sample.gpu_end_qpc_or_ts != 0 && sample.submit_tick_end != 0 &&
        sample.command_tick != 0 && sample.submit_tick_end > sample.command_tick &&
        sample.tail_commands_after_producer != 0) {
        return Label::O01Candidate;
    }
    if ((sample.flags & FlagFallback) != 0 || (sample.flags & FlagReduction) != 0) {
        return Label::O02Candidate;
    }
    if (sample.grouped_max_tick != 0 && sample.tick_max != 0 &&
        sample.grouped_max_tick > sample.tick_max) {
        return Label::O08Candidate;
    }
    if (sample.done && sample.current == 0 && sample.submitted == 0 &&
        sample.finish_readback_qpc != 0 && sample.wake_qpc > sample.finish_readback_qpc) {
        return Label::PublishWake;
    }
    if (sample.current && sample.done == 0) {
        return Label::O03Redirect;
    }
    return Label::Inconclusive;
}

void ExportJsonl(const std::filesystem::path& path) noexcept {
    if (!IsActive()) {
        return;
    }
    State* state = g_state.load(std::memory_order_acquire);
    if (!state) {
        return;
    }

    std::vector<Sample> sealed;
    std::filesystem::path out;
    {
        std::scoped_lock lock{state->mutex};
        out = path.empty() ? state->export_path : path;
        if (out.empty() || state->sealed_count == 0) {
            return;
        }
        sealed.reserve(state->sealed_count);
        for (const auto& sample : state->ring) {
            if (sample.sealed) {
                sealed.push_back(sample);
            }
        }
    }

    try {
        std::filesystem::create_directories(out.parent_path());
        std::ofstream file{out, std::ios::out | std::ios::trunc};
        if (!file) {
            return;
        }
        for (const auto& sample : sealed) {
            char line[768];
            const int n = std::snprintf(
                line, sizeof(line),
                "{\"schema\":%u,\"trace_id\":%llu,\"fault_id\":%llu,"
                "\"demand_addr\":%llu,\"demand_size\":%llu,"
                "\"window_start\":%llu,\"window_end\":%llu,\"copy_bytes\":%llu,"
                "\"writer_domain\":%u,\"tick_max\":%llu,"
                "\"tick_components\":[%llu,%llu,%llu,%llu],"
                "\"submitted\":%u,\"done\":%u,\"current\":%u,\"queued\":%u,"
                "\"dispatch_id\":%llu,\"shader_hash\":%llu,\"command_tick\":%llu,"
                "\"gpu_end_qpc_or_ts\":%llu,\"submit_tick_end\":%llu,"
                "\"tail_commands_after_producer\":%u,"
                "\"prefetch_job_id\":%llu,\"grouped_max_tick\":%llu,"
                "\"finish_readback_qpc\":%llu,\"wake_qpc\":%llu,"
                "\"cpu_wait_ms_attr\":%llu,\"flags\":%u,\"label\":\"%s\","
                "\"missing_gpu_end\":%s}\n",
                sample.schema, static_cast<unsigned long long>(sample.trace_id),
                static_cast<unsigned long long>(sample.fault_id),
                static_cast<unsigned long long>(sample.demand_addr),
                static_cast<unsigned long long>(sample.demand_size),
                static_cast<unsigned long long>(sample.window_start),
                static_cast<unsigned long long>(sample.window_end),
                static_cast<unsigned long long>(sample.copy_bytes), sample.writer_domain,
                static_cast<unsigned long long>(sample.tick_max),
                static_cast<unsigned long long>(sample.tick_components[0]),
                static_cast<unsigned long long>(sample.tick_components[1]),
                static_cast<unsigned long long>(sample.tick_components[2]),
                static_cast<unsigned long long>(sample.tick_components[3]), sample.submitted,
                sample.done, sample.current, sample.queued,
                static_cast<unsigned long long>(sample.dispatch_id),
                static_cast<unsigned long long>(sample.shader_hash),
                static_cast<unsigned long long>(sample.command_tick),
                static_cast<unsigned long long>(sample.gpu_end_qpc_or_ts),
                static_cast<unsigned long long>(sample.submit_tick_end),
                sample.tail_commands_after_producer,
                static_cast<unsigned long long>(sample.prefetch_job_id),
                static_cast<unsigned long long>(sample.grouped_max_tick),
                static_cast<unsigned long long>(sample.finish_readback_qpc),
                static_cast<unsigned long long>(sample.wake_qpc),
                static_cast<unsigned long long>(sample.cpu_wait_ms_attr), sample.flags,
                LabelName(sample.label),
                ((sample.flags & FlagMissingGpuEnd) != 0 || sample.gpu_end_qpc_or_ts == 0)
                    ? "true"
                    : "false");
            if (n > 0 && static_cast<size_t>(n) < sizeof(line)) {
                file.write(line, n);
            }
        }
    } catch (...) {
        // Diagnostics must never throw into the caller.
    }
}

} // namespace VideoCore::FeedbackLink
