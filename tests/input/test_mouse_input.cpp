// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
#include <cstdlib>
#include <iostream>
#include <limits>
#include <vector>
#include "input/mouse_motion.h"
#include "input/touchpad_frames.h"
#ifdef _WIN32
#include "input/windows_touchpad.h"
#endif

using namespace Input;
static void Check(bool value, const char* label) {
    if (!value) { std::cerr << "FAIL: " << label << '\n'; std::exit(1); }
}
static bool Near(float a, float b) { return std::abs(a-b) < 0.001f; }

static void MotionBehavior() {
    MouseMotion motion;
    uint64_t now = 1000000;
    auto sample = motion.Step(now, true, false, 0, 0);
    Check(Near(sample.acceleration[0], 1) && Near(sample.acceleration[1], 0), "upright gravity");
    Check(Near(sample.orientation[2], 0.70710678f), "upright orientation");
    motion.ResetOrientation();
    sample = motion.Step(now += 16667, true, false, 0, 0);
    Check(Near(sample.orientation[3], 1) && Near(sample.acceleration[0], 1),
          "game reset changes orientation reference without changing physical gravity");
    float peak{};
    for (int i = 0; i < 120; ++i) {
        sample = motion.Step(now += 16667, true, false, 0, i%2 ? 100 : -100);
        peak = std::max(peak, std::abs(sample.acceleration[0]-1));
        float norm{};
        for (const float v : sample.orientation) { Check(std::isfinite(v), "finite orientation"); norm += v*v; }
        Check(Near(norm, 1), "unit quaternion while shaking");
        for (const float v : sample.acceleration) Check(std::isfinite(v) && std::abs(v) <= 6.67f, "bounded shake");
    }
    Check(peak > 0.8f, "reversals produce observable shake acceleration");
    for (int i = 0; i < 120; ++i) sample = motion.Step(now += 16667, true, false, 0, 0);
    float gravity{};
    for (const auto v : sample.acceleration) gravity += v*v;
    Check(Near(gravity, 1), "idle acceleration settles to gravity");
    sample = motion.Step(now += 16667, true, true,
        std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity());
    Check(Near(sample.acceleration[0], 1), "center and nonfinite movement");
    sample = motion.Step(now += 16667, false, false, 100, 100);
    Check(sample.orientation[3] == 1 && sample.acceleration[0] == 0, "released motion is neutral");
    std::array<std::array<float, 4>, 2> final{};
    for (int rate = 0; rate < 2; ++rate) {
        MouseMotion pose;
        now = 1000000;
        for (int i = 0; i < (60 << rate); ++i)
            pose.Step(now += 1000000/(60 << rate), true, false, 2.f/(1 << rate), 0);
        for (int i = 0; i < 120; ++i) sample = pose.Step(now += 16667, true, false, 0, 0);
        final[rate] = sample.orientation;
    }
    for (size_t i = 0; i < 4; ++i) Check(Near(final[0][i], final[1][i]), "pose independent of polling rate");
}

static void MouseButtonsAndRelease() {
    MouseInput input;
    input.Toggle(Touchpad);
    input.Read();
    input.Button(false, true);
    input.Button(true, true);
    input.Button(true, false);
    input.Button(false, false);
    auto first = input.Read();
    auto two = input.Read();
    auto one = input.Read();
    auto up = input.Read();
    Check(first.touch.count == 1 && !first.touch.clicked, "left button single contact");
    Check(two.touch.count == 2 && two.touch.clicked, "both buttons two contacts and click");
    Check(two.touch.contacts[0].id != two.touch.contacts[1].id, "distinct two-finger IDs");
    Check(one.touch.count == 1 && !one.touch.clicked && up.touch.count == 0, "quick contact edges preserved");
    input.Button(true, true);
    Check(input.Read().touch.count == 1 && input.Read().touch.clicked, "right button single contact and click");
    input.Release();
    Check(input.Read().mode == Off && input.Read().touch.count == 0, "focus release clears queued contacts");
    input.Toggle(Gyro);
    input.Button(false, true);
    Check(input.Read().trigger, "left button sprays through R2 in motion mode");
    input.Toggle(Touchpad);
    Check(!input.Read().trigger, "mode switch releases spray trigger");
    for (int i = 0; i < 100; ++i) input.Button(false, i%2 == 0);
    for (int i = 0; i < 32; ++i) up = input.Read();
    Check(up.touch.count == 0, "bounded queue retains latest release on overflow");
}

static void NativeFrames() {
    MouseInput input;
    input.Toggle(Touchpad);
    TouchpadFrames frames;
    std::vector<TouchFrame> output;
    const auto publish = [&](std::span<const TouchContact> contacts, bool clicked) {
        TouchFrame frame;
        frame.count = uint8_t(contacts.size()); frame.clicked = clicked;
        std::copy(contacts.begin(), contacts.end(), frame.contacts.begin());
        output.push_back(frame);
        input.NativeTouch(contacts, clicked, 1000);
    };
    const std::array a{TouchpadFrames::Finger{17, 100, 200, true}};
    const std::array b{TouchpadFrames::Finger{29, 1500, 800, true}};
    frames.Report(10, 2, a, false, publish);
    Check(output.empty(), "hybrid report waits for both fingers");
    frames.Report(10, 0, b, false, publish);
    Check(output.size() == 1 && output.back().count == 2, "hybrid reports assembled");
    const auto original = output.back();
    const std::array reordered{b[0], a[0]};
    frames.Report(11, 2, reordered, true, publish);
    Check(output.back().contacts[0].id == original.contacts[0].id &&
          output.back().contacts[0].x == 100 && output.back().clicked, "stable IDs when report slots reorder");
    input.Button(false, true); // Windows-derived mouse event must not replace native fingers.
    Check(input.Read().touch.count == 2 && input.Read().touch.count == 2, "native reports win over derived mouse");
    Check(input.Read().touch.count == 2, "derived mouse did not enqueue a single contact");
    input.Tick(1600);
    Check(input.Read().touch.count == 0, "lost native device report releases stale contact");
    frames.Report(12, 0, {}, false, publish);
    Check(output.back().count == 0, "native contact release");
    Check(TouchpadFrames::Coordinate(500, 0, 1000, 1919) == 960, "HID coordinate scaling");
    Check(TouchpadFrames::Coordinate(2000, 0, 1000, 942) == 942, "HID coordinate clamping");
}

int main() {
    MotionBehavior();
    MouseButtonsAndRelease();
    NativeFrames();
    std::cout << "PASS: motion, shake, reset, mouse contacts, native frames, release\n";
}
