// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "common/types.h"

namespace VideoCore {

enum class ScreenshotRequest : u32 {
    None = 0,
    GameOnly = 1,
    WithOverlays = 2,
};

// Ordinary screenshots are independent of frame debuggers and profiling tools.
void RequestScreenshot(ScreenshotRequest request);
u32 ConsumeGameOnlyScreenshotRequests();
u32 ConsumeWithOverlaysScreenshotRequests();

} // namespace VideoCore
