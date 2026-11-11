// SPDX-License-Identifier: GPL-2.0-or-later
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <string_view>
#include <vector>
#include "video_core/buffer_cache/gpu_full_word_summary.h"

namespace {
using U64 = std::uint64_t;
constexpr U64 Full = ~U64{0};
struct Region {
    std::array<U64, 64> words{};
    VideoCore::GpuFullWordSummary summary;
    void Set(U64 word, U64 mask) {
        const U64 before = words[word];
        words[word] |= mask;
        if (before != Full && words[word] == Full) summary.AfterFullSet(word);
    }
    void Clear(U64 word, U64 mask) {
        if (words[word] == Full && mask) summary.BeforeClear(word);
        words[word] &= ~mask;
    }
    bool Query(U64 first_page, U64 last_page, bool fast) const {
        const U64 first = first_page / 64, last = last_page / 64;
        const U64 begin_mask = Full << (first_page % 64);
        const U64 end_mask = Full >> (63 - last_page % 64);
        if (first == last) {
            const U64 mask = begin_mask & end_mask;
            return (words[first] & mask) == mask;
        }
        if ((words[first] & begin_mask) != begin_mask) return false;
        if (fast) {
            if (!summary.Contains(first + 1, last - 1)) return false;
        } else {
            for (U64 word = first + 1; word < last; ++word) {
                if (words[word] != Full) return false;
            }
        }
        return (words[last] & end_mask) == end_mask;
    }
};
void Check(bool pass, const char* message) {
    if (!pass) { std::fprintf(stderr, "FAIL: %s\n", message); std::exit(1); }
}
void Verify() {
    Region region;
    std::array<bool, 4096> pages{};
    std::mt19937_64 random{0x531f12};
    for (unsigned step = 0; step < 100'000; ++step) {
        const U64 word = random() % 64;
        const U64 mask = (step % 3 == 0) ? Full : random();
        const bool set = (random() & 1) != 0;
        if (set) region.Set(word, mask); else region.Clear(word, mask);
        for (U64 bit = 0; bit < 64; ++bit) {
            if (mask & (U64{1} << bit)) pages[word * 64 + bit] = set;
        }
        U64 first = random() % 4096, last = random() % 4096;
        if (first > last) std::swap(first, last);
        bool expected = true;
        for (U64 page = first; page <= last; ++page) expected &= pages[page];
        Check(region.Query(first, last, true) == expected, "summary disagrees with page oracle");
    }
    for (U64 word = 0; word < 64; ++word) region.Set(word, Full);
    Check(region.Query(0, 4095, true), "whole region set");
    for (U64 page = 0; page < 4096; ++page) {
        region.Clear(page / 64, U64{1} << (page % 64));
        Check(!region.Query(0, 4095, true), "one cleared page must invalidate whole-region query");
        region.Set(page / 64, U64{1} << (page % 64));
        Check(region.Query(0, 4095, true), "last page republishes complete word");
    }
    Check(region.Query(64, 127, true), "single word");
    Check(region.Query(64, 128, true), "two adjacent words, empty middle");
    std::puts("PASS: page oracle, full-region clears/restores, endpoint masks");
}
void Bench() {
    constexpr U64 Count = 1024, Repeats = 2'000'000;
    std::vector<Region> regions(Count);
    for (auto& region : regions) for (U64 word = 0; word < 64; ++word) region.Set(word, Full);
    for (unsigned pass = 0; pass < 6; ++pass) {
        const bool fast = (pass & 1) != 0;
        U64 accepted = 0;
        const auto begin = std::chrono::steady_clock::now();
        for (U64 i = 0; i < Repeats; ++i) {
            const auto& region = regions[(i * 397) & (Count - 1)];
            accepted += region.Query(1, 3071, fast);
        }
        const double ns = std::chrono::duration<double, std::nano>(
                              std::chrono::steady_clock::now() - begin).count() / Repeats;
        Check(accepted == Repeats, "benchmark outputs");
        std::printf("%s %.3f ns/query (%llu)\n", fast ? "summary" : "scan", ns, accepted);
    }
}
} // namespace
int main(int argc, char** argv) {
    Verify();
    if (argc == 2 && std::string_view{argv[1]} == "--bench") Bench();
}
