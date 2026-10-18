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
    HostPhaseBegin, HostPhaseEnd,
    ReadProducer, ReadProducerRange, ReadProducerTime, ReadProducerDomain, ReadProducerCoverage, ReadProducerWindow,
    SubmitQueued, SubmitDriverBegin, SubmitDriverEnd, FeedbackFence,
};
// Explicit startup-only diagnostic. No allocation, file I/O, mutex, or wait in Emit/NextId.
// IDs identify one request, NOT one rendered frame. guest_frame is contextual SubmitDone count.
void Start(const std::filesystem::path& directory) noexcept;
void Stop() noexcept;
bool Active() noexcept;
u64 NextId() noexcept; // zero when disabled
void Emit(Kind kind, u64 id, u64 a = 0, u64 b = 0, u64 c = 0, u64 d = 0) noexcept;
enum class HostPhase : u64 { Acquire, PresentFlush, PresentLock, PresentCall,
                             SubmitLock, QueueSubmit, Recreate };
// Existing trace transport; no clock reads when both tracing and V3 counters are disabled.
class HostScope {
public:
    HostScope(HostPhase phase, u32 counter) noexcept;
    ~HostScope();
    HostScope(const HostScope&) = delete;
    HostScope& operator=(const HostScope&) = delete;
private:
    HostPhase phase;
    u32 counter;
    u64 id{}, start{};
    bool timed{};
};
} // namespace Common::PathTrace
