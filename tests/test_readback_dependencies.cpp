// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <cstdio>
#include <cstdlib>
#include <vector>

#include "video_core/buffer_cache/readback_dependencies.h"

namespace {
void Require(bool value, const char* message) {
    if (!value) {
        std::fprintf(stderr, "%s\n", message);
        std::exit(1);
    }
}
struct Copy {
    std::uint64_t srcOffset, size;
};
using Ticks = std::array<std::uint64_t, 2>;
} // namespace

int main() {
    using VideoCore::SelectReadbackDependencies;
    constexpr std::uint64_t base = 0x100000;
    constexpr std::uint64_t word = 256 * 1024;
    std::uint64_t sequence = 7;
    std::vector<Copy> copies{{0, 4096}};
    const auto query = [&](std::uint64_t start, std::uint64_t size) -> Ticks {
        if (start < base + word && size <= base + word - start) {
            return {3, 0};
        }
        if (start >= base + word) {
            return {0, 9};
        }
        return {3, 9};
    };
    const auto select = [&] {
        return SelectReadbackDependencies(base, base, base + 2 * word, copies, Ticks{3, 9}, 7, word,
                                          query, [&] { return sequence; });
    };
    auto result = select();
    Require(result && *result == Ticks{3, 0}, "exclude unrelated compute writer in second word");
    copies.push_back({word, 4096});
    result = select();
    Require(result && *result == Ticks{3, 9}, "retain both queues when both words are copied");
    copies = {{word - 16, 32}};
    Require(!SelectReadbackDependencies(base, base, base + 2 * word, copies, Ticks{3, 9}, 7,
                                       word, [&](auto start, auto) {
                                           return start < base + word ? Ticks{3, 0} : Ticks{};
                                       }, [&] { return sequence; }),
            "known first word must not hide unknown writer in another word");
    result = select();
    Require(result && *result == Ticks{3, 9}, "range spanning words must retain both writers");
    copies = {{0, 4096}};
    Require(!SelectReadbackDependencies(base, base, base + 2 * word, copies, Ticks{3, 9}, 7, word,
                                       [](auto, auto) { return Ticks{}; },
                                       [&] { return sequence; }), "unknown writer must fall back");
    Require(!SelectReadbackDependencies(base, base, base + 2 * word, copies, Ticks{2, 9}, 7, word,
                                       query, [&] { return sequence; }),
            "newer writer than window snapshot must fall back");
    sequence++;
    Require(!select(), "version change before querying must fall back");
    sequence = 7;
    Require(!SelectReadbackDependencies(base, base, base + 2 * word, copies, Ticks{3, 9}, 7, word,
                                       [&](auto, auto) { ++sequence; return Ticks{3, 0}; },
                                       [&] { return sequence; }),
            "version change during querying must fall back");
    sequence = 7;
    copies = {{2 * word, 1}};
    Require(!select(), "copy outside window must fall back");
    copies = {{2 * word - 1, 2}};
    Require(!select(), "copy crossing window end must fall back");
    copies = {{0, 0}};
    Require(!select(), "empty range must fall back");
    copies = {{~std::uint64_t{0}, 1}};
    Require(!select(), "address wrap must fall back");
    copies.clear();
    Require(!select(), "empty copy list has no dependency proof");
    copies = {{0, 4096}};
    Require(!SelectReadbackDependencies(base, base + 1, base + word, copies, Ticks{3, 9}, 7, word,
                                       query, [&] { return sequence; }),
            "copy before window start must fall back");
    Require(!SelectReadbackDependencies(base, base, base + word, copies, Ticks{3, 9}, 7, 0,
                                       query, [&] { return sequence; }),
            "invalid tracking granularity must fall back");
    // A copy can touch both queues even within one tracking chunk: keep both timelines.
    result = SelectReadbackDependencies(base, base, base + word, copies, Ticks{3, 9}, 7, word,
                                       [](auto, auto) { return Ticks{3, 9}; },
                                       [&] { return sequence; });
    Require(result && *result == Ticks{3, 9}, "retain every producer queue reported by tracker");
    std::puts("Readback dependency regression checks passed.");
}
