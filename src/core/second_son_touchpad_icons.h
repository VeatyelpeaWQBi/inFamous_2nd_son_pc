// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "core/second_son_prompts.h"
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string_view>
#include <vector>

namespace Core::SecondSon::TouchpadIcons {
// Verified core_common.sprig.xpps file offsets, never runtime addresses.
// The caller must verify the complete immutable pack SHA-256 before using these ranges.
inline constexpr std::size_t PackBytes = 317731196;
inline constexpr std::size_t DescriptorOffset = 0xf55adc;
inline constexpr std::size_t TextureOffset = 0xd5fd3e4, TextureBytes = 0x16800;
inline constexpr std::size_t SingleUvOffset = 0x12beea94, PairUvOffset = 0x12be5c44;
inline constexpr std::size_t SinglePositionOffset = 0x12be83b4, PairPositionOffset = 0x12bdde54;
inline constexpr std::size_t SingleBoundsOffset = 0x3761974, PairBoundsOffset = 0x3761a24;
static_assert(SingleBoundsOffset + 24 < PairBoundsOffset &&
              PairBoundsOffset + 24 < TextureOffset &&
              TextureOffset + TextureBytes < PairPositionOffset &&
              PairPositionOffset + 96 < PairUvOffset && PairUvOffset + 64 < SinglePositionOffset &&
              SinglePositionOffset + 48 < SingleUvOffset && SingleUvOffset + 32 < PackBytes);

inline constexpr std::array<std::uint32_t, 8> Descriptor{
    0x0d5f3460, 0x02500000, 0x003fc0ff, 0x96d80fac, 0x001fe000, 0, 0, 0};
inline constexpr std::array<std::uint16_t, 8> SingleBefore{
    0, 0x3c00, 0x3c00, 0, 0x3c00, 0x3c00, 0, 0};
inline constexpr std::array<std::uint16_t, 16> PairBefore{
    0x3c00, 0x3c00, 0, 0, 0, 0x3c00, 0x3c00, 0,
    0, 0x3c00, 0x3c00, 0, 0x3c00, 0x3c00, 0, 0};
// Verified original IEEE-754 words, including the original one-ULP rounding.
inline constexpr std::array<std::uint32_t, 12> SinglePositionBefore{
    0xc21fffff, 0x421fffff, 0, 0x421fffff, 0xc21fffff, 0,
    0x421fffff, 0x421fffff, 0, 0xc21fffff, 0xc21fffff, 0};
inline constexpr std::array<std::uint32_t, 24> PairPositionBefore{
    0x41ffffff, 0x421fffff, 0, 0x42dfffff, 0xc21fffff, 0,
    0x42dfffff, 0x421fffff, 0, 0x41ffffff, 0xc21fffff, 0,
    0xc2dfffff, 0x421fffff, 0, 0xc1ffffff, 0xc21fffff, 0,
    0xc1ffffff, 0x421fffff, 0, 0xc2dfffff, 0xc21fffff, 0};
inline constexpr std::array<std::uint32_t, 6> SingleBoundsBefore{
    0xc21fffff, 0xc21fffff, 0xbeffffff, 0x421fffff, 0x421fffff, 0x3effffff};
inline constexpr std::array<std::uint32_t, 6> PairBoundsBefore{
    0xc2dfffff, 0xc21fffff, 0xbeffffff, 0x42dfffff, 0x421fffff, 0x3effffff};

// Calibrated from the user's in-game screenshot, not an origin-aligned texture preview.
// Background and fingerprint instances have different placement. Move the overlay center
// left 28/down 20 game units, reduce glyphs about 20%, and tighten row spacing below.
// At the captured 2 px/unit scale, ink fits x~89..428,y~162..361 inside the pad
// (approximately x=72..448,y=144..380), leaving margins on every side.
inline constexpr float CenterX = -28;
inline constexpr float Left = -120, Right = 64, Top = 57, Bottom = -65;
inline constexpr std::array<float, 12> SinglePositionAfter{
    Left, Top, 0, Right, Bottom, 0, Right, Top, 0, Left, Bottom, 0};
inline constexpr std::array<float, 24> PairPositionAfter{
    CenterX, Top, 0, Right, Bottom, 0, Right, Top, 0, CenterX, Bottom, 0,
    Left, Top, 0, CenterX, Bottom, 0, CenterX, Top, 0, Left, Bottom, 0};
inline constexpr std::array<float, 6> BoundsAfter{Left, Bottom, -0.5f, Right, Top, 0.5f};

// Two 256x128 panels: Either (OR) above, Both (AND) below. The pair's quads
// meet at CenterX and sample adjacent halves, including the middle AND exactly once.
// Top-origin image rows need smaller V at the screen-top (+Y) vertices.
// Half values: 0x3800=0.5, 0x3c00=1.
inline constexpr std::array<std::uint16_t, 8> SingleAfter{
    0, 0, 0x3c00, 0x3800, 0x3c00, 0, 0, 0x3800};
inline constexpr std::array<std::uint16_t, 16> PairAfter{
    0x3800, 0x3800, 0x3c00, 0x3c00, 0x3c00, 0x3800, 0x3800, 0x3c00,
    0, 0x3800, 0x3800, 0x3c00, 0x3800, 0x3800, 0, 0x3c00};

inline constexpr bool TextInk(unsigned x, unsigned y, std::string_view text,
                              unsigned center, unsigned top) {
    // X=4,Y=3 compensates the atlas-to-wide-quad scale; final glyph pixels differ
    // by under 2% in aspect ratio. A square cell squeezed into a wide quad would not.
    constexpr unsigned scale_x = 4, scale_y = 3;
    const unsigned width = (static_cast<unsigned>(text.size()) * 6 - 1) * scale_x;
    const unsigned left = center - width / 2;
    if (x < left || x >= left + width || y < top || y >= top + 7 * scale_y) {
        return false;
    }
    const auto column = (x - left) / scale_x;
    return column % 6 < 5 &&
           (ButtonLetter(text[column / 6])[(y - top) / scale_y] & (16 >> (column % 6))) != 0;
}
enum class Requirement { Left, Right, Either, Both };
inline constexpr unsigned PanelAlpha(unsigned x, unsigned y, Requirement requirement) {
    if (x >= 256 || y >= 128) {
        return 0;
    }
    const bool left = requirement != Requirement::Right &&
        (TextInk(x, y, "LT", 32, 12) || TextInk(x, y, "+", 32, 53) ||
         TextInk(x, y, "LB", 32, 95));
    const bool right = requirement != Requirement::Left &&
        (TextInk(x, y, "RT", 224, 12) || TextInk(x, y, "+", 224, 53) ||
         TextInk(x, y, "RB", 224, 95));
    const bool connector =
        (requirement == Requirement::Either && TextInk(x, y, "OR", 128, 54)) ||
        (requirement == Requirement::Both && TextInk(x, y, "AND", 128, 54));
    return left || right || connector ? 255u : 0u;
}
inline constexpr unsigned Alpha(unsigned x, unsigned y) {
    // These two verified meshes encode Either and Both, not a side chosen by the player.
    // Only bind a side-specific resource to Left/Right once its requirement is identified.
    return y < 256 ? PanelAlpha(x, y % 128, y < 128 ? Requirement::Either : Requirement::Both) : 0;
}
inline constexpr std::size_t BlockOffset(unsigned x, unsigned y, unsigned pitch) {
    // Thin1DThin addressing in BC3 blocks (each block covers 4x4 texels, 16 bytes).
    const unsigned morton = (x & 1) | ((y & 1) << 1) | ((x & 2) << 1) |
                            ((y & 2) << 2) | ((x & 4) << 2) | ((y & 4) << 3);
    return (((y / 8) * (pitch / 8) + x / 8) * 64 + morton) * 16;
}

// White BC3 color with an independently encoded alpha mask. All nine mips use box-filtered
// coverage from the original procedural image, not repeatedly rounded/compressed mip data.
inline std::vector<std::uint8_t> MakeTexture() {
    std::vector<std::uint8_t> result(TextureBytes);
    constexpr std::array<unsigned, 8> palette{255, 0, 218, 182, 145, 109, 72, 36};
    std::size_t offset = 0;
    for (unsigned mip = 0; mip < 9; ++mip) {
        const unsigned width = 256u >> mip, scale = 1u << mip;
        const unsigned pitch = std::max(8u, (width + 3) / 4);
        for (unsigned by = 0; by < pitch; ++by) {
            for (unsigned bx = 0; bx < pitch; ++bx) {
                std::uint64_t indices = 0;
                for (unsigned py = 0; py < 4; ++py) {
                    for (unsigned px = 0; px < 4; ++px) {
                        const unsigned x = bx * 4 + px, y = by * 4 + py;
                        unsigned alpha = 0;
                        if (x < width && y < width) {
                            for (unsigned sy = 0; sy < scale; ++sy) {
                                for (unsigned sx = 0; sx < scale; ++sx) {
                                    alpha += Alpha(x * scale + sx, y * scale + sy);
                                }
                            }
                            alpha = (alpha + scale * scale / 2) / (scale * scale);
                        }
                        unsigned best = 0, distance = 256;
                        for (unsigned i = 0; i < palette.size(); ++i) {
                            const unsigned delta = alpha > palette[i] ? alpha - palette[i]
                                                                     : palette[i] - alpha;
                            if (delta < distance) {
                                best = i;
                                distance = delta;
                            }
                        }
                        indices |= std::uint64_t{best} << ((py * 4 + px) * 3);
                    }
                }
                const auto at = offset + BlockOffset(bx, by, pitch);
                result[at] = 255; // alpha endpoints 255 and 0
                for (unsigned i = 0; i < 6; ++i) {
                    result[at + 2 + i] = static_cast<std::uint8_t>(indices >> (i * 8));
                }
                result[at + 8] = result[at + 9] = 255; // RGB565 white; color indices stay 0
            }
        }
        offset += static_cast<std::size_t>(pitch) * pitch * 16;
    }
    return result;
}

inline bool MatchUv(std::span<const std::uint8_t> bytes,
                    std::span<const std::uint16_t> expected) {
    if (bytes.size() != expected.size() * 4) {
        return false;
    }
    for (std::size_t i = 0; i < expected.size() / 2; ++i) {
        for (std::size_t c = 0; c < 2; ++c) {
            const auto at = i * 8 + c * 2;
            const auto word = static_cast<unsigned>(bytes[at]) |
                              (static_cast<unsigned>(bytes[at + 1]) << 8);
            if (word != expected[i * 2 + c]) {
                return false;
            }
        }
        for (std::size_t c = 4; c < 8; ++c) {
            if (bytes[i * 8 + c] != 255) {
                return false;
            }
        }
    }
    return true;
}
inline void WriteUv(std::span<std::uint8_t> bytes,
                    std::span<const std::uint16_t> values) {
    for (std::size_t i = 0; i < values.size(); ++i) {
        const auto at = (i / 2) * 8 + (i % 2) * 2;
        bytes[at] = static_cast<std::uint8_t>(values[i]);
        bytes[at + 1] = static_cast<std::uint8_t>(values[i] >> 8);
    }
}
struct MeshView {
    std::span<std::uint8_t> uv, position, bounds;
};
inline bool MatchWords(std::span<const std::uint8_t> bytes,
                       std::span<const std::uint32_t> expected) {
    if (bytes.size() != expected.size_bytes()) {
        return false;
    }
    // Supported host and verified game format are both little-endian.
    return std::memcmp(bytes.data(), expected.data(), bytes.size()) == 0;
}
inline bool Replace(std::span<std::uint8_t> texture, MeshView single, MeshView pair) {
    if (texture.size() != TextureBytes || !MatchUv(single.uv, SingleBefore) ||
        !MatchUv(pair.uv, PairBefore) ||
        !MatchWords(single.position, SinglePositionBefore) ||
        !MatchWords(pair.position, PairPositionBefore) ||
        !MatchWords(single.bounds, SingleBoundsBefore) ||
        !MatchWords(pair.bounds, PairBoundsBefore)) {
        return false;
    }
    // Allocate and finish all fallible work before changing any target byte.
    const auto replacement = MakeTexture();
    std::copy(replacement.begin(), replacement.end(), texture.begin());
    WriteUv(single.uv, SingleAfter);
    WriteUv(pair.uv, PairAfter);
    std::memcpy(single.position.data(), SinglePositionAfter.data(), single.position.size());
    std::memcpy(pair.position.data(), PairPositionAfter.data(), pair.position.size());
    std::memcpy(single.bounds.data(), BoundsAfter.data(), single.bounds.size());
    std::memcpy(pair.bounds.data(), BoundsAfter.data(), pair.bounds.size());
    return true;
}
} // namespace Core::SecondSon::TouchpadIcons
