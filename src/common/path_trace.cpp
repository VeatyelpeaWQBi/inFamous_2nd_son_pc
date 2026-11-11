// SPDX-License-Identifier: GPL-2.0-or-later
#include "common/path_trace.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <locale>
#include <memory>
#include <stdexcept>
#include <thread>
#ifdef _WIN32
#include <windows.h>
#endif
#include "common/logging/log.h"
#include "common/path_trace_queue.h"
#include "common/perf_stats.h"
#include "common/thread.h"

namespace Common::PathTrace {
namespace {
constexpr u32 MaxThreads = 64;
// Bounded ~18 MiB total: absorb short CPU/I/O scheduling stalls during particle peaks.
constexpr u32 Capacity = 4096;
struct Entry {
    u64 sequence, ns, frame, id, a, b, c, d;
    Kind kind;
};
struct Slot {
    SpscRing<Entry, Capacity> ring;
    std::atomic<u64> dropped{0};
};
struct Anchor {
    u64 ns_before{}, qpc{}, ns_after{}, frequency{};
};
Anchor SampleClock() {
    Anchor a;
    a.ns_before = Perf::NowNs();
#ifdef _WIN32
    LARGE_INTEGER qpc{}, frequency{};
    if (QueryPerformanceCounter(&qpc) && QueryPerformanceFrequency(&frequency)) {
        a.qpc = qpc.QuadPart;
        a.frequency = frequency.QuadPart;
    }
#endif
    a.ns_after = Perf::NowNs();
    return a;
}
constexpr std::array Names{
    "read_request", "read_cp_begin", "read_cp_end", "read_completion_request", "read_dependencies",
    "read_copy", "read_wait_begin", "read_wait_end", "read_fallback_begin", "read_fallback_end",
    "read_return", "flip_request", "flip_prepare", "flip_ready", "flip_consume",
    "flip_present_return", "flip_done",
    "copy_record", "copy_publish_begin", "copy_publish_end", "gpu_dispatch_result", "frame_sample", "tuning_reload",
    "submit_queued", "submit_driver_begin", "submit_driver_end", "flip_poll",
    "feedback_checkpoint",
    "gpu_dispatch_bounds", "gpu_copy_bounds", "gpu_session_bounds",
    "word_load_result", "word_value_check",
    "gpu_clock_calibration",
    "flip_eop_register", "gnm_wait_begin", "gnm_wait_end", "word_prefetch_copy",
    "gpu_session_workload",
    "gpu_session_dispatch", "gpu_dispatch_geometry", "gfx_command_census", "read_target_ticks",
    "flip_caller_sample", "particle_label_write", "particle_label_schedule", "cp_queue_slice",
    "cp_asc_slice_work", "cp_asc_dispatch_phases", "cp_asc_binding_phases", "cp_asc_buffer_detail",
    "readback_barrier_scope", "page_protect", "compute_scope_audit", "dispatch_pair_census",
    "dispatch_pair_shader", "compute_output_range", "draw_batch_census", "indirect_args_census",
    "readback_wait_scope", "readback_wait_layout", "readback_wait_span", "readback_wait_writer",
    "compute_protect_scope", "compute_protect_binding", "compute_protect_binding_cost",
    "draw_prep_slice", "draw_prep_phases_0", "draw_prep_phases_1", "draw_prep_resources",
    "draw_prep_buffer_detail", "draw_prep_identity", "draw_prep_limit",
    "draw_prep_phase",
    "read_prefetch_job", "read_prefetch_batch",
};
static_assert(Names.size() == static_cast<u32>(Kind::ReadPrefetchBatch) + 1);
struct State {
    std::array<Slot, MaxThreads> slots;
    std::atomic<u32> next_slot{0};
    std::atomic<u64> next_id{1}, rejected{0};
    // Bit zero closes entry; the remaining bits count active publishers. This avoids a
    // stop/start race between a writer's final drain and a previously admitted publisher.
    std::atomic<u64> gate{0};
    std::atomic<bool> stopping{false};
    std::thread worker;
    std::ofstream file;
    std::filesystem::path directory;
    Anchor first{};
    u32 initial_gpu_profile{};
    u64 initial_compute_hash{};
    u64 events{}, cutoff_publishers{};
    bool io_failed{};
    bool page_protect_audit{};
    bool compute_scope_audit{};
    bool readback_wait_scope_audit{};
    bool compute_protect_audit{};
    bool draw_prep_audit{};
    std::atomic<bool> draw_prep_cap_hit{};

    bool Drain() {
        bool any = false;
        const auto count = std::min(next_slot.load(std::memory_order_relaxed), MaxThreads);
        for (u32 i = 0; i < count; ++i) {
            Entry e;
            // Bound each pass, including shutdown: a busy producer cannot starve other rings.
            for (u32 n = 0; n < Capacity && slots[i].ring.Pop(e); ++n) {
                any = true;
                file << i << ',' << e.sequence << ',' << e.ns << ',' << e.frame << ','
                     << Names[static_cast<u32>(e.kind)] << ',' << e.id << ',' << e.a << ','
                     << e.b << ',' << e.c << ',' << e.d << '\n';
                ++events;
            }
        }
        io_failed |= !file.good();
        return any;
    }
    void Metadata(bool finished) {
        const auto end = SampleClock();
        u64 dropped = rejected.load(std::memory_order_relaxed);
        for (const auto& slot : slots) dropped += slot.dropped.load(std::memory_order_relaxed);
        std::ofstream meta{directory / "path-trace.json", std::ios::trunc};
        meta.imbue(std::locale::classic());
        const auto anchor = [&meta](const Anchor& a) {
            meta << "{\"ns_before\":" << a.ns_before << ",\"qpc\":" << a.qpc
                 << ",\"ns_after\":" << a.ns_after << ",\"frequency\":" << a.frequency << '}';
        };
        meta << "{\"schema\":1,\"clock\":\"steady_clock_ns\",\"start\":";
        anchor(first);
        meta << ",\"end\":";
        anchor(end);
        meta << ",\"complete\":" << (finished && !io_failed && !dropped && !cutoff_publishers ? "true" : "false")
             << ",\"io_failed\":" << (io_failed ? "true" : "false")
             << ",\"events\":" << events << ",\"dropped\":" << dropped
             << ",\"cutoff_publishers\":" << cutoff_publishers
             << ",\"registered_threads\":" << std::min(next_slot.load(), MaxThreads)
             << ",\"gpu_profile_at_start\":" << initial_gpu_profile
             << ",\"compute_hash_at_start\":" << initial_compute_hash
             << ",\"queue_bytes\":" << sizeof(slots)
             << ",\"draw_prep_audit\":" << (draw_prep_audit ? "true" : "false")
             << ",\"draw_prep_protocol\":2"
             << ",\"draw_prep_sample_period\":" << DrawPrepSamplePeriod
             << ",\"draw_prep_sample_cap\":" << DrawPrepSampleCap
             << ",\"draw_prep_cap_hit\":" << (draw_prep_cap_hit.load() ? "true" : "false")
             << ",\"guest_frame_semantics\":\"SubmitDone context only; not a causal frame ID\"}\n";
        meta.close();
        if (!meta) throw std::runtime_error("path trace metadata write failed");
    }
    void Run() noexcept {
        try {
            SetCurrentThreadName("IFSS:PathTraceWriter");
            SetCurrentThreadPriority(ThreadPriority::Low);
            auto flushed = std::chrono::steady_clock::now();
            while (!stopping.load(std::memory_order_acquire)) {
                Drain();
                const auto now = std::chrono::steady_clock::now();
                if (now - flushed >= std::chrono::seconds{1}) {
                    file.flush();
                    io_failed |= !file.good();
                    flushed = now;
                }
                if (io_failed) break;
                std::this_thread::sleep_for(std::chrono::milliseconds{20});
            }
            // Only shutdown waits, never a guest handler. Bound the wait for pre-empted
            // publishers; retain their storage until process exit and mark any cut tail.
            gate.fetch_or(1, std::memory_order_acq_rel);
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds{100};
            while (gate.load(std::memory_order_acquire) != 1 &&
                   std::chrono::steady_clock::now() < deadline) {
                Drain();
                std::this_thread::sleep_for(std::chrono::milliseconds{1});
            }
            cutoff_publishers = gate.load(std::memory_order_acquire) >> 1;
            Drain();
            file.flush();
            io_failed |= !file.good();
            Metadata(true);
            file.close();
        } catch (...) {
            gate.fetch_or(1, std::memory_order_acq_rel);
            io_failed = true; // Initial incomplete metadata survives if the final write failed.
        }
    }
};
static_assert(std::atomic<u64>::is_always_lock_free);
static_assert(std::atomic<State*>::is_always_lock_free);
std::atomic<State*> active{nullptr};
State* retained{}; // Never freed: fault handlers can have loaded active immediately before Stop.
} // namespace

void Start(const std::filesystem::path& directory, bool paused) noexcept {
    if (retained) return; // One startup-only capture per process.
    try {
        auto state = std::make_unique<State>();
        const char* draw_audit = std::getenv("IFSS_DRAW_PREP_AUDIT");
        state->draw_prep_audit = draw_audit && std::strcmp(draw_audit, "1") == 0;
        const char* audit = std::getenv("IFSS_PAGE_PROTECT_AUDIT");
        state->page_protect_audit = audit && audit[0] == '1' && audit[1] == '\0';
        const char* scope_audit = std::getenv("IFSS_COMPUTE_SCOPE_AUDIT");
        state->compute_scope_audit =
            scope_audit && scope_audit[0] == '1' && scope_audit[1] == '\0';
        const char* wait_audit = std::getenv("IFSS_READBACK_WAIT_SCOPE_AUDIT");
        state->readback_wait_scope_audit =
            wait_audit && wait_audit[0] == '1' && wait_audit[1] == '\0';
        const char* protect_audit = std::getenv("IFSS_COMPUTE_PROTECT_AUDIT");
        state->compute_protect_audit =
            protect_audit && protect_audit[0] == '1' && protect_audit[1] == '\0';
        state->directory = directory;
        state->file.open(directory / "path-trace.csv", std::ios::trunc);
        state->file.imbue(std::locale::classic());
        state->file << "thread,seq,ns,guest_frame,event,id,a,b,c,d\n";
        if (!state->file) throw std::runtime_error("path trace CSV open failed");
        state->first = SampleClock();
        state->initial_gpu_profile = Perf::GetTuning().gpu_profile.load(std::memory_order_relaxed);
        state->initial_compute_hash = Perf::GetTuning().gpu_profile_compute_hash.load(std::memory_order_relaxed);
        state->Metadata(false);
        State* ptr = state.get();
        state->worker = std::thread([ptr] { ptr->Run(); });
        retained = state.release();
        active.store(paused ? nullptr : retained, std::memory_order_release);
        LOG_INFO(Debug, "Critical path trace enabled: bounded {} byte queues, background CSV writer",
                 sizeof(retained->slots));
    } catch (const std::exception& e) {
        LOG_WARNING(Debug, "Critical path trace unavailable: {}", e.what());
    }
}
bool Resume() noexcept {
    if (!retained || !retained->worker.joinable() ||
        (retained->gate.load(std::memory_order_acquire) & 1)) return false;
    active.store(retained, std::memory_order_release);
    return true;
}
void Stop() noexcept {
    active.store(nullptr, std::memory_order_release);
    if (!retained || !retained->worker.joinable()) return;
    retained->gate.fetch_or(1, std::memory_order_acq_rel);
    retained->stopping.store(true, std::memory_order_release);
    retained->worker.join();
    u64 dropped = retained->rejected.load(std::memory_order_relaxed);
    for (const auto& slot : retained->slots) dropped += slot.dropped.load(std::memory_order_relaxed);
    if (retained->io_failed || retained->cutoff_publishers || dropped) {
        LOG_WARNING(Debug, "Critical path trace incomplete: {} dropped, {} active publishers, I/O failure {}; inspect path-trace.json",
                    dropped, retained->cutoff_publishers, retained->io_failed);
    }
}
u64 NextId() noexcept {
    State* state = active.load(std::memory_order_acquire);
    return state ? state->next_id.fetch_add(1, std::memory_order_relaxed) : 0;
}
bool IsActive() noexcept {
    return active.load(std::memory_order_acquire) != nullptr;
}
namespace {
thread_local u64 guest_submit_total_ns{};
thread_local u64 guest_submit_start_ns{};
std::atomic<u32> flip_caller_tid{};
std::atomic<u64> sampled_particle_label{};
}

GuestSubmitScope::GuestSubmitScope() noexcept {
    if (active.load(std::memory_order_acquire) && guest_submit_start_ns == 0) {
        guest_submit_start_ns = Perf::NowNs();
        outer = true;
    }
}

GuestSubmitScope::~GuestSubmitScope() {
    if (outer) {
        guest_submit_total_ns += Perf::NowNs() - guest_submit_start_ns;
        guest_submit_start_ns = 0;
    }
}

void EmitFlipRegistration(u64 id, u64 buffer_id, u64 flip_arg) noexcept {
    if (!id) {
        return;
    }
    u64 encoded_cpu_ns = 0;
#ifdef _WIN32
    flip_caller_tid.store(GetCurrentThreadId(), std::memory_order_relaxed);
    FILETIME creation{}, exit{}, kernel{}, user{};
    if (GetThreadTimes(GetCurrentThread(), &creation, &exit, &kernel, &user)) {
        const auto ticks = [](FILETIME t) {
            return (static_cast<u64>(t.dwHighDateTime) << 32) | t.dwLowDateTime;
        };
        // Reserve zero for legacy records and failed queries.
        encoded_cpu_ns = (ticks(kernel) + ticks(user)) * 100 + 1;
    }
#endif
    const u64 submit_ns = guest_submit_total_ns +
                          (guest_submit_start_ns ? Perf::NowNs() - guest_submit_start_ns : 0);
    Emit(Kind::FlipEopRegister, id, buffer_id, flip_arg, encoded_cpu_ns, submit_ns + 1);
}
bool IsFlipCaller(u32 tid) noexcept {
    return tid && tid == flip_caller_tid.load(std::memory_order_relaxed) &&
           active.load(std::memory_order_acquire);
}
bool IsSampledParticleLabel(u64 address) noexcept {
    return address && address == sampled_particle_label.load(std::memory_order_relaxed);
}
void EmitFlipCallerSample(u32 tid, u64 rip, u64 frame_return_hint, u64 label_hint,
                          u32 label_threshold) noexcept {
    if (label_hint) {
        sampled_particle_label.store(label_hint, std::memory_order_relaxed);
    }
    const u64 caller = tid | (static_cast<u64>(label_threshold) << 32);
    Emit(Kind::FlipCallerSample, NextId(), caller, rip, frame_return_hint, label_hint);
}
bool PageProtectAuditActive() noexcept {
    const State* state = active.load(std::memory_order_acquire);
    return state && state->page_protect_audit;
}

bool ReadbackWaitScopeAuditActive() noexcept {
    const State* state = active.load(std::memory_order_acquire);
    return state && state->readback_wait_scope_audit;
}
bool DrawPrepAuditActive() noexcept {
    const State* state = active.load(std::memory_order_acquire);
    return state && state->draw_prep_audit;
}

bool SampleComputeProtectScope() noexcept {
    const State* state = active.load(std::memory_order_acquire);
    if (!state || !state->compute_protect_audit) {
        return false;
    }
    // Mix the ordinal to avoid taking the same slot in each recurring set of dispatches.
    static thread_local u64 ordinal{};
    const u64 sample = ++ordinal * 0x9e3779b97f4a7c15ULL;
    return (sample >> 61) == 0;
}

ComputeProtectionScope::ComputeProtectionScope(u64 shader_hash) noexcept
    : previous{current}, shader{shader_hash} {
    // Retain real binding footprints only for the families dominating the prior scope audit.
    binding_detail = shader == 0x9cd53a1f || shader == 0x46fccbf8 || shader == 0x31a66b7c ||
                     shader == 0xb29d89a7 || shader == 0x4b4988c3 || shader == 0xc214b634;
    current = this;
}

ComputeProtectionScope::~ComputeProtectionScope() {
    FinishBinding();
    current = previous;
    Emit(Kind::ComputeProtectScope, NextId(), shader, calls,
         (u64{same_binding} << 32) | different_binding, total_ns);
}

void ComputeProtectionScope::FinishBinding() noexcept {
    if (!binding_detail || binding_calls == 0) {
        return;
    }
    const u64 id = NextId();
    Emit(Kind::ComputeProtectBinding, id, shader, (u64{binding} << 32) | binding_calls,
         binding_address, binding_size);
    Emit(Kind::ComputeProtectBindingCost, id, binding_ns, binding_bytes, binding_min, binding_end);
    binding_calls = binding_ns = binding_bytes = 0;
}

void ComputeProtectionScope::Binding(u32 index, bool is_written, u64 address, u64 size) noexcept {
    FinishBinding();
    binding = index;
    written = is_written;
    binding_address = address;
    binding_size = size;
}

void ComputeProtectionScope::Note(u64 address, u64 size, u64 view, bool no_access,
                                  u64 elapsed_ns) noexcept {
    if (!written || !no_access) {
        last_eligible = false;
        return;
    }
    ++calls;
    total_ns += elapsed_ns;
    if (binding_detail) {
        binding_min = binding_calls == 0 ? address : std::min(binding_min, address);
        binding_end = binding_calls == 0 ? address + size : std::max(binding_end, address + size);
        ++binding_calls;
        binding_ns += elapsed_ns;
        binding_bytes += size;
    }
    if (last_eligible && view == last_view &&
        (last_end == address || address + size == last_address)) {
        if (last_binding == binding) {
            ++same_binding;
        } else {
            ++different_binding;
        }
    }
    last_eligible = true;
    last_address = address;
    last_end = address + size;
    last_view = view;
    last_binding = binding;
}

void Emit(Kind kind, u64 id, u64 a, u64 b, u64 c, u64 d) noexcept {
    if (!id) return;
    State* state = active.load(std::memory_order_acquire);
    if (!state) return;
    if (state->draw_prep_audit && kind != Kind::DrawPrepSlice &&
        kind != Kind::DrawPrepPhases0 && kind != Kind::DrawPrepPhases1 &&
        kind != Kind::DrawPrepResources && kind != Kind::DrawPrepBufferDetail &&
        kind != Kind::DrawPrepIdentity && kind != Kind::DrawPrepLimit &&
        kind != Kind::DrawPrepPhase &&
        kind != Kind::FrameSample && kind != Kind::FlipEopRegister &&
        kind != Kind::SubmitQueued && kind != Kind::SubmitDriverBegin &&
        kind != Kind::SubmitDriverEnd) {
        return;
    }
    if (state->draw_prep_audit && kind == Kind::DrawPrepLimit) {
        state->draw_prep_cap_hit.store(true, std::memory_order_relaxed);
    }
    if (state->compute_protect_audit && kind != Kind::ComputeProtectScope &&
        kind != Kind::ComputeProtectBinding && kind != Kind::ComputeProtectBindingCost &&
        kind != Kind::FrameSample) {
        return;
    }
    if (state->readback_wait_scope_audit && kind != Kind::ReadbackWaitScope &&
        kind != Kind::ReadbackWaitLayout && kind != Kind::ReadbackWaitSpan &&
        kind != Kind::ReadbackWaitWriter && kind != Kind::CopyRecord &&
        kind != Kind::FrameSample) {
        return;
    }
    if (state->readback_wait_scope_audit && kind == Kind::CopyRecord) {
        const auto& tuning = Perf::GetTuning();
        const auto contains_page = [&](u64 address) {
            const u64 page = address & ~u64{4095};
            return address && a <= page && page < b;
        };
        if (!contains_page(tuning.trace_reads_lo.load(std::memory_order_relaxed)) &&
            !contains_page(tuning.trace_writer_word.load(std::memory_order_relaxed))) {
            return;
        }
    }
    if (state->page_protect_audit && kind != Kind::PageProtect && kind != Kind::FrameSample &&
        kind != Kind::CpQueueSlice) {
        return;
    }
    if (state->compute_scope_audit && kind != Kind::ComputeScopeAudit &&
        kind != Kind::DispatchPairCensus && kind != Kind::DispatchPairShader &&
        kind != Kind::ComputeOutputRange && kind != Kind::DrawBatchCensus &&
        kind != Kind::IndirectArgsCensus && kind != Kind::FrameSample) {
        return;
    }
    const u64 gate = state->gate.fetch_add(2, std::memory_order_acq_rel);
    if (gate & 1) {
        state->gate.fetch_sub(2, std::memory_order_release);
        return;
    }
    struct Producer {
        u32 slot{MaxThreads};
        u64 sequence{};
        bool registered{}, busy{};
    };
    static thread_local Producer producer;
    if (producer.busy) {
        state->rejected.fetch_add(1, std::memory_order_relaxed);
        state->gate.fetch_sub(2, std::memory_order_release);
        return;
    }
    producer.busy = true; // Guard registration too against a recursive fault.
    if (!producer.registered) {
        producer.slot = state->next_slot.fetch_add(1, std::memory_order_relaxed);
        producer.registered = true;
    }
    if (producer.slot >= MaxThreads) {
        state->rejected.fetch_add(1, std::memory_order_relaxed);
    } else {
        const Entry entry{producer.sequence++, Perf::NowNs(),
                          Perf::GuestFrameCounter().load(std::memory_order_relaxed),
                          id, a, b, c, d, kind};
        if (!state->slots[producer.slot].ring.Push(entry)) {
            state->slots[producer.slot].dropped.fetch_add(1, std::memory_order_relaxed);
        }
    }
    producer.busy = false;
    state->gate.fetch_sub(2, std::memory_order_release);
}
} // namespace Common::PathTrace
