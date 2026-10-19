// SPDX-License-Identifier: GPL-2.0-or-later
#include "common/frame_rate.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace {
void Require(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "%s\n", message);
        std::exit(1);
    }
}
bool Near(double actual, double expected) {
    return std::abs(actual - expected) < 1e-8;
}
} // namespace

int main() {
    Common::FrameRateCounter counter;
    Require(counter.FramesPerSecond() == 0.0, "No frame intervals must mean no FPS estimate.");
    std::uint64_t time = 0;
    counter.NewFrame(time, 60000, false);
    Require(counter.FramesPerSecond() == 0.0, "The first frame must not invent a 60 FPS sample.");
    for (int i = 0; i < 120; ++i) {
        time += 1000;
        counter.NewFrame(time, 60000, false);
        Require(Near(counter.FramesPerSecond(), 60.0),
                "Warm-up and a full ring must both report the actual 60 FPS.");
    }
    for (int i = 0; i < 60; ++i) {
        time += 2000;
        counter.NewFrame(time, 60000, false);
    }
    Require(Near(counter.FramesPerSecond(), 30.0),
            "The latest 60 intervals must replace the previous frame-rate history.");

    Common::FrameRateCounter reused;
    reused.NewFrame(0, 1000, false);
    reused.NewFrame(10, 1000, true);
    reused.NewFrame(20, 1000, true);
    reused.NewFrame(40, 1000, false);
    Require(Near(reused.FramesPerSecond(), 25.0),
            "Overlay redraws must neither count as game frames nor shorten their interval.");
    reused.NewFrame(40, 1000, false);
    reused.NewFrame(30, 1000, false);
    reused.NewFrame(80, 0, false);
    Require(Near(reused.FramesPerSecond(), 25.0),
            "Invalid timestamps or frequency must not alter the estimate.");
    reused.NewFrame(80, 1000, false);
    Require(Near(reused.FramesPerSecond(), 25.0),
            "An invalid timing sample must not advance the reference timestamp.");

    std::puts("Frame-rate regressions passed: warm-up, sliding window, reused frames and invalid timing.");
}
