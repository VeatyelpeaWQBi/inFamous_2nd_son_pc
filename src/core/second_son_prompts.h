// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <string_view>

namespace Core::SecondSon {
struct ButtonGlyph {
    std::uint16_t code;
    std::string_view label;
};
// Keep the game's glyph codes, metrics, binding resolver and layout intact.
// F089 is not identified as a physical Xbox button; never relabel it by appearance alone.
inline constexpr std::array<ButtonGlyph, 13> XboxGlyphs{{
    {0xf080, "B"}, {0xf081, "X"}, {0xf082, "Y"}, {0xf083, "A"},
    {0xf084, "LB"}, {0xf085, "LT"}, {0xf086, "RB"}, {0xf087, "RT"},
    {0xf088, "MENU"}, {0xf08a, "LS"}, {0xf08b, "LS+"},
    {0xf08c, "RS"}, {0xf08d, "RS+"},
}};

// Small native bitmap alphabet; no installed font, renderer hook or new dependency.
inline constexpr std::array<std::uint8_t, 7> ButtonLetter(char c) {
    switch (c) {
    case 'A': return {14,17,17,31,17,17,17};
    case 'B': return {30,17,17,30,17,17,30};
    case 'D': return {30,17,17,17,17,17,30};
    case 'E': return {31,16,16,30,16,16,31};
    case 'L': return {16,16,16,16,16,16,31};
    case 'M': return {17,27,21,21,17,17,17};
    case 'N': return {17,25,25,21,19,19,17};
    case 'O': return {14,17,17,17,17,17,14};
    case 'R': return {30,17,17,30,20,18,17};
    case 'S': return {15,16,16,14,1,1,30};
    case 'T': return {31,4,4,4,4,4,4};
    case 'U': return {17,17,17,17,17,17,14};
    case 'X': return {17,17,10,4,10,17,17};
    case 'Y': return {17,17,10,4,4,4,4};
    case '+': return {0,4,4,31,4,4,0};
    default: return {};
    }
}
inline constexpr std::size_t ButtonAtlasBytes = 512 * 512 * 4;
inline constexpr std::size_t ButtonPixelOffset(unsigned x, unsigned y) {
    // Same Thin1DThin (8x8 Morton) addressing as the engine's detiler.
    const unsigned morton = (x & 1) | ((y & 1) << 1) | ((x & 2) << 1) |
                            ((y & 2) << 2) | ((x & 4) << 2) | ((y & 4) << 3);
    return (((y / 8) * 64 + x / 8) * 64 + morton) * 4;
}

// Called only after the complete source resource SHA-256 matches.
// Every rectangle is checked before any byte changes; dimensions/UVs/advances are unchanged.
inline std::size_t ReplaceXboxAtlas(std::span<std::uint8_t> bytes, std::size_t glyphs,
                                   std::size_t atlas) {
    if (atlas > bytes.size() || ButtonAtlasBytes > bytes.size() - atlas ||
        glyphs > bytes.size() || 14 * 32 > bytes.size() - glyphs ||
        (glyphs < atlas + ButtonAtlasBytes && atlas < glyphs + 14 * 32)) {
        return 0;
    }
    const auto word = [&](std::size_t at) {
        return static_cast<unsigned>(bytes[at] | (static_cast<unsigned>(bytes[at + 1]) << 8));
    };
    struct Rect { unsigned x, y, width, height; };
    std::array<Rect, XboxGlyphs.size()> rects{};
    for (std::size_t i = 0; i < XboxGlyphs.size(); ++i) {
        const auto& glyph = XboxGlyphs[i];
        const auto at = glyphs + (glyph.code - 0xf080) * 32;
        auto& r = rects[i];
        r = {word(at + 2), word(at + 4), word(at + 6), word(at + 8)};
        if (word(at) != glyph.code || r.width < 24 || r.height < 24 ||
            r.width > 64 || r.height > 64 || r.x > 512 - r.width || r.y > 512 - r.height ||
            r.width < glyph.label.size() * 6 + 7) {
            return 0;
        }
        for (std::size_t j = 0; j < i; ++j) {
            const auto& other = rects[j];
            if (r.x < other.x + other.width && other.x < r.x + r.width &&
                r.y < other.y + other.height && other.y < r.y + r.height) {
                return 0;
            }
        }
    }
    for (std::size_t i = 0; i < XboxGlyphs.size(); ++i) {
        const auto& r = rects[i];
        const auto text = XboxGlyphs[i].label;
        const unsigned text_width = static_cast<unsigned>(text.size()) * 6 - 1;
        const unsigned margin = text.size() == 1 ? 16 : 8;
        const unsigned scale = std::min((r.width - margin) / text_width, (r.height - margin) / 7);
        const unsigned left = (r.width - text_width * scale) / 2;
        const unsigned top = (r.height - 7 * scale) / 2;
        for (unsigned y = 0; y < r.height; ++y) {
            for (unsigned x = 0; x < r.width; ++x) {
                bool ink = false;
                if (x >= left && y >= top && x < left + text_width * scale &&
                    y < top + 7 * scale) {
                    const auto column = (x - left) / scale;
                    const auto letter = ButtonLetter(text[column / 6]);
                    ink = column % 6 < 5 && (letter[(y - top) / scale] & (16 >> (column % 6)));
                }
                if (text.size() == 1) {
                    const int dx = static_cast<int>(x * 2 + 1) - static_cast<int>(r.width);
                    const int dy = static_cast<int>(y * 2 + 1) - static_cast<int>(r.height);
                    const int outer = static_cast<int>(std::min(r.width, r.height)) - 4;
                    const int inner = outer - 5;
                    const int distance = dx * dx + dy * dy;
                    ink |= distance <= outer * outer && distance >= inner * inner;
                } else {
                    ink |= x >= 2 && x < r.width - 2 && y >= 2 && y < r.height - 2 &&
                           (x < 4 || x >= r.width - 4 || y < 4 || y >= r.height - 4);
                }
                // Glyph UVs use a bottom-origin Y coordinate in these verified packs.
                const auto pixel = atlas + ButtonPixelOffset(r.x + x, 511 - (r.y + y));
                bytes[pixel] = bytes[pixel + 1] = bytes[pixel + 2] = 255;
                bytes[pixel + 3] = ink ? 255 : 0;
            }
        }
    }
    return XboxGlyphs.size();
}

// Phrase rules apply to every localization value, independent of mission/key/scene.
// Longest phrases first distinguish two thumbs from a single touchpad-button press.
// Swipes never match these button-only phrases.
inline std::string TouchpadText(std::string_view text) {
    std::string result{text};
    const auto replace = [&](std::string_view from, std::string_view to) {
        for (std::size_t at = 0; (at = result.find(from, at)) != std::string::npos; at += to.size()) {
            result.replace(at, from.size(), to);
        }
    };
    replace("the Touch Pad Button with both thumbs", "LT+LB+RT+RB");
    replace("Touch Pad Button with both thumbs", "LT+LB+RT+RB");
    replace("\xe7\x94\xa8\xe5\x85\xa9\xe9\x9a\xbb\xe5\xa4\xa7\xe6\x8b\x87\xe6\x8c\x87\xe6\x8c\x89\xe4\xbd\x8f\xe8\xa7\xb8\xe7\xa2\xb0\xe6\x9d\xbf\xe6\x8c\x89\xe9\x88\x95", "\xe6\x8c\x89\xe4\xbd\x8f LT+LB+RT+RB");
    replace("\xe7\x94\xa8\xe5\x85\xa9\xe9\x9a\xbb\xe6\x8b\x87\xe6\x8c\x87\xe6\x8c\x89\xe4\xbd\x8f\xe8\xa7\xb8\xe7\xa2\xb0\xe6\x9d\xbf\xe6\x8c\x89\xe9\x88\x95", "\xe6\x8c\x89\xe4\xbd\x8f LT+LB+RT+RB");
    replace("\xec\x96\x91\xec\xaa\xbd \xec\x97\x84\xec\xa7\x80 \xec\x86\x90\xea\xb0\x80\xeb\x9d\xbd\xec\x9c\xbc\xeb\xa1\x9c \xed\x84\xb0\xec\xb9\x98\xed\x8c\xa8\xeb\x93\x9c \xeb\xb2\x84\xed\x8a\xbc", "LT+LB+RT+RB");
    replace("\xec\x96\x91\xec\xaa\xbd \xec\x97\x84\xec\xa7\x80\xec\x86\x90\xea\xb0\x80\xeb\x9d\xbd\xec\x9c\xbc\xeb\xa1\x9c \xed\x84\xb0\xec\xb9\x98\xed\x8c\xa8\xeb\x93\x9c \xeb\xb2\x84\xed\x8a\xbc", "LT+LB+RT+RB");
    replace("\xec\x96\x91\xec\x86\x90 \xec\x97\x84\xec\xa7\x80\xec\x86\x90\xea\xb0\x80\xeb\x9d\xbd\xec\x9c\xbc\xeb\xa1\x9c \xed\x84\xb0\xec\xb9\x98\xed\x8c\xa8\xeb\x93\x9c \xeb\xb2\x84\xed\x8a\xbc", "LT+LB+RT+RB");
    replace("the Touch Pad Button", "(LT+LB / RT+RB)");
    replace("Touch Pad Button", "(LT+LB / RT+RB)");
    replace("\xe8\xa7\xb8\xe7\xa2\xb0\xe6\x9d\xbf\xe6\x8c\x89\xe9\x88\x95", "(LT+LB / RT+RB)");
    replace("\xed\x84\xb0\xec\xb9\x98\xed\x8c\xa8\xeb\x93\x9c \xeb\xb2\x84\xed\x8a\xbc", "(LT+LB / RT+RB)");
    return result;
}

// Values remain NUL-terminated at their original addresses. Shortened text is padded
// only at its end; binary headers, relocations, glyph metrics and images are not scanned.
inline std::size_t ReplaceTouchpadText(std::span<std::uint8_t> bytes) {
    const auto key_char = [](std::uint8_t c) {
        return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
    };
    std::size_t changed = 0;
    for (std::size_t at = 0; at + 4 < bytes.size(); ++at) {
        if (bytes[at] != 'u' || bytes[at + 1] != '_' ||
            (at != 0 && (key_char(bytes[at - 1]) ||
                         (bytes[at - 1] >= 'A' && bytes[at - 1] <= 'Z')))) {
            continue;
        }
        auto key_end = at + 2;
        while (key_end < bytes.size() && key_end - at <= 122 && key_char(bytes[key_end])) {
            ++key_end;
        }
        if (key_end - at < 4 || key_end == bytes.size() || bytes[key_end] != 0) {
            continue;
        }
        auto end = key_end + 1;
        while (end < bytes.size() && bytes[end] != 0) {
            ++end;
        }
        if (end == bytes.size()) {
            break;
        }
        const auto first = key_end + 1;
        const std::string_view original{reinterpret_cast<const char*>(bytes.data() + first), end - first};
        if (original.find("Touch Pad Button") != std::string_view::npos ||
            original.find("\xe8\xa7\xb8\xe7\xa2\xb0\xe6\x9d\xbf\xe6\x8c\x89\xe9\x88\x95") != std::string_view::npos ||
            original.find("\xed\x84\xb0\xec\xb9\x98\xed\x8c\xa8\xeb\x93\x9c \xeb\xb2\x84\xed\x8a\xbc") != std::string_view::npos) {
            const auto text = TouchpadText(original);
            if (text != original && text.size() <= original.size()) {
                std::memcpy(bytes.data() + first, text.data(), text.size());
                std::fill(bytes.begin() + first + text.size(), bytes.begin() + end, ' ');
                ++changed;
            }
        }
        at = end;
    }
    return changed;
}
} // namespace Core::SecondSon
