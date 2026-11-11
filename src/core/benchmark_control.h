// SPDX-FileCopyrightText: Copyright 2026 shadPS4 contributors
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <filesystem>
#include <string_view>
#include "common/types.h"

namespace Input {
class GameController;
}

namespace Core::Benchmark {
// Session isolation and the local control protocol are also used by ordinary play.
void Configure(const std::filesystem::path& directory);
bool Enabled();
bool CaptureEnabled();
// Diagnostic SDK injection before Vulkan objects exist; ordinary launches do nothing.
void PrepareGpuDiagnostics();
// Bounded native exception evidence during profiler DLL teardown; never handles faults.
void ArmShutdownTrace();
// Only after an acknowledged diagnostic quit and successful collector export.
bool ProfilerFastExitReady();
[[noreturn]] void ExitAfterProfilerShutdown();
std::filesystem::path TuningPath();
void PrepareGame(std::string_view serial);
void Poll(Input::GameController& controller);
// One cheap timestamp per guest frame, buffered CSV output in explicit test mode only.
void OnFlip();
void NoteNativeCompute(bool graphics);
// Reuses the existing sampler's GetThreadTimes deltas; 100% means one logical processor.
void NoteThreadCpu(std::string_view name, double cpu, double kernel, double interval_seconds);
void NoteGdsCounterScope(bool reduced_conflict);
void NoteGdsCounterBarrierAvoided();
} // namespace Core::Benchmark
