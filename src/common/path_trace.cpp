// SPDX-License-Identifier: GPL-2.0-or-later
#include "common/path_trace.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
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
constexpr u32 Capacity = 8192; // 36 MiB total, allocated only for explicit diagnostic tracing.
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
    "copy_record", "copy_publish_begin", "copy_publish_end", "gpu_dispatch_result", "frame_sample", "tuning_reload", "host_phase_begin", "host_phase_end",
    "read_producer", "read_producer_range", "read_producer_time", "read_producer_domain", "read_producer_coverage", "read_producer_window",
    "submit_queued", "submit_driver_begin", "submit_driver_end", "feedback_fence",
};
static_assert(Names.size() == static_cast<u32>(Kind::FeedbackFence) + 1);
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
                const bool drained = Drain();
                const auto now = std::chrono::steady_clock::now();
                if (now - flushed >= std::chrono::seconds{1}) {
                    file.flush();
                    io_failed |= !file.good();
                    flushed = now;
                }
                if (io_failed) break;
                if (!drained) std::this_thread::sleep_for(std::chrono::milliseconds{2});
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

void Start(const std::filesystem::path& directory) noexcept {
    if (retained) return; // One startup-only capture per process.
    try {
        auto state = std::make_unique<State>();
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
        active.store(retained, std::memory_order_release);
        LOG_INFO(Debug, "Critical path trace enabled: bounded {} byte queues, background CSV writer",
                 sizeof(retained->slots));
    } catch (const std::exception& e) {
        LOG_WARNING(Debug, "Critical path trace unavailable: {}", e.what());
    }
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
bool Active() noexcept { return active.load(std::memory_order_acquire)!=nullptr; }
u64 NextId() noexcept {
    State* state = active.load(std::memory_order_acquire);
    return state ? state->next_id.fetch_add(1, std::memory_order_relaxed) : 0;
}
void Emit(Kind kind, u64 id, u64 a, u64 b, u64 c, u64 d) noexcept {
    if (!id) return;
    State* state = active.load(std::memory_order_acquire);
    if (!state) return;
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
HostScope::HostScope(HostPhase phase_, u32 counter_) noexcept
    : phase{phase_}, counter{counter_}, id{NextId()},
      timed{Perf::GetTuning().perf_log.load(std::memory_order_relaxed) &&
            Perf::GetTuning().v3_submit_prepare.load(std::memory_order_relaxed)} {
    if (timed) start = Perf::NowNs();
    Emit(Kind::HostPhaseBegin, id, static_cast<u64>(phase));
}
HostScope::~HostScope() {
    Emit(Kind::HostPhaseEnd, id, static_cast<u64>(phase));
    if (timed) Perf::Add(static_cast<Perf::Counter>(counter), Perf::NowNs() - start);
}
} // namespace Common::PathTrace
