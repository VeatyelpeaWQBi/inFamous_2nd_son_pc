// SPDX-FileCopyrightText: Copyright 2026 shadPS4-AYOUB1080p contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#include <array>
#include <cstdlib>
#include <iostream>
#include "video_core/buffer_cache/range_set.h"

static void Check(bool value, const char* message) {
    if (!value) { std::cerr << "FAIL: " << message << '\n'; std::exit(1); }
}
int main() {
    VideoCore::RangeSet ranges;
    std::array<bool, 1024> expected{};
    u32 random = 0x593abc;
    const auto next = [&] { random = random * 1664525U + 1013904223U; return random; };
    for (u32 step = 0; step < 20000; ++step) {
        const u32 start = (next() >> 16) % 896;
        const u32 size = (next() >> 16) % 128 + 1;
        if (step % 157 == 0) { ranges.Clear(); expected = {}; }
        if (step % 3 == 0) {
            ranges.Subtract(start, size);
            for (u32 i = start; i < start + size; ++i) expected[i] = false;
        } else {
            ranges.Add(start, size);
            ranges.Add(start, size); // Common repeated particle record-pool insertion.
            for (u32 i = start; i < start + size; ++i) expected[i] = true;
        }
        std::array<bool, 1024> actual{};
        ranges.ForEach([&](VAddr begin, VAddr end) {
            Check(begin < end && end <= actual.size(), "valid right-open ranges");
            for (u64 i = begin; i < end; ++i) actual[i] = true;
        });
        Check(actual == expected, "add/subtract/clear equality with byte ownership");
    }
    ranges.Clear();
    ranges.Add(32, 64); ranges.Subtract(48, 16); ranges.Add(32, 64);
    Check(ranges.Contains(32, 64), "subtraction invalidates a previous positive witness");
    ranges.Clear(); ranges.Add(32, 64);
    Check(ranges.Contains(32, 64), "clear invalidates a previous positive witness");
    std::cout << "range set: byte ownership and witness invalidation passed\n";
}
