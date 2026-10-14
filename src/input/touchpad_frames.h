// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
// Adapted from infamous2ndson_pc/gpu/shim/bbport_touchpad_frames.h.
#pragma once

#include "input/touchpad_input.h"

namespace Input {

class TouchpadFrames {
public:
    struct Finger { uint32_t id; uint16_t x, y; bool down; };
    template <typename Publish>
    void Report(uint32_t scan, uint32_t count, std::span<const Finger> incoming,
                bool clicked, Publish publish) {
        if (count > fingers.size()) { Reset(); publish(std::span<const TouchContact>{}, false); return; }
        if (count) {
            expected = count; received = 0; stamp = scan; pending = true;
        } else if (!pending || stamp != scan) {
            Reset(); publish(std::span<const TouchContact>{}, clicked); return;
        }
        for (const auto& finger : incoming) {
            if (received == expected) break;
            if (std::any_of(fingers.begin(), fingers.begin()+received,
                            [&](const Finger& f) { return f.id == finger.id; })) continue;
            fingers[received++] = finger;
        }
        if (received < expected) return;
        std::array<TouchContact, 2> output{};
        std::array<uint32_t, 2> live{};
        size_t active{};
        for (size_t old = 0; old < previous_count; ++old) {
            for (size_t i = 0; i < received && active < 2; ++i) {
                if (fingers[i].down && fingers[i].id == previous_ids[old]) {
                    live[active] = fingers[i].id;
                    output[active++] = {fingers[i].x, fingers[i].y, previous_guest[old]};
                }
            }
        }
        for (size_t i = 0; i < received && active < 2; ++i) {
            const auto& f = fingers[i];
            if (!f.down || std::find(live.begin(), live.begin()+active, f.id) != live.begin()+active) continue;
            uint8_t guest = next_id++ & 127;
            if (active && guest == output[0].id) guest = next_id++ & 127;
            live[active] = f.id;
            output[active++] = {f.x, f.y, guest};
        }
        previous_count = active; previous_ids = live;
        for (size_t i = 0; i < active; ++i) previous_guest[i] = output[i].id;
        pending = false;
        publish(std::span{output.data(), active}, clicked);
    }
    void Reset() { pending = false; previous_count = 0; }
    static uint16_t Coordinate(uint32_t value, int32_t minimum, int32_t maximum, uint16_t extent) {
        if (maximum <= minimum) return 0;
        const double normalized = (double(value)-minimum)/(double(maximum)-minimum);
        return uint16_t(std::clamp(normalized, 0., 1.)*extent+0.5);
    }

private:
    std::array<Finger, 5> fingers{};
    std::array<uint32_t, 2> previous_ids{};
    std::array<uint8_t, 2> previous_guest{};
    uint32_t stamp{}, expected{}, received{};
    size_t previous_count{};
    uint8_t next_id{};
    bool pending{};
};

} // namespace Input
