// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstdint>
#include <SDL3/SDL_gamepad.h>

namespace Input {
enum class GamepadFamily { None, PlayStation, Xbox, Other };

inline constexpr GamepadFamily IdentifyGamepad(SDL_GamepadType type) {
    switch (type) {
    case SDL_GAMEPAD_TYPE_PS3:
    case SDL_GAMEPAD_TYPE_PS4:
    case SDL_GAMEPAD_TYPE_PS5:
        return GamepadFamily::PlayStation;
    case SDL_GAMEPAD_TYPE_XBOX360:
    case SDL_GAMEPAD_TYPE_XBOXONE: // SDL also reports Xbox Series controllers in this family.
        return GamepadFamily::Xbox;
    default:
        return GamepadFamily::Other;
    }
}
struct GamepadDevice {
    GamepadFamily family = GamepadFamily::None;
    std::uint32_t id{};
    std::uint64_t generation{};
    bool operator==(const GamepadDevice&) const = default;
};
inline constexpr const char* GamepadFamilyName(GamepadFamily family) {
    switch (family) {
    case GamepadFamily::PlayStation: return "PlayStation";
    case GamepadFamily::Xbox: return "Xbox";
    case GamepadFamily::None: return "none";
    default: return "unrecognized";
    }
}
} // namespace Input
