// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
// Adapted from infamous2ndson_pc/src/pad_motion.h (GPL-2.0-or-later).
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace Input {

struct MotionReport {
    std::array<float, 4> orientation{0, 0, 0, 1};
    std::array<float, 3> acceleration{}; // Orbis units: g, not SDL m/s^2.
    std::array<float, 3> angular_velocity{}; // radians/s
};

class MouseMotion {
public:
    static std::array<float, 4> Multiply(const std::array<float, 4>& a,
                                        const std::array<float, 4>& b) {
        return {a[3]*b[0]+a[0]*b[3]+a[1]*b[2]-a[2]*b[1],
                a[3]*b[1]-a[0]*b[2]+a[1]*b[3]+a[2]*b[0],
                a[3]*b[2]+a[0]*b[1]-a[1]*b[0]+a[2]*b[3],
                a[3]*b[3]-a[0]*b[0]-a[1]*b[1]-a[2]*b[2]};
    }

    static std::array<float, 4> Relative(const std::array<float, 4>& reference,
                                        const std::array<float, 4>& physical) {
        return Multiply({-reference[0], -reference[1], -reference[2], reference[3]}, physical);
    }

    void ResetOrientation() { reference = physical; }
    std::array<float, 4> Orientation() const { return Relative(reference, physical); }

    MotionReport Step(uint64_t now, bool enabled, bool center, float dx, float dy,
                      float sensitivity = 0.003f, bool roll_mode = false) {
        MotionReport report;
        if (!enabled) {
            *this = {};
            return report;
        }
        const bool fresh = !active || !time || now <= time || now - time > 250000;
        const float dt = std::max(fresh ? 1.f/60.f : float(now-time)/1000000.f, 0.0001f);
        if (fresh) {
            Center(roll_mode);
            reference = {0, 0, 0, 1};
        }
        if (center) Center(roll_mode);
        active = true;
        time = now;
        const auto previous = physical;
        if (!std::isfinite(dx)) dx = 0;
        if (!std::isfinite(dy)) dy = 0;
        if (!std::isfinite(sensitivity) || sensitivity <= 0) sensitivity = 0.003f;
        const std::array delta{std::clamp(dx, -4096.f, 4096.f),
                               std::clamp(dy, -4096.f, 4096.f)};
        const float blend = 1.f - std::exp(-dt/0.025f);
        for (size_t i = 0; i < 2; ++i) {
            target[i] = std::clamp(target[i] - delta[i]*sensitivity, -1.15f, 1.15f);
            angle[i] += (target[i]-angle[i])*blend;
            const float speed = delta[i]/dt;
            const float force = std::clamp((speed-velocity[i])*0.0001f/dt, -4.f, 4.f);
            acceleration[i] += (force-acceleration[i])*(1.f-std::exp(-dt/0.035f));
            velocity[i] = speed;
        }
        Pose(roll_mode);
        report.orientation = Orientation();
        const auto rotation = Relative(previous, physical);
        for (size_t i = 0; i < 3; ++i) {
            report.angular_velocity[i] = std::clamp(2.f*rotation[i]/dt, -20.f, 20.f);
        }
        const auto [x, y, z, s] = physical;
        const float ax = acceleration[0], ay = -acceleration[1];
        report.acceleration = {
            2*(x*y+z*s)+(1-2*(y*y+z*z))*ax+2*(x*y+z*s)*ay,
            1-2*(x*x+z*z)+2*(x*y-z*s)*ax+(1-2*(x*x+z*z))*ay,
            2*(y*z-x*s)+2*(x*z+y*s)*ax+2*(y*z-x*s)*ay};
        return report;
    }

private:
    void Pose(bool roll_mode) {
        const std::array<float, 4> yaw = roll_mode
            ? std::array<float, 4>{0, 0, std::sin(angle[0]/2), std::cos(angle[0]/2)}
            : std::array<float, 4>{0, std::sin(angle[0]/2), 0, std::cos(angle[0]/2)};
        const std::array<float, 4> pitch{std::sin(angle[1]/2), 0, 0, std::cos(angle[1]/2)};
        constexpr std::array<float, 4> upright{0, 0, 0.7071067811865475f, 0.7071067811865475f};
        physical = Multiply(Multiply(yaw, pitch), upright);
    }
    void Center(bool roll_mode) {
        target = angle = velocity = acceleration = {};
        Pose(roll_mode);
    }
    std::array<float, 2> target{}, angle{}, velocity{}, acceleration{};
    std::array<float, 4> physical{0, 0, 0, 1}, reference{0, 0, 0, 1};
    uint64_t time{};
    bool active{};
};

} // namespace Input
