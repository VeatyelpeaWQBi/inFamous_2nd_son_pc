// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <filesystem>
#include "common/types.h"

namespace Common::PathTrace {
enum class Kind : u32 {
    ReadRequest, ReadCpBegin, ReadCpEnd, ReadCompletionRequest, ReadDependencies,
    ReadCopy, ReadWaitBegin, ReadWaitEnd, ReadFallbackBegin, ReadFallbackEnd, ReadReturn,
    FlipRequest, FlipPrepare, FlipReady, FlipConsume, FlipPresentReturn, FlipDone,
    CopyRecord, CopyPublishBegin, CopyPublishEnd, GpuDispatchResult, FrameSample, TuningReload,
    SubmitQueued, SubmitDriverBegin, SubmitDriverEnd, FlipPoll,
    FeedbackCheckpoint,
    GpuDispatchBounds, GpuCopyBounds, GpuSessionBounds,
    WordLoadResult, WordValueCheck,
    GpuClockCalibration,
    FlipEopRegister, GnmWaitBegin, GnmWaitEnd, WordPrefetchCopy, GpuSessionWorkload,
    GpuSessionDispatch, GpuDispatchGeometry, GfxCommandCensus, ReadTargetTicks, FlipCallerSample,
    ParticleLabelWrite, ParticleLabelSchedule, CpQueueSlice, CpAscSliceWork,
    CpAscDispatchPhases, CpAscBindingPhases, CpAscBufferDetail,
    ReadbackBarrierScope, PageProtect, ComputeScopeAudit, DispatchPairCensus, DispatchPairShader,
    ComputeOutputRange, DrawBatchCensus, IndirectArgsCensus, ReadbackWaitScope,
    ReadbackWaitLayout, ReadbackWaitSpan, ReadbackWaitWriter, ComputeProtectScope,
    ComputeProtectBinding, ComputeProtectBindingCost,
    DrawPrepSlice, DrawPrepPhases0, DrawPrepPhases1, DrawPrepResources, DrawPrepBufferDetail,
    DrawPrepIdentity, DrawPrepLimit,
    DrawPrepPhase,
    ReadPrefetchJob, ReadPrefetchBatch,
};
// Explicit startup-only diagnostic. No allocation, file I/O, mutex, or wait in Emit/NextId.
// IDs identify one request, NOT one rendered frame. guest_frame is contextual SubmitDone count.
void Start(const std::filesystem::path& directory, bool paused = false) noexcept;
bool Resume() noexcept;
void Stop() noexcept;
u64 NextId() noexcept; // zero when disabled
bool IsActive() noexcept;
bool PageProtectAuditActive() noexcept;
bool ReadbackWaitScopeAuditActive() noexcept;
bool DrawPrepAuditActive() noexcept;
inline constexpr u32 DrawPrepSamplePeriod = 256;
inline constexpr u32 DrawPrepSampleCap = 65536;
bool SampleComputeProtectScope() noexcept;
class ComputeProtectionScope {
public:
    explicit ComputeProtectionScope(u64 shader_hash) noexcept;
    ~ComputeProtectionScope();
    ComputeProtectionScope(const ComputeProtectionScope&) = delete;
    ComputeProtectionScope& operator=(const ComputeProtectionScope&) = delete;

    static ComputeProtectionScope* Current() noexcept { return current; }
    void Binding(u32 index, bool is_written, u64 address = 0, u64 size = 0) noexcept;
    void Note(u64 address, u64 size, u64 view, bool no_access, u64 elapsed_ns) noexcept;

private:
    void FinishBinding() noexcept;
    inline static thread_local ComputeProtectionScope* current{};
    ComputeProtectionScope* previous{};
    u64 shader{}, calls{}, total_ns{};
    u64 last_address{}, last_end{}, last_view{};
    u32 binding{}, last_binding{}, same_binding{}, different_binding{};
    bool written{}, last_eligible{};
    bool binding_detail{};
    u64 binding_address{}, binding_size{}, binding_calls{}, binding_ns{}, binding_bytes{};
    u64 binding_min{}, binding_end{};
};
class GuestSubmitScope {
public:
    GuestSubmitScope() noexcept;
    ~GuestSubmitScope();
    GuestSubmitScope(const GuestSubmitScope&) = delete;
    GuestSubmitScope& operator=(const GuestSubmitScope&) = delete;

private:
    bool outer{};
};
void EmitFlipRegistration(u64 id, u64 buffer_id, u64 flip_arg) noexcept;
bool IsFlipCaller(u32 tid) noexcept;
bool IsSampledParticleLabel(u64 address) noexcept;
void EmitFlipCallerSample(u32 tid, u64 rip, u64 frame_return_hint, u64 label_hint,
                          u32 label_threshold) noexcept;
void Emit(Kind kind, u64 id, u64 a = 0, u64 b = 0, u64 c = 0, u64 d = 0) noexcept;
} // namespace Common::PathTrace
