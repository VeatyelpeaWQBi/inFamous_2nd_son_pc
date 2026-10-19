// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace Common {

// The development overlay's 60-frame average, independent of debug/collector state.
class FrameRateCounter {
public:
    void NewFrame(std::uint64_t time, std::uint64_t frequency, bool is_reusing_frame) {
        if (is_reusing_frame || frequency == 0) {
            return;
        }
        if (!started) {
            last_time = time;
            started = true;
            return;
        }
        if (time <= last_time) {
            return;
        }

        const double interval = static_cast<double>(time - last_time) /
                                static_cast<double>(frequency);
        last_time = time;
        seconds += interval - intervals[next];
        intervals[next] = interval;
        next = (next + 1) % intervals.size();
        if (count < intervals.size()) {
            ++count;
        }
    }

    double FramesPerSecond() const {
        return seconds > 0.0 ? static_cast<double>(count) / seconds : 0.0;
    }

private:
    std::array<double, 60> intervals{};
    std::size_t next{}, count{};
    std::uint64_t last_time{};
    double seconds{};
    bool started{};
};

} // namespace Common
