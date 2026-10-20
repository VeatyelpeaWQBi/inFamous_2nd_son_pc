// SPDX-License-Identifier: GPL-2.0-or-later
#include "core/second_son_chords.h"
#include "core/second_son_prompts.h"
#include "core/second_son_touchpad_icons.h"
#include "input/gamepad_type.h"
#include <algorithm>
#include <array>
#include <cstdint>
#include <initializer_list>
#include <span>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {
void Require(bool value, const char* reason) {
    if (!value) {
        std::fprintf(stderr, "FAIL: %s\n", reason);
        std::exit(1);
    }
}
} // namespace
int main() {
    using namespace Core::SecondSon;
    using Input::GamepadFamily;
    for (const auto type : {SDL_GAMEPAD_TYPE_PS3, SDL_GAMEPAD_TYPE_PS4, SDL_GAMEPAD_TYPE_PS5}) {
        Require(Input::IdentifyGamepad(type) == GamepadFamily::PlayStation,
                "PlayStation models bypass Xbox changes");
    }
    for (const auto type : {SDL_GAMEPAD_TYPE_XBOX360, SDL_GAMEPAD_TYPE_XBOXONE}) {
        Require(Input::IdentifyGamepad(type) == GamepadFamily::Xbox, "Xbox families recognized");
    }
    for (const auto type : {SDL_GAMEPAD_TYPE_UNKNOWN, SDL_GAMEPAD_TYPE_STANDARD,
                           SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO}) {
        Require(Input::IdentifyGamepad(type) == GamepadFamily::Other,
                "unknown and other families never assumed Xbox");
    }
    const Input::GamepadDevice no_device;
    Require(no_device.family == GamepadFamily::None, "keyboard-only device is not Xbox");
    Require(Input::GamepadDevice{GamepadFamily::Xbox, 1, 1} !=
                Input::GamepadDevice{GamepadFamily::Xbox, 1, 2},
            "reconnect invalidates pending chord identity");
    Require(Input::GamepadDevice{GamepadFamily::Xbox, 1, 1} !=
                Input::GamepadDevice{GamepadFamily::PlayStation, 2, 1},
            "switching families invalidates pending chord identity");
    ChordSide side;
    Require(side.Step(false, 0, 0, 0, 0).buttons == 0, "idle");
    Require(side.Step(true, 0, 0x400, 0, 100).buttons == 0, "shoulder waits for chord");
    auto out = side.Step(true, 255, 0xc00, 1, 20000);
    Require(out.touch && out.buttons == 0 && out.trigger == 0, "chord consumes both actions");
    out = side.Step(false, 255, 0x800, 1, 30000);
    Require(!out.touch && out.buttons == 0 && out.trigger == 0, "release shoulder first");
    Require(side.Step(false, 0, 0, 0, 40000).buttons == 0, "release all resets");
    Require(side.Step(false, 255, 0x800, 1, 50000).buttons == 0, "trigger-first waits");
    Require(side.Step(true, 255, 0xc00, 1, 60000).touch, "reverse chord order");
    out = side.Step(true, 0, 0x400, 0, 70000);
    Require(!out.touch && out.buttons == 0, "release trigger first");
    side.Step(false, 0, 0, 0, 80000);
    Require(side.Step(true, 0, 0x400, 0, 100000).buttons == 0, "short tap waits");
    Require(side.Step(false, 0, 0, 0, 110000).buttons == 0x400, "short tap is replayed once");
    Require(side.Step(false, 0, 0, 0, 120000).buttons == 0, "no stuck replay");
    side.Step(false, 255, 0x800, 1, 200000);
    Require(side.Step(false, 255, 0x800, 1, 279999).buttons == 0, "window has fixed time");
    out = side.Step(false, 255, 0x800, 1, 280000);
    Require(out.buttons == 0x800 && out.trigger == 1 && !out.touch, "single hold resumes");
    Require(side.Step(false, 255, 0x800, 1, 300000).buttons == 0x800, "hold is continuous");
    side.Step(false, 0, 0, 0, 310000);
    Require(!side.Step(true, 127, 0xc00, .49f, 400000).touch, "half-pull threshold");
    Require(side.Step(true, 128, 0xc00, .5f, 410000).touch, "half-pull activates chord");
    ChordSide other;
    Require(other.Step(true, 255, 0x300, 1, 410000).touch, "two sides independent");

    Require(ButtonLetter('A')[0] == 14 && ButtonLetter('?')[0] == 0,
            "native alphabet has a defined fallback");
    Require(TouchpadText("Press the Touch Pad Button with both thumbs") ==
                "Press LT+LB+RT+RB", "two thumbs require both chords");
    Require(TouchpadText("Press Touch Pad Button for Details") ==
                "Press (LT+LB / RT+RB) for Details", "single press offers either side");
    Require(TouchpadText("\xe7\x94\xa8\xe5\x85\xa9\xe9\x9a\xbb\xe6\x8b\x87\xe6\x8c\x87\xe6\x8c\x89\xe4\xbd\x8f\xe8\xa7\xb8\xe7\xa2\xb0\xe6\x9d\xbf\xe6\x8c\x89\xe9\x88\x95") ==
                "\xe6\x8c\x89\xe4\xbd\x8f LT+LB+RT+RB", "Chinese two-thumb phrase");
    Require(TouchpadText("\xec\x96\x91\xec\xaa\xbd \xec\x97\x84\xec\xa7\x80 \xec\x86\x90\xea\xb0\x80\xeb\x9d\xbd\xec\x9c\xbc\xeb\xa1\x9c \xed\x84\xb0\xec\xb9\x98\xed\x8c\xa8\xeb\x93\x9c \xeb\xb2\x84\xed\x8a\xbc\xec\x9d\x84 \xea\xb8\xb8\xea\xb2\x8c") ==
                "LT+LB+RT+RB\xec\x9d\x84 \xea\xb8\xb8\xea\xb2\x8c", "Korean spaced-thumb variant");
    for (const auto swipe : {"Swipe the Touch Pad to open the door", "\xe6\xbb\x91\xe7\xa2\xb0\xe8\xa7\xb8\xe7\xa2\xb0\xe6\x9d\xbf\xe9\x96\x8b\xe9\x96\x80",
                             "\xed\x84\xb0\xec\xb9\x98\xed\x8c\xa8\xeb\x93\x9c\xeb\xa5\xbc \xec\x86\x90\xec\x9c\xbc\xeb\xa1\x9c \xea\xb7\xb8\xec\x96\xb4 \xeb\xac\xb8\xec\x9d\x84 \xec\x82\xac\xec\x9a\xa9\xed\x95\xa9\xeb\x8b\x88\xeb\x8b\xa4."}) {
        Require(TouchpadText(swipe) == swipe, "swipe instructions retain their real gesture");
    }
    const char fixture[] =
        "HEADER Touch Pad Button\0u_hint\0Press Touch Pad Button for Details\0"
        "u_pair\0Press the Touch Pad Button with both thumbs\0TAIL";
    std::vector<std::uint8_t> bytes(fixture, fixture + sizeof(fixture));
    const auto before = bytes;
    Require(ReplaceTouchpadText(bytes) == 2, "rules cover values without a scene-key whitelist");
    Require(bytes.size() == before.size(), "file length unchanged");
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        Require((bytes[i] == 0) == (before[i] == 0), "all terminators and offsets preserved");
    }
    Require(std::memcmp(bytes.data(), fixture, 23) == 0, "binary prefix retained");
    Require(ReplaceTouchpadText(bytes) == 0, "idempotent text conversion");
    const char truncated[] = "u_hint\0Touch Pad Button";
    std::vector<std::uint8_t> incomplete(truncated, truncated + sizeof(truncated) - 1);
    const auto original = incomplete;
    Require(ReplaceTouchpadText(incomplete) == 0 && incomplete == original,
            "unterminated resource rejected");

    // 14 metrics include the deliberately retained F089. Unknown areas use sentinel bytes.
    constexpr std::size_t metrics = 0x20, pixels = 0x400;
    std::vector<std::uint8_t> atlas(pixels + ButtonAtlasBytes, 0x5a);
    const auto word = [&](std::size_t at, unsigned value) {
        atlas[at] = static_cast<std::uint8_t>(value);
        atlas[at + 1] = static_cast<std::uint8_t>(value >> 8);
    };
    for (unsigned i = 0; i < 14; ++i) {
        const auto at = metrics + i * 32;
        word(at, 0xf080 + i);
        word(at + 2, 2 + (i % 7) * 64);
        word(at + 4, 2 + (i / 7) * 64);
        word(at + 6, 58);
        word(at + 8, 50);
    }
    const auto atlas_before = atlas;
    Require(ReplaceXboxAtlas(atlas, metrics, pixels) == XboxGlyphs.size(),
            "one shared atlas covers text, dynamic bindings and glyph-based images");
    Require(std::memcmp(atlas.data(), atlas_before.data(), pixels) == 0,
            "glyph metrics, layout and package metadata remain intact");
    const auto patched = atlas;
    Require(ReplaceXboxAtlas(atlas, metrics, pixels) == XboxGlyphs.size() && atlas == patched,
            "atlas conversion is idempotent");
    for (unsigned y = 0; y < 50; ++y) {
        for (unsigned x = 0; x < 58; ++x) {
            const auto at = pixels + ButtonPixelOffset(130 + x, 511 - (66 + y));
            Require(std::memcmp(atlas.data() + at, atlas_before.data() + at, 4) == 0,
                    "unidentified F089 button is never guessed");
        }
    }
    // Morton addressing must be bijective over the complete fixed-size atlas.
    std::vector<bool> seen(512 * 512);
    for (unsigned y = 0; y < 512; ++y) {
        for (unsigned x = 0; x < 512; ++x) {
            const auto at = ButtonPixelOffset(x, y) / 4;
            Require(at < seen.size() && !seen[at], "tiled pixel addressing has no overlaps");
            seen[at] = true;
        }
    }
    atlas = atlas_before;
    word(metrics + 12 * 32 + 6, 0);
    const auto invalid = atlas;
    Require(ReplaceXboxAtlas(atlas, metrics, pixels) == 0 && atlas == invalid,
            "a late invalid rectangle rejects the entire group before any write");
    Require(ReplaceXboxAtlas(atlas, metrics, atlas.size()) == 0 && atlas == invalid,
            "truncated texture rejected");
    atlas = atlas_before;
    word(metrics + 8 * 32 + 6, 24);
    const auto too_narrow = atlas;
    Require(ReplaceXboxAtlas(atlas, metrics, pixels) == 0 && atlas == too_narrow,
            "undersized labels reject before drawing or dividing by a zero scale");
    atlas = atlas_before;
    word(metrics + 13 * 32 + 2, 2);
    word(metrics + 13 * 32 + 4, 2);
    const auto overlapping = atlas;
    Require(ReplaceXboxAtlas(atlas, metrics, pixels) == 0 && atlas == overlapping,
            "overlapping glyph rectangles reject the complete patch");
    // The fingerprint is a shared BC3 image; the pair's right quad originally mirrors it.
    // Fixtures are the captured original UV/color bytes, independent of the UV writer.
    namespace Icons = Core::SecondSon::TouchpadIcons;
    constexpr std::array<std::uint8_t, 32> single_original{
        0,0,0,0x3c,255,255,255,255, 0,0x3c,0,0,255,255,255,255,
        0,0x3c,0,0x3c,255,255,255,255, 0,0,0,0,255,255,255,255};
    constexpr std::array<std::uint8_t, 64> pair_original{
        0,0x3c,0,0x3c,255,255,255,255, 0,0,0,0,255,255,255,255,
        0,0,0,0x3c,255,255,255,255, 0,0x3c,0,0,255,255,255,255,
        0,0,0,0x3c,255,255,255,255, 0,0x3c,0,0,255,255,255,255,
        0,0x3c,0,0x3c,255,255,255,255, 0,0,0,0,255,255,255,255};
    // Independent captured IEEE-754 fixtures. Keep guards around each newly written range.
    const auto words_fixture = [](std::initializer_list<std::uint32_t> words) {
        std::vector<std::uint8_t> bytes(words.size() * 4 + 2, 0xa5);
        std::size_t at = 1;
        for (const auto word : words) {
            for (unsigned b = 0; b < 4; ++b) {
                bytes[at++] = static_cast<std::uint8_t>(word >> (b * 8));
            }
        }
        return bytes;
    };
    auto single_positions = words_fixture({
        0xc21fffff,0x421fffff,0, 0x421fffff,0xc21fffff,0,
        0x421fffff,0x421fffff,0, 0xc21fffff,0xc21fffff,0});
    auto pair_positions = words_fixture({
        0x41ffffff,0x421fffff,0, 0x42dfffff,0xc21fffff,0,
        0x42dfffff,0x421fffff,0, 0x41ffffff,0xc21fffff,0,
        0xc2dfffff,0x421fffff,0, 0xc1ffffff,0xc21fffff,0,
        0xc1ffffff,0x421fffff,0, 0xc2dfffff,0xc21fffff,0});
    auto single_bounds = words_fixture({
        0xc21fffff,0xc21fffff,0xbeffffff,0x421fffff,0x421fffff,0x3effffff});
    auto pair_bounds = words_fixture({
        0xc2dfffff,0xc21fffff,0xbeffffff,0x42dfffff,0x421fffff,0x3effffff});
    const auto inner = [](auto& bytes) {
        return std::span{bytes}.subspan(1, bytes.size() - 2);
    };
    std::vector<std::uint8_t> icon_storage(Icons::TextureBytes + 2, 0xa5);
    std::array<std::uint8_t, 34> single_storage;
    std::array<std::uint8_t, 66> pair_storage;
    single_storage.fill(0xa5);
    pair_storage.fill(0xa5);
    std::copy(single_original.begin(), single_original.end(), single_storage.begin() + 1);
    std::copy(pair_original.begin(), pair_original.end(), pair_storage.begin() + 1);
    auto icon_texture = std::span{icon_storage}.subspan(1, Icons::TextureBytes);
    auto single_uv = std::span{single_storage}.subspan(1, 32);
    auto pair_uv = std::span{pair_storage}.subspan(1, 64);
    const Icons::MeshView single_mesh{single_uv, inner(single_positions), inner(single_bounds)};
    const Icons::MeshView pair_mesh{pair_uv, inner(pair_positions), inner(pair_bounds)};
    const auto positions_before = pair_positions;
    const auto bounds_before = pair_bounds;
    const auto original_icons = icon_storage;
    const std::vector<std::uint8_t> original_single(single_uv.begin(), single_uv.end());
    pair_uv.back() = 0; // Failure in the very last color byte must leave earlier targets untouched.
    const auto invalid_pair = pair_storage;
    Require(!Icons::Replace(icon_texture, single_mesh, pair_mesh) &&
                icon_storage == original_icons &&
                std::equal(single_uv.begin(), single_uv.end(), original_single.begin()) &&
                pair_storage == invalid_pair && pair_positions == positions_before &&
                pair_bounds == bounds_before, "late UV failure leaves texture, UVs and geometry intact");
    pair_uv.back() = 255;
    Require(!Icons::Replace(icon_texture.first(Icons::TextureBytes - 1), single_mesh, pair_mesh) &&
                icon_storage == original_icons, "missing final mip rejects before any write");
    pair_bounds[pair_bounds.size() - 2] ^= 1;
    const auto invalid_bounds = pair_bounds;
    Require(!Icons::Replace(icon_texture, single_mesh, pair_mesh) &&
                icon_storage == original_icons && pair_bounds == invalid_bounds &&
                pair_positions == positions_before &&
                std::equal(single_uv.begin(), single_uv.end(), original_single.begin()),
            "late bounds mismatch rejects the entire group before writing texture or geometry");
    std::copy(bounds_before.begin(), bounds_before.end(), pair_bounds.begin());
    pair_positions[pair_positions.size() - 2] ^= 1;
    const auto invalid_positions = pair_positions;
    Require(!Icons::Replace(icon_texture, single_mesh, pair_mesh) &&
                icon_storage == original_icons && pair_positions == invalid_positions &&
                pair_bounds == bounds_before &&
                std::equal(single_uv.begin(), single_uv.end(), original_single.begin()),
            "position byte mismatch rejects without publishing partial texture or UV writes");
    std::copy(positions_before.begin(), positions_before.end(), pair_positions.begin());
    Require(Icons::Replace(icon_texture, single_mesh, pair_mesh), "complete icon replacement succeeds");
    Require(icon_storage.front() == 0xa5 && icon_storage.back() == 0xa5 &&
                single_storage.front() == 0xa5 && single_storage.back() == 0xa5 &&
                pair_storage.front() == 0xa5 && pair_storage.back() == 0xa5,
            "all texture and UV range boundaries retain their canaries");
    for (const auto* bytes : {&single_positions, &pair_positions, &single_bounds, &pair_bounds}) {
        Require(bytes->front() == 0xa5 && bytes->back() == 0xa5,
                "position and bounds writes retain both guard bytes");
    }
    const auto scalar = [](std::span<const std::uint8_t> bytes, std::size_t index) {
        float value;
        std::memcpy(&value, bytes.data() + index * 4, sizeof(value));
        return value;
    };
    for (const auto& mesh : {single_mesh, pair_mesh}) {
        Require(scalar(mesh.bounds, 0) == -120 && scalar(mesh.bounds, 1) == -65 &&
                    scalar(mesh.bounds, 3) == 64 && scalar(mesh.bounds, 4) == 57,
                "both culling bounds contain the complete calibrated touchpad hint");
        for (std::size_t at = 0; at < mesh.position.size() / sizeof(float); at += 3) {
            Require(scalar(mesh.position, at) >= -120 && scalar(mesh.position, at) <= 64 &&
                        scalar(mesh.position, at + 1) >= -65 &&
                        scalar(mesh.position, at + 1) <= 57 &&
                        scalar(mesh.position, at + 2) == 0,
                    "every vertex remains on the original plane and inside its new bounds");
        }
    }
    Require(scalar(single_mesh.position, 0) == -120 &&
                scalar(single_mesh.position, 3) == 64 &&
                scalar(pair_mesh.position, 0) == -28 &&
                scalar(pair_mesh.position, 3) == 64 &&
                scalar(pair_mesh.position, 12) == -120 &&
                scalar(pair_mesh.position, 15) == -28,
            "both double hint halves move together and meet at the calibrated center");
    const auto uv_word = [](std::span<const std::uint8_t> data, std::size_t vertex, unsigned axis) {
        const auto at = vertex * 8 + axis * 2;
        return static_cast<unsigned>(data[at]) | (static_cast<unsigned>(data[at + 1]) << 8);
    };
    Require(uv_word(single_uv, 0, 0) == 0 && uv_word(single_uv, 0, 1) == 0 &&
                uv_word(single_uv, 2, 0) == 0x3c00 && uv_word(single_uv, 1, 1) == 0x3800,
            "single quad samples the full-width OR panel with smaller V at screen top");
    Require(uv_word(pair_uv, 0, 0) == 0x3800 && uv_word(pair_uv, 2, 0) == 0x3c00 &&
                uv_word(pair_uv, 4, 0) == 0 && uv_word(pair_uv, 6, 0) == 0x3800 &&
                uv_word(pair_uv, 0, 1) == 0x3800 && uv_word(pair_uv, 1, 1) == 0x3c00,
            "pair quads sample adjacent halves of the upright AND panel without mirroring");
    for (std::size_t vertex = 0; vertex < 12; ++vertex) {
        const auto data = vertex < 4 ? std::span<const std::uint8_t>{single_uv}
                                    : std::span<const std::uint8_t>{pair_uv};
        const auto index = vertex < 4 ? vertex : vertex - 4;
        for (std::size_t c = 4; c < 8; ++c) {
            Require(data[index * 8 + c] == 255, "UV updates preserve every vertex color");
        }
    }
    for (std::size_t at = 0; at < icon_texture.size(); at += 16) {
        Require(icon_texture[at] == 255 && icon_texture[at + 1] == 0 &&
                    icon_texture[at + 8] == 255 && icon_texture[at + 9] == 255,
                "all tiled blocks including small-mip padding have valid BC3 endpoints");
        for (std::size_t c = 10; c < 16; ++c) {
            Require(icon_texture[at + c] == 0, "all color selectors retain solid white");
        }
    }
    for (unsigned y = 0; y < 128; ++y) {
        for (unsigned x = 0; x < 256; ++x) {
            const auto left = Icons::PanelAlpha(x, y, Icons::Requirement::Left);
            const auto right = Icons::PanelAlpha(x, y, Icons::Requirement::Right);
            Require((x < 96 || left == 0) && (x >= 160 || right == 0),
                    "a side-specific panel has neither the opposite chord nor OR/AND");
        }
    }
    Require(Icons::PanelAlpha(10, 12, Icons::Requirement::Left) == 255 &&
                Icons::PanelAlpha(202, 12, Icons::Requirement::Right) == 255,
            "side-specific panels retain their required chord");
    // Independent BC3 alpha decoding and tiled-block reference addressing.
    constexpr std::array<std::size_t, 9> mip_offsets{
        0, 0x10000, 0x14000, 0x15000, 0x15400, 0x15800, 0x15c00, 0x16000, 0x16400};
    const auto decoded_alpha = [&](unsigned mip, unsigned x, unsigned y) {
        const auto width = 256u >> mip, pitch = std::max(8u, (width + 3) / 4);
        const auto bx = x / 4, by = y / 4;
        unsigned interleaved = 0;
        for (unsigned bit = 0; bit < 3; ++bit) {
            interleaved |= ((bx >> bit) & 1) << (bit * 2);
            interleaved |= ((by >> bit) & 1) << (bit * 2 + 1);
        }
        const auto at = mip_offsets[mip] +
            (((by / 8) * (pitch / 8) + bx / 8) * 64 + interleaved) * 16;
        std::uint64_t indices = 0;
        for (unsigned b = 0; b < 6; ++b) {
            indices |= std::uint64_t{icon_texture[at + 2 + b]} << (b * 8);
        }
        const auto index = static_cast<unsigned>((indices >> (((y % 4) * 4 + x % 4) * 3)) & 7);
        if (index == 0) { return static_cast<unsigned>(icon_texture[at]); }
        if (index == 1) { return static_cast<unsigned>(icon_texture[at + 1]); }
        return ((8 - index) * icon_texture[at] + (index - 1) * icon_texture[at + 1]) / 7;
    };
    Require(decoded_alpha(0, 10, 12) == 255 && decoded_alpha(0, 14, 12) == 0 &&
                decoded_alpha(0, 14, 30) == 255 &&
                decoded_alpha(0, 106, 54) == 0 && decoded_alpha(0, 110, 54) == 255 &&
                decoded_alpha(0, 202, 12) == 255 && decoded_alpha(0, 218, 12) == 0 &&
                decoded_alpha(0, 0, 6) == 0 && decoded_alpha(0, 6, 6) == 0,
            "wide OR panel has upright LT and RT columns, with no rectangular border");
    Require(decoded_alpha(0, 94, 182) == 0 && decoded_alpha(0, 98, 182) == 255 &&
                decoded_alpha(0, 118, 182) == 255 && decoded_alpha(0, 142, 182) == 255 &&
                decoded_alpha(0, 10, 140) == 255 && decoded_alpha(0, 202, 140) == 255,
            "bottom panel explicitly spells AND between the two chord columns");

    // Reconstruct a whole displayed 256x128 panel from the actual vertex UVs.
    // The left and right halves of the double mesh must join without losing/duplicating AND.
    const auto displayed_alpha = [&](bool pair, unsigned x, unsigned y) {
        const auto data = pair ? std::span<const std::uint8_t>{pair_uv}
                               : std::span<const std::uint8_t>{single_uv};
        const unsigned first = !pair ? 0 : x < 128 ? 4 : 0;
        const unsigned width = pair ? 128 : 256, local_x = pair ? x % 128 : x;
        const auto to_pixel = [](unsigned half) {
            Require(half == 0 || half == 0x3800 || half == 0x3c00, "known UV half-float");
            return half == 0 ? 0 : half == 0x3800 ? 128 : 256;
        };
        const auto coord = [&](unsigned axis, unsigned pixel, unsigned extent) {
            const int start = to_pixel(uv_word(data, first, axis));
            const int end = to_pixel(uv_word(data, first + 1, axis));
            return static_cast<unsigned>((start * 2 * static_cast<int>(extent) +
                (end - start) * (2 * static_cast<int>(pixel) + 1)) /
                (2 * static_cast<int>(extent)));
        };
        return decoded_alpha(0, coord(0, local_x, width), coord(1, y, 128));
    };
    for (unsigned y = 0; y < 128; ++y) {
        for (unsigned x = 0; x < 256; ++x) {
            Require(displayed_alpha(false, x, y) == Icons::Alpha(x, y) &&
                        displayed_alpha(true, x, y) == Icons::Alpha(x, y + 128),
                    "projected OR and AND panels match the complete upright image at every pixel");
        }
    }
    // Use the captured screen projection rather than assuming the background shares
    // the icon's origin. Original LT/RT columns were x=102..179 / 450..527;
    // both top rows were y=78..125. This yields about 2 screen pixels per game unit.
    constexpr float pixel_x = 184.0f / 256, pixel_y = 122.0f / 128;
    Require(4 * pixel_x > 0.98f * 3 * pixel_y &&
                4 * pixel_x < 1.02f * 3 * pixel_y,
            "calibrated overlay preserves glyph proportions within 2%");
    constexpr float screen_origin_x = 314.5f, screen_origin_y = 253.5f;
    const float ink_left = screen_origin_x + 2 * (-120 + 10 * pixel_x);
    const float ink_right = screen_origin_x + 2 * (-120 + 246 * pixel_x);
    const float ink_top = screen_origin_y - 2 * (57 - 12 * pixel_y);
    const float ink_bottom = screen_origin_y - 2 * (57 - 116 * pixel_y);
    Require(ink_left >= 86 && ink_right <= 432 && ink_top >= 158 && ink_bottom <= 366,
            "all hint ink stays inside the captured pad with at least 14-pixel margins");
    for (unsigned mip = 0; mip < 9; ++mip) {
        const auto width = 256u >> mip, scale = 1u << mip;
        for (unsigned y = 0; y < width; ++y) {
            for (unsigned x = 0; x < width; ++x) {
                unsigned coverage = 0;
                for (unsigned sy = 0; sy < scale; ++sy) {
                    for (unsigned sx = 0; sx < scale; ++sx) {
                        coverage += Icons::Alpha(x * scale + sx, y * scale + sy);
                    }
                }
                const auto expected = (coverage + scale * scale / 2) / (scale * scale);
                const auto actual = decoded_alpha(mip, x, y);
                Require((actual > expected ? actual - expected : expected - actual) <= 19,
                        "every mip matches box-filtered coverage within BC3 quantization error");
            }
        }
    }
    const auto finished_icons = icon_storage;
    Require(!Icons::Replace(icon_texture, single_mesh, pair_mesh) && icon_storage == finished_icons,
            "already changed UVs reject without rewriting the image");

    std::puts("Second Son chords, shared Xbox atlas, touchpad text and icon checks passed.");
}
