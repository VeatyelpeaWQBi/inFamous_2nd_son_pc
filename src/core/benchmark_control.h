// SPDX-FileCopyrightText: Copyright 2026 shadPS4-AYOUB1080p contributors
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <filesystem>
#include <string_view>
#include "common/types.h"

namespace Input {
class GameController;
}

namespace Core::Benchmark {
// Explicit local test mode only. Ordinary launches never poll command files or redirect saves.
void Configure(const std::filesystem::path& directory);
bool Enabled();
std::filesystem::path TuningPath();
void PrepareGame(std::string_view serial);
void Poll(Input::GameController& controller);
// One cheap timestamp per guest frame, buffered CSV output in explicit test mode only.
void OnFlip();
void NoteNativeCompute(bool graphics);
} // namespace Core::Benchmark
