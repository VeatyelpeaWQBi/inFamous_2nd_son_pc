// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "video_core/screenshot.h"

#include <atomic>

namespace VideoCore {
namespace {
std::atomic<u32> game_only_count{};
std::atomic<u32> with_overlays_count{};
} // namespace

void RequestScreenshot(const ScreenshotRequest request) {
    switch (request) {
    case ScreenshotRequest::GameOnly:
        game_only_count.fetch_add(1, std::memory_order_relaxed);
        break;
    case ScreenshotRequest::WithOverlays:
        with_overlays_count.fetch_add(1, std::memory_order_relaxed);
        break;
    case ScreenshotRequest::None:
    default:
        break;
    }
}

u32 ConsumeGameOnlyScreenshotRequests() {
    return game_only_count.exchange(0, std::memory_order_acq_rel);
}

u32 ConsumeWithOverlaysScreenshotRequests() {
    return with_overlays_count.exchange(0, std::memory_order_acq_rel);
}

} // namespace VideoCore
