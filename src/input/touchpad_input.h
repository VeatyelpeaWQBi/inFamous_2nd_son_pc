// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
// Adapted from infamous2ndson_pc/gpu/shim/bbport_mouse_motion.h.
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <span>

namespace Input {

enum MouseMode { Off = 0, Joystick, Gyro, Touchpad };

struct TouchContact {
    uint16_t x{}, y{};
    uint8_t id{};
    bool operator==(const TouchContact&) const = default;
};
struct TouchFrame {
    std::array<TouchContact, 2> contacts{};
    uint8_t count{};
    bool clicked{};
    bool operator==(const TouchFrame&) const = default;
};
struct MouseSample {
    MouseMode mode{Off};
    float dx{}, dy{};
    bool trigger{}, center{};
    TouchFrame touch;
};

class MouseInput {
public:
    MouseMode Mode() const { std::scoped_lock lock{mutex}; return state.mode; }
    bool TouchActive() const { return Mode() == Touchpad; }
    bool Toggle(MouseMode mode) {
        std::scoped_lock lock{mutex};
        const bool enabled = state.mode != mode;
        ReleaseLocked();
        state.mode = enabled ? mode : Off;
        state.center = true;
        return enabled;
    }
    void Release() { std::scoped_lock lock{mutex}; ReleaseLocked(); }
    void Center() {
        std::scoped_lock lock{mutex};
        state.dx = state.dy = 0;
        state.center = true;
        if (state.mode == Touchpad) {
            state.touch = {};
            left = right = native_recent = mouse_override = false;
            queued = head = 0;
            x = 960; y = 471;
            Queue(false);
        }
    }
    MouseSample Read() {
        std::scoped_lock lock{mutex};
        auto sample = state;
        if (queued) {
            sample.touch = queue[head].frame;
            head = (head+1)%queue.size();
            --queued;
        }
        state.dx = state.dy = 0;
        state.center = false;
        return sample;
    }
    void Motion(float dx, float dy) {
        std::scoped_lock lock{mutex};
        if (!std::isfinite(dx) || !std::isfinite(dy)) return;
        if (state.mode == Gyro) {
            state.dx = std::clamp(state.dx+dx, -4096.f, 4096.f);
            state.dy = std::clamp(state.dy+dy, -4096.f, 4096.f);
        } else if (state.mode == Touchpad && !native_recent) {
            x = std::clamp(x+dx*2.f, 0.f, 1919.f);
            y = std::clamp(y+dy*2.f, 0.f, 942.f);
            SetMouseCoordinates();
            if (state.touch.count) Queue(true);
        }
    }
    void Button(bool right_button, bool down) {
        std::scoped_lock lock{mutex};
        if (state.mode == Gyro) {
            if (!right_button) state.trigger = down;
            return;
        }
        if (state.mode != Touchpad) return;
        if (right_button) right = down; else left = down;
        if (native_recent && !(left && right)) return;
        if (native_recent) { native_recent = false; mouse_override = true; }
        const auto old = state.touch.count;
        state.touch.count = left && right ? 2 : left || right ? 1 : 0;
        state.touch.clicked = right;
        if (!old && state.touch.count) state.touch.contacts[0].id = next_id++ & 127;
        if (old < 2 && state.touch.count == 2) {
            state.touch.contacts[1].id = next_id++ & 127;
            mouse_override = true;
        }
        if (!state.touch.count) mouse_override = false;
        SetMouseCoordinates();
        Queue(false);
    }
    void NativeTouch(std::span<const TouchContact> contacts, bool clicked, uint64_t now) {
        std::scoped_lock lock{mutex};
        if (state.mode != Touchpad || mouse_override) return;
        native_recent = true;
        native_tick = now;
        const bool movement = state.touch.count == std::min<size_t>(contacts.size(), 2) &&
                              state.touch.clicked == clicked;
        state.touch.count = uint8_t(std::min<size_t>(contacts.size(), 2));
        state.touch.clicked = clicked;
        for (size_t i = 0; i < state.touch.count; ++i) state.touch.contacts[i] = contacts[i];
        Queue(movement);
    }
    void Tick(uint64_t now) {
        std::scoped_lock lock{mutex};
        if (native_recent && now-native_tick > 500) {
            native_recent = false;
            if (state.touch.count || state.touch.clicked) {
                state.touch.count = 0;
                state.touch.clicked = false;
                Queue(false);
            }
        }
    }

private:
    void SetMouseCoordinates() {
        state.touch.contacts[0].x = uint16_t(std::lround(x));
        state.touch.contacts[0].y = uint16_t(std::lround(y));
        state.touch.contacts[1].x = x < 1600 ? uint16_t(x)+320 : uint16_t(x)-320;
        state.touch.contacts[1].y = uint16_t(std::lround(y));
    }
    void Queue(bool movement) {
        if (movement && queued) {
            auto& last = queue[(head+queued-1)%queue.size()];
            if (last.movement && last.frame.count == state.touch.count &&
                last.frame.clicked == state.touch.clicked &&
                last.frame.contacts[0].id == state.touch.contacts[0].id &&
                last.frame.contacts[1].id == state.touch.contacts[1].id) {
                last.frame = state.touch;
                return;
            }
        }
        if (queued == queue.size()) { head = (head+1)%queue.size(); --queued; }
        queue[(head+queued)%queue.size()] = {state.touch, movement};
        ++queued;
    }
    void ReleaseLocked() {
        state = {};
        left = right = native_recent = mouse_override = false;
        head = queued = 0;
        x = 960; y = 471;
    }
    struct QueuedTouch { TouchFrame frame; bool movement; };
    mutable std::mutex mutex;
    MouseSample state;
    std::array<QueuedTouch, 32> queue{};
    size_t head{}, queued{};
    float x{960}, y{471};
    bool left{}, right{}, native_recent{}, mouse_override{};
    uint64_t native_tick{};
    uint8_t next_id{};
};

} // namespace Input
