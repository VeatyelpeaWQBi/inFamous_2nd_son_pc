// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <cmath>

#include "common/assert.h"
#include "common/types.h"
#include "input/controller.h"
#include "input/input_handler.h"
#include "input_mouse.h"

#include "SDL3/SDL.h"

namespace Input {

extern std::list<std::pair<InputEvent, bool>> pressed_keys;

int mouse_joystick_binding = 0;
float mouse_deadzone_offset = 0.5, mouse_speed = 1, mouse_speed_offset = 0.1250;
bool mouse_gyro_roll_mode = false;
Uint32 mouse_polling_id = 0;
static MouseInput mouse_input;
static std::mutex mouse_poll_mutex;

static bool IsCapturedMouseButton(const InputEvent& event) {
    return event.input.type == InputType::KeyboardMouse &&
           (event.input.sdl_id == SDL_BUTTON_LEFT || event.input.sdl_id == SDL_BUTTON_RIGHT);
}

static void ForgetCapturedMouseButtons() {
    const auto mode = mouse_input.Mode();
    if (mode != MouseMode::Gyro && mode != MouseMode::Touchpad) return;
    // Require a new press after releasing capture; do not reactivate a held mouse
    // button through its ordinary game binding. Physical gamepad inputs stay intact.
    pressed_keys.remove_if([](const auto& key) {
        return IsCapturedMouseButton(key.first);
    });
}

// Switches mouse to a set mode or turns mouse emulation off if it was already in that mode.
// Returns whether the mode is turned on.
bool ToggleMouseModeTo(MouseMode m) {
    std::scoped_lock lock{mouse_poll_mutex};
    ForgetCapturedMouseButtons();
    const bool enabled = mouse_input.Toggle(m);
    constexpr std::array names{"off", "joystick", "motion", "touch contacts"};
    LOG_INFO(Input, "Mouse emulation mode: {}", names[static_cast<size_t>(mouse_input.Mode())]);
    return enabled;
}

MouseMode GetMouseMode() { return mouse_input.Mode(); }
MouseInput& GetMouseInput() { return mouse_input; }
void CenterMouseInput() {
    mouse_input.Center();
    LOG_INFO(Input, "Mouse emulation center requested");
}
void ReleaseMouseInput(GameController* controller) {
    std::scoped_lock lock{mouse_poll_mutex};
    const bool active = mouse_input.Mode() != MouseMode::Off;
    ForgetCapturedMouseButtons();
    mouse_input.Release();
    controller->SetMouseInput({});
    if (active) LOG_INFO(Input, "Mouse emulation released");
}

void ProcessMouseEvent(const SDL_Event& event) {
    if (event.type == SDL_EVENT_MOUSE_MOTION) {
        mouse_input.Motion(event.motion.xrel, event.motion.yrel);
    } else if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN || event.type == SDL_EVENT_MOUSE_BUTTON_UP) {
        if (event.button.button == SDL_BUTTON_LEFT || event.button.button == SDL_BUTTON_RIGHT) {
            mouse_input.Button(event.button.button == SDL_BUTTON_RIGHT,
                               event.type == SDL_EVENT_MOUSE_BUTTON_DOWN);
        }
    }
}

void SetMouseToJoystick(int joystick) {
    std::scoped_lock lock{mouse_poll_mutex};
    mouse_joystick_binding = joystick;
}

void SetMouseParams(float mdo, float ms, float mso) {
    std::scoped_lock lock{mouse_poll_mutex};
    mouse_deadzone_offset = mdo;
    mouse_speed = ms;
    mouse_speed_offset = mso;
}

void SetMouseGyroRollMode(bool mode) {
    std::scoped_lock lock{mouse_poll_mutex};
    mouse_gyro_roll_mode = mode;
}

void EmulateJoystick(GameController* controller, u32 interval) {

    Axis axis_x, axis_y;
    switch (mouse_joystick_binding) {
    case 1:
        axis_x = Axis::LeftX;
        axis_y = Axis::LeftY;
        break;
    case 2:
        axis_x = Axis::RightX;
        axis_y = Axis::RightY;
        break;
    default:
        return; // no update needed
    }

    float d_x = 0, d_y = 0;
    SDL_GetRelativeMouseState(&d_x, &d_y);

    float output_speed =
        SDL_clamp(sqrt(d_x * d_x + d_y * d_y) * mouse_speed + mouse_speed_offset * 128,
                  mouse_deadzone_offset * 128, 128.0);

    float angle = atan2(d_y, d_x);
    float a_x = cos(angle) * output_speed, a_y = sin(angle) * output_speed;

    if (d_x == 0 && d_y == 0) a_x = a_y = 0;
    controller->Axis(axis_x, GetAxis(-0x80, 0x7f, a_x), false);
    controller->Axis(axis_y, GetAxis(-0x80, 0x7f, a_y), false);
}

void ApplyMouseInputBlockers() {
    const auto mode = GetMouseMode();
    if (mode != MouseMode::Gyro && mode != MouseMode::Touchpad) return;
    for (auto& [event, blocked] : pressed_keys) {
        if (IsCapturedMouseButton(event)) blocked = true;
    }
}

Uint32 MousePolling(void* param, Uint32 id, Uint32 interval) {
    std::scoped_lock lock{mouse_poll_mutex};
    auto* controller = static_cast<GameController*>(param);
    mouse_input.Tick(SDL_GetTicks());
    const auto sample = mouse_input.Read();
    if (sample.mode == MouseMode::Joystick) {
        EmulateJoystick(controller, interval);
    }
    controller->SetMouseInput(sample, mouse_gyro_roll_mode);
    return interval;
}

} // namespace Input
