// SPDX-FileCopyrightText: Copyright 2026 inFamous_2nd_son_pc contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#include <array>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <vector>
#include "video_core/buffer_cache/feedback_policy.h"
#include "video_core/buffer_cache/readback_dependencies.h"
#ifdef _WIN64
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <cstring>
#endif

namespace {
void Check(bool ok, const char* message) {
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        std::exit(1);
    }
}
using Tracker = VideoCore::FeedbackPageWriters;
using Ticks = Tracker::Ticks;
constexpr auto Page = Tracker::PageSize;
struct Copy {
    std::uint64_t srcOffset, size;
};
#ifdef _WIN64
std::uint8_t* test_load_code{};
std::uint8_t* test_load_data{};
unsigned test_load_faults{};
LONG CALLBACK TestWordFault(EXCEPTION_POINTERS* fault) {
    auto* r = fault->ContextRecord;
    if (fault->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION ||
        r->Rip != reinterpret_cast<std::uint64_t>(test_load_code + 3) ||
        fault->ExceptionRecord->ExceptionInformation[0] != 0) return EXCEPTION_CONTINUE_SEARCH;
    const auto address = fault->ExceptionRecord->ExceptionInformation[1];
    const auto* op = reinterpret_cast<const std::uint8_t*>(r->Rip);
    if (!VideoCore::IsExactFeedbackMov32({op[0], op[1], op[2]}, r->Rax, address))
        return EXCEPTION_CONTINUE_SEARCH;
    const auto flags = r->EFlags;
    r->Rax = std::uint32_t{0xf1234567};
    r->Rip += 3;
    Check(r->EFlags == flags, "emulated MOV preserves flags");
    ++test_load_faults;
    return EXCEPTION_CONTINUE_EXECUTION;
}
void TestProtectedMov() {
    test_load_data = static_cast<std::uint8_t*>(VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    test_load_code = static_cast<std::uint8_t*>(VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    Check(test_load_data && test_load_code, "isolated native load fixture allocated");
    // Win64 ABI: MOV RAX,RCX; MOV EAX,[RAX+4]; RET.
    const std::uint8_t program[]{0x48, 0x89, 0xc8, 0x8b, 0x40, 0x04, 0xc3};
    std::memcpy(test_load_code, program, sizeof(program));
    DWORD old{};
    Check(VirtualProtect(test_load_data, 4096, PAGE_NOACCESS, &old), "protect private data page");
    Check(VirtualProtect(test_load_code, 4096, PAGE_EXECUTE_READ, &old), "make isolated load thunk executable");
    FlushInstructionCache(GetCurrentProcess(), test_load_code, sizeof(program));
    auto handler = AddVectoredExceptionHandler(1, TestWordFault);
    Check(handler != nullptr, "install fixture-only exception handler");
    using Load = std::uint64_t(*)(const void*);
    const auto load = reinterpret_cast<Load>(test_load_code);
    Check(load(test_load_data) == 0xf1234567ULL, "faulting MOV returns new word and zero-extends RAX");
    Check(load(test_load_data) == 0xf1234567ULL && test_load_faults == 2, "word reads do not unprotect their page");
    MEMORY_BASIC_INFORMATION memory{};
    Check(VirtualQuery(test_load_data, &memory, sizeof(memory)) && memory.Protect == PAGE_NOACCESS,
          "surrounding bytes remain protected");
    RemoveVectoredExceptionHandler(handler);
    VirtualFree(test_load_code, 0, MEM_RELEASE);
    VirtualFree(test_load_data, 0, MEM_RELEASE);
}
#endif
} // namespace

int main() {
    Check(VideoCore::IsFeedbackWordPrefetchProducer(0xa4edc92b, 48, false), "original producer retained");
    Check(!VideoCore::IsFeedbackWordPrefetchProducer(0x19100b24, 48, false), "new producer stays opt-in");
    Check(VideoCore::IsFeedbackWordPrefetchProducer(0x19100b24, 48, true), "observed second producer");
    Check(!VideoCore::IsFeedbackWordPrefetchProducer(0x19100b24, 480, true), "wide declarations excluded");
    Check(!VideoCore::IsFeedbackWordPrefetchProducer(0x12345678, 48, true), "unknown producers excluded");
    const std::array<VideoCore::FeedbackWordWindow, 2> windows{{{0x1000, 0x3000},
                                                              {0x4000, 0x6000}}};
    for (auto word : {0x1404ULL, 0x1604ULL, 0x2ffcULL, 0x4404ULL, 0x5004ULL}) {
        Check(VideoCore::IsFeedbackWindowWord(word, windows),
              "whole words on either observed download window are eligible");
    }
    Check(!VideoCore::IsFeedbackWindowWord(0x3404, windows), "gap between windows excluded");
    Check(!VideoCore::IsFeedbackWindowWord(0x3000, windows), "exclusive upper bound excluded");
    Check(!VideoCore::IsFeedbackWindowWord(0x1605, windows), "unaligned word excluded");
    Check(!VideoCore::IsFeedbackWindowWord(0x1404, {}), "no observed windows, no eligibility");
    Check(!VideoCore::IsFeedbackWindowWord(0x1404, {{{0x1000, 0x100000}, {}}}),
          "excessively large window cannot expand scope");
    Check(!VideoCore::IsFeedbackWindowWord(UINT64_MAX - 3, {{{UINT64_MAX - 7, UINT64_MAX}, {}}}),
          "overflowing word excluded");
    Check(VideoCore::IsFeedbackProducerSeed(0x1400, 48, 0x57341707, windows),
          "actual producer can seed exact provenance before first fault");
    Check(!VideoCore::IsFeedbackProducerSeed(0x1400, 48, 0, windows),
          "unattributed host writes do not discover speculative words");
    Check(!VideoCore::IsFeedbackProducerSeed(0x1400, 49, 1, windows) &&
          !VideoCore::IsFeedbackProducerSeed(0x3400, 48, 1, windows),
          "producer discovery remains exact length and window bounded");
    Check(VideoCore::IsExactFeedbackMov32({0x8b, 0x40, 0x04}, 0x1400, 0x1404), "exact supported MOV recognized");
    Check(!VideoCore::IsExactFeedbackMov32({0x8b, 0x40, 0x08}, 0x1400, 0x1404), "different displacement falls back");
    Check(!VideoCore::IsExactFeedbackMov32({0x8b, 0x40, 0x04}, UINT64_MAX - 2, 1), "wrapped effective address falls back");
#ifdef _WIN64
    TestProtectedMov();
#endif
    using VideoCore::IsFeedbackCheckpointWrite;
    Check(IsFeedbackCheckpointWrite(0x1000, 48, 0x1004, 4), "observed count in small output");
    Check(!IsFeedbackCheckpointWrite(0x1000, 1024, 0x1004, 4), "large declared arena excluded");
    Check(!IsFeedbackCheckpointWrite(0x1000, 48, 0x1004, 0), "page hint without actual read excluded");
    Check(!IsFeedbackCheckpointWrite(0x1000, 48, 0x102f, 4), "whole read must be written");
    Check(!IsFeedbackCheckpointWrite(0x1000, 48, 0x1100, 4), "same page alone insufficient");
    Check(!IsFeedbackCheckpointWrite(0x1000, 48, 0x1004, 16), "large reads excluded");
    Check(!IsFeedbackCheckpointWrite(~std::uint64_t{0} - 1, 4, 0, 4), "overflow excluded");

    Tracker tracker;
    Check(!tracker.Query(0, Page), "unlearned page has no proof");
    Check(!tracker.Learn(0, {}), "unknown initial writer excluded");
    Check(tracker.Learn(0, {3, 0}), "seed conservative provenance");
    tracker.NoteWrite(Page, 4, 99, 0);
    Check(tracker.Query(0, 4) == Ticks{3, 0}, "unrelated page cannot delay count");
    tracker.NoteWrite(2, 2, 100, 0);
    Check(tracker.Query(0, Page) == Ticks{100, 0}, "any partial page write is retained");
    tracker.NoteWrite(0, Page, 7, 1);
    Check(tracker.Query(0, Page) == Ticks{100, 7}, "both writer queues retained");
    Check(!tracker.Query(Page - 1, 2), "partial coverage never narrows whole copy");
    Check(tracker.Learn(Page, {101, 9}), "learn next page");
    Check(tracker.Query(Page - 1, 2) == Ticks{101, 9}, "cross-page range waits on both pages");
    tracker.Learn(0, {999, 999});
    Check(tracker.Query(0, 4) == Ticks{100, 7}, "relearning does not restore false sharing");
    tracker.NoteWrite(0, 1, 5, 2);
    Check(!tracker.Query(0, 4), "unknown queue invalidates all learned proof");
    tracker.Learn(0, {1, 0});
    tracker.NoteWrite(~std::uint64_t{0}, 2, 2, 0);
    Check(!tracker.Query(0, 4), "unrepresentable write invalidates proof");
    for (std::size_t i = 0; i < Tracker::Capacity; ++i) {
        Check(tracker.Learn(i * Page, {1, 0}), "bounded page capacity available");
    }
    Check(!tracker.Learn(Tracker::Capacity * Page, {1, 0}), "full table retains fallback");
    Check(!tracker.Query(Tracker::Capacity * Page, 1), "overflow slot is not fabricated");
    tracker.Clear();
    Check(!tracker.Query(0, 4), "off/on clearing prevents stale provenance");

    // Integration with the existing conservative copied-range dependency selection.
    tracker.Learn(0, {3, 0});
    const std::vector<Copy> copies{{0, 4}};
    auto selected = VideoCore::SelectReadbackDependencies(
        0, 0, 64 * Page, copies, Ticks{99, 0}, 5, 64 * Page,
        [&](auto start, auto size) { return tracker.Query(start, size).value_or(Ticks{99, 0}); },
        [] { return 5; });
    Check(selected == Ticks{3, 0}, "known page excludes later coarse neighbour dependency");

    // Independent per-page, per-queue oracle. The candidate may retain older conservative
    // dependencies but may NEVER return less than any actual write touching a queried page.
    tracker.Clear();
    std::array<Ticks, 128> oracle{};
    std::uint64_t random = 0x91875ab2;
    const auto next = [&] {
        random ^= random << 13;
        random ^= random >> 7;
        random ^= random << 17;
        return random;
    };
    for (std::uint64_t tick = 1; tick <= 20000; ++tick) {
        const auto first = next() % oracle.size();
        const auto bytes = 1 + next() % (3 * Page);
        const auto address = first * Page + next() % Page;
        const auto size = std::min<std::uint64_t>(bytes, oracle.size() * Page - address);
        const unsigned queue = next() % 2;
        for (auto page = address / Page; page <= (address + size - 1) / Page; ++page) {
            oracle[page][queue] = tick;
        }
        tracker.NoteWrite(address, size, tick, queue);
        const auto page = next() % oracle.size();
        Ticks coarse{};
        for (std::size_t i = page / 64 * 64; i < (page / 64 + 1) * 64; ++i) {
            for (unsigned q = 0; q < 2; ++q) coarse[q] = std::max(coarse[q], oracle[i][q]);
        }
        tracker.Learn(page * Page, coarse);
        const auto query_page = next() % oracle.size();
        if (const auto proof = tracker.Query(query_page * Page, Page)) {
            Check((*proof)[0] >= oracle[query_page][0] && (*proof)[1] >= oracle[query_page][1],
                  "provenance must dominate independent actual-write oracle");
        }
        if (tick % 4093 == 0) tracker.Clear();
    }
    VideoCore::FeedbackWordWriters words;
    Check(words.Learn(0x1404, 10), "learn aligned exact word");
    const auto word = words.Query(0x1404);
    words.NoteWrite(0x1000, 0x100, 90);
    Check(words.Query(0x1404) == word, "same-page adjacent arena cannot advance exact word");
    words.NoteWrite(0x1a00, 48, 100);
    Check(words.Query(0x1404) == word, "neighbour control structure cannot advance exact word");
    words.NoteWrite(0x1400, 48, 11);
    auto changed = words.Query(0x1404);
    Check(changed && changed->tick == 11 && changed != word, "declared overlapping write invalidates token");
    words.NoteWrite(0x1404, 4, 11);
    Check(words.Query(0x1404) != changed, "same-tick write must invalidate a private value");
    Check(!words.Learn(0x1405, 12), "unaligned private word rejected");
    words.Clear();
    Check(!words.Query(0x1404), "clearing invalidates all private value proofs");
    Check(words.Learn(0x1404, 12) && words.Query(0x1404) != changed, "relearning cannot revive old tokens");
    words.Clear();
    for (std::uint64_t i = 64; i > 0; --i)
        Check(words.Learn(0x1000 + i * 4, 1), "reverse insertion retains original capacity");
    Check(!words.Learn(0x2400, 1), "default capacity remains bounded at 64");
    for (std::uint64_t i = 0; i < words.Capacity - 64; ++i)
        Check(words.Learn(0x4000 + i * 4, 1, words.Capacity), "candidate capacity available");
    Check(!words.Learn(0x3000, 1, words.Capacity), "candidate capacity exhausted conservatively");
    const auto retained = words.Query(0x1004);
    const auto discarded = words.Query(0x4000);
    words.RetainInWindows({{{0x1000, 0x2000}, {}}});
    Check(words.Query(0x1004) == retained && !words.Query(0x4000),
          "scope pruning retains valid token and discards other scopes");
    Check(words.Learn(0x4000, 1, words.Capacity) && words.Query(0x4000) != discarded,
          "discarded and relearned words never revive stale tokens");
    words.NoteWrite(0x1007, 1, 2);
    Check(words.Query(0x1004) != retained, "last-byte write invalidates earlier aligned word");
    std::vector<std::uint64_t> overlapping;
    words.VisitOverlaps(0x1007, 2, [&](auto address) { overlapping.push_back(address); });
    Check(overlapping == std::vector<std::uint64_t>{0x1004, 0x1008},
          "one declaration discovers all learned partial-byte overlaps in address order");
    words.VisitOverlaps(UINT64_MAX - 1, 4, [&](auto) { Check(false, "wrapped hint range rejected"); });
    words.Clear();
    for (std::uint64_t i = 0; i < 64; ++i)
        Check(words.Learn(0x1000 + i * 4, 1, words.Capacity, false), "bounded producer discovery");
    Check(!words.Learn(0x2000, 1, words.Capacity, false),
          "unread fields cannot consume the actual-read reserve");
    const auto speculative = words.Query(0x1000);
    Check(words.Learn(0x1000, 99, words.Capacity) && words.Query(0x1000) == speculative,
          "first actual read promotes without reviving coarse false sharing");
    Check(words.Learn(0x2000, 1, words.Capacity, false), "promotion releases discovery quota");
    Check(words.Learn(0x3000, 1, words.Capacity), "actual read bypasses speculative quota");
    words.RetainInWindows({{{0x3000, 0x4000}, {}}});
    Check(words.Learn(0x2000, 1, words.Capacity, false), "pruning recomputes discovery quota");
    // Independent exact-byte overlap oracle, including repeated writes in one tick.
    words.Clear();
    std::array<std::uint64_t, 32> actual_ticks{}, actual_versions{};
    for (std::size_t i = 0; i < actual_ticks.size(); ++i) {
        Check(words.Learn(0x1000 + i * 16, 1), "learn independent word oracle");
        actual_ticks[i] = 1;
    }
    for (std::uint64_t write = 1; write <= 20000; ++write) {
        const auto first = 0xff0 + next() % 600;
        const auto length = 1 + next() % 64;
        const auto tick = 2 + write / 3;
        std::array<VideoCore::FeedbackWordWriters::Token, 32> before{};
        for (std::size_t i = 0; i < before.size(); ++i) before[i] = *words.Query(0x1000 + i * 16);
        words.NoteWrite(first, length, tick);
        for (std::size_t i = 0; i < before.size(); ++i) {
            const auto address = 0x1000 + i * 16;
            bool overlap = false;
            for (auto byte = first; byte < first + length; ++byte)
                for (auto part = address; part < address + 4; ++part) overlap |= byte == part;
            if (overlap) { actual_ticks[i] = tick; ++actual_versions[i]; }
            const auto after = *words.Query(address);
            Check(after.tick == actual_ticks[i] && ((after != before[i]) == overlap),
                  "word token must change exactly on independent byte-overlap oracle");
        }
    }
    std::puts("PASS: feedback qualification, bounded provenance, dependency composition and oracle.");
}
