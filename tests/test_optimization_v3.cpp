// SPDX-License-Identifier: GPL-2.0-or-later
#include <array>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <random>
#include <set>
#include <utility>
#include "video_core/buffer_cache/v3_readback_policy.h"

static void Require(bool ok, const char* why) {
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", why); std::exit(1); }
}
int main() {
    using VideoCore::DemandPageWriters;
    using VideoCore::DemandRange;
    using VideoCore::PlanDemandBatches;
    constexpr auto P = DemandPageWriters::PageSize;
    DemandPageWriters writers;
    writers.Watch(P, {4, 0});
    writers.Watch(2 * P, {3, 0});
    writers.NoteWrite(20 * P, 1, 0, 50);
    Require(writers.Query(P, P) == DemandPageWriters::Ticks{4, 0},
            "disjoint writes cannot delay an observed page");
    writers.NoteWrite(2 * P - 1, 2, 1, 9);
    Require(writers.Query(P, 2 * P) == DemandPageWriters::Ticks{4, 9},
            "cross-page write observes both queues and both pages");
    writers.Watch(P, {999, 999});
    Require(writers.Query(P, P) == DemandPageWriters::Ticks{4, 9},
            "learning an existing page cannot replace its established provenance");
    Require(!writers.Query(3 * P, 1), "unknown page retains conservative fallback");
    Require(!writers.Query(P, 0), "empty range is not completion evidence");
    Require(!writers.Query(std::numeric_limits<std::uint64_t>::max() - 1, 3), "overflow rejected");
    writers.Clear();
    Require(!writers.Query(P, P), "unknown alias invalidation discards all cached precision");
    writers.Watch(P, {4, 0});
    writers.NoteWrite(std::numeric_limits<std::uint64_t>::max() - 1, 3, 0, 10);
    Require(!writers.Query(P, P), "invalid writes discard cached precision");

    std::mt19937 random{91201};
    for (unsigned round = 0; round < 100; ++round) {
        DemandPageWriters model;
        std::array<DemandPageWriters::Ticks, 8> expected{};
        for (unsigned i = 0; i < 8; ++i) model.Watch((i + 1) * P, {});
        for (unsigned n = 1; n <= 100; ++n) {
            const unsigned first = random() % 8, last = first + random() % (8 - first);
            const unsigned queue = random() % 2;
            model.NoteWrite((first + 1) * P, (last - first + 1) * P, queue, n);
            for (unsigned i = first; i <= last; ++i) expected[i][queue] = n;
            for (unsigned i = 0; i < 8; ++i) {
                const auto actual = model.Query((i + 1) * P, P);
                // A direct-mapped collision may lose precision; any retained entry must be exact.
                Require(!actual || *actual == expected[i], "write provenance matches independent byte-range model");
            }
        }
    }
    std::array<DemandRange, 5> fixed{{
        {2 * P, 3 * P, 0, 1}, {P, 2 * P, 0, 0}, {4 * P, 5 * P, 0, 2},
        {5 * P, 6 * P, 1, 3}, {P, 2 * P, 0, 4},
    }};
    auto plan = PlanDemandBatches(fixed, 2 * P);
    Require(plan.count == 3, "merge adjacency/overlap, never gaps or different arenas");
    Require(plan.batches[0].start == P && plan.batches[0].end == 3 * P &&
            plan.batches[0].sources == ((1ULL << 0) | (1ULL << 1) | (1ULL << 4)),
            "merged batch retains every source lifetime");
    for (unsigned round = 0; round < 1000; ++round) {
        std::array<DemandRange, 32> ranges{};
        std::set<std::pair<std::uint64_t, std::uint64_t>> before, after;
        for (unsigned i = 0; i < ranges.size(); ++i) {
            const auto page = random() % 64;
            const auto pages = 1 + random() % 2;
            const auto arena = random() % 3;
            ranges[i] = {page * P, (page + pages) * P, arena, i};
            for (auto p = page; p < page + pages; ++p) before.emplace(arena, p);
        }
        const auto result = PlanDemandBatches(ranges, 8 * P);
        std::uint64_t sources = 0;
        for (std::size_t i = 0; i < result.count; ++i) {
            const auto& batch = result.batches[i];
            Require(batch.end - batch.start <= 8 * P, "bounded batch span");
            Require((sources & batch.sources) == 0, "source appears in exactly one batch");
            sources |= batch.sources;
            for (auto p = batch.start / P; p < batch.end / P; ++p) after.emplace(batch.arena, p);
            for (unsigned j = 0; j < ranges.size(); ++j) if (batch.sources & (1ULL << j)) {
                Require(batch.arena == ranges[j].arena && batch.start <= ranges[j].start &&
                        batch.end >= ranges[j].end, "all original pages remain covered");
            }
        }
        Require(sources == ((1ULL << ranges.size()) - 1), "no source dropped");
        Require(before == after, "no speculative gaps or unobserved pages introduced");
    }
    std::puts("V3 provenance and pre-allocation batch policies passed.");
}
