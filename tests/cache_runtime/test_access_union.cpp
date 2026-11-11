// SPDX-License-Identifier: GPL-2.0-or-later
#include <array>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <vector>
#include "video_core/access_range_list.h"

using VideoCore::AccessRangeList;
static void Check(bool good, const char* why) {
    if (!good) { std::cerr << "FAIL: " << why << '\n'; std::exit(1); }
}

static void Correctness() {
    IntervalList<Interval> original;
    AccessRangeList candidate;
    std::array<bool, 512> bytes{};
    u32 random = 0x128b33;
    const auto next = [&] { random = random * 1664525U + 1013904223U; return random; };
    for (u32 step = 0; step < 30000; ++step) {
        if (step % 179 == 0) { original.Clear(); candidate.Clear(); bytes.fill(false); }
        const auto start = (next() >> 16) % 512;
        const auto end = std::min<u32>(512, start + ((next() >> 16) % 129));
        original.Add({start, end}); candidate.AddUnion({start, end});
        for (u32 b = start; b < end; ++b) bytes[b] = true;
        for (u32 b = 0; b < bytes.size(); ++b) {
            Check(candidate.Contains(b) == bytes[b], "independent byte coverage");
            Check(original.Contains(b) == bytes[b], "reference byte coverage");
        }
        Check(candidate.Size() == original.Size(), "canonical range count");
        auto expected = original.begin();
        for (const auto& range : candidate) {
            Check(range.start == expected->start && range.end == expected->end, "exact interval sequence");
            ++expected;
        }
        const u32 query = (next() >> 16) % 480;
        bool any = false, all = true;
        for (u32 b = query; b < query + 32; ++b) { any |= bytes[b]; all &= bytes[b]; }
        Check(candidate.Overlaps(query, query + 32) == any, "overlap hazards");
        Check(candidate.Contains(query, query + 32) == all, "full containment");
    }
    candidate.Clear();
    const auto max = std::numeric_limits<u64>::max();
    candidate.AddUnion({max - 8, max}); candidate.AddUnion({0, 8});
    candidate.AddUnion({8, max - 8});
    Check(candidate.Size() == 1 && candidate.begin()->start == 0 && candidate.begin()->end == max,
          "adjacency and maximum endpoint without overflow");
    candidate.AddUnion({max, 0}); candidate.AddUnion({17, 17});
    Check(candidate.Size() == 1, "invalid ranges do not mutate state");
    std::cout << "access union: 30000 operations, byte oracle, exact canonical ranges, queries and endpoints passed\n";
}

static volatile u64 sink{};
template<bool Fast>
static double Bench(const std::vector<Interval>& pattern) {
    AccessRangeList ranges;
    ranges.Reserve(256);
    constexpr u32 groups = 30000;
    u64 checksum{};
    const auto begin = std::chrono::steady_clock::now();
    for (u32 group = 0; group < groups; ++group) {
        ranges.Clear();
        for (const auto& range : pattern) {
            if constexpr (Fast) ranges.AddUnion(range);
            else ranges.Add(range);
        }
        checksum += ranges.Size();
        for (const auto& range : ranges) checksum += range.start ^ range.end;
    }
    sink = checksum;
    return std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - begin).count() /
           (groups * pattern.size());
}

int main() {
    Correctness();
    for (u32 kind = 0; kind < 4; ++kind) {
        std::vector<Interval> pattern;
        if (kind == 0) pattern.push_back({0, 16384});
        for (u64 i = 0; i < 128; ++i) {
            const u64 start = kind == 3 ? ((i * 157) % 4096) : i * 64;
            pattern.push_back({start, start + (kind == 1 ? 16 : 96)});
        }
        if (kind == 2) pattern.push_back({0, 16384});
        std::cout << "case " << kind;
        for (u32 run = 0; run < 3; ++run) {
            double a, b;
            if (run & 1) { b = Bench<true>(pattern); a = Bench<false>(pattern); }
            else { a = Bench<false>(pattern); b = Bench<true>(pattern); }
            std::cout << " old/new ns " << a << '/' << b;
        }
        std::cout << '\n';
    }
}
