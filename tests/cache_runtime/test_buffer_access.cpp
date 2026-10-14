// SPDX-FileCopyrightText: Copyright 2026 shadPS4-AYOUB1080p contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#include <array>
#include <cstdlib>
#include <iostream>
#include "video_core/buffer_access_tracker.h"

static void Check(bool value, const char* message) {
    if (!value) { std::cerr << "FAIL: " << message << '\n'; std::exit(1); }
}

int main() {
    VideoCore::BufferAccessTracker<u32> tracker;
    // Compare against byte-by-byte ownership, independently of the interval algorithm, across
    // overlapping writes, reads, adjacent gaps, barriers and recycled buffer identities.
    using Bits = std::array<bool, 512>;
    std::array<Bits, 64> reads{}, writes{};
    u32 random = 0x17ade;
    const auto next = [&] { random = random * 1664525U + 1013904223U; return random; };
    u32 identity_base = 0;
    for (u32 step = 0; step < 30000; ++step) {
        if (step % 137 == 0) {
            tracker.ResetScope();
            reads = {}; writes = {};
            identity_base ^= 0x10000;
            // Old identities and inactive retained slots must not leak accesses into a scope.
            for (u32 handle : {identity_base, identity_base ^ 0x10000}) {
                auto& access = tracker.Get(handle);
                Check(!access.read_ranges.Overlaps(0, 512), "reads cleared across barriers");
                Check(!access.write_ranges.Overlaps(0, 512), "writes cleared across barriers");
            }
        }
        const u32 slot = (next() >> 16) % 64;
        const u32 start = (next() >> 16) % 384;
        const u32 end = start + 1 + (next() >> 16) % 128;
        const bool write = (next() >> 16) & 1;
        auto& access = tracker.Get(identity_base + slot);
        bool expected_read = false, expected_write = false;
        for (u32 byte = start; byte < end; ++byte) {
            expected_read |= reads[slot][byte];
            expected_write |= writes[slot][byte];
        }
        Check(access.read_ranges.Overlaps(start, end) == expected_read, "read hazard equality");
        Check(access.write_ranges.Overlaps(start, end) == expected_write, "write hazard equality");
        (write ? access.write_ranges : access.read_ranges).Add(Interval{start, end});
        auto& expected = write ? writes[slot] : reads[slot];
        for (u32 byte = start; byte < end; ++byte) expected[byte] = true;
    }
    tracker.ResetScope();
    // Force pool growth/reallocation and then the bounded retention fallback.
    for (u32 i = 0; i < 1000; ++i) tracker.Get(i).write_ranges.Add(Interval{i, i + 1});
    tracker.ResetScope();
    for (u32 i = 0; i < 1000; ++i)
        Check(!tracker.Get(i).write_ranges.Overlaps(0, 2000), "large scope completely cleared");
    std::cout << "buffer access: differential hazards and scope recycling passed\n";
}
