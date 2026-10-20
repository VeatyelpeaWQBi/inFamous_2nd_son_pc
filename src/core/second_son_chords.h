// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <algorithm>
#include <cstdint>

namespace Core::SecondSon {
// Game-side arbitration. Time is monotonic microseconds, not rendered frame count.
struct ChordSide {
    static constexpr std::uint64_t Grace = 80000;
    std::uint64_t started{};
    std::uint32_t saved_buttons{};
    float saved_trigger{};
    bool pending{}, passed{}, consumed{};

    struct Output {
        std::uint32_t buttons;
        float trigger;
        bool touch{};
    };

    Output Step(bool shoulder, unsigned trigger_byte, std::uint32_t buttons,
                float trigger, std::uint64_t now) {
        const bool active = shoulder || trigger_byte != 0;
        const bool pair = shoulder && trigger_byte >= 128;
        if (consumed) {
            if (active) {
                return {0, 0, pair}; // Do not fire a leftover key while releasing a chord.
            }
            *this = {};
            return {buttons, trigger};
        }
        if (pair) {
            consumed = true;
            pending = false;
            return {0, 0, true};
        }
        if (!pending && !passed && active) {
            pending = true;
            started = now;
        }
        if (pending) {
            saved_buttons |= buttons;
            saved_trigger = std::max(saved_trigger, trigger);
            if (!active) {
                const Output tap{saved_buttons, saved_trigger};
                *this = {};
                return tap; // Preserve a single tap shorter than the arbitration window.
            }
            if (now - started < Grace) {
                return {0, 0};
            }
            pending = false;
            passed = true;
        }
        if (!active) {
            *this = {};
        }
        return {buttons, trigger};
    }
};
} // namespace Core::SecondSon
