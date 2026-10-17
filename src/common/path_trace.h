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
};
// Explicit startup-only diagnostic. No allocation, file I/O, mutex, or wait in Emit/NextId.
// IDs identify one request, NOT one rendered frame. guest_frame is contextual SubmitDone count.
void Start(const std::filesystem::path& directory) noexcept;
void Stop() noexcept;
u64 NextId() noexcept; // zero when disabled
void Emit(Kind kind, u64 id, u64 a = 0, u64 b = 0, u64 c = 0, u64 d = 0) noexcept;
} // namespace Common::PathTrace
