// SPDX-FileCopyrightText: Copyright 2026 inFamous_2nd_son_pc contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#include <cstdio>
#include <cstdlib>
#include <memory>
#include "video_core/priority_compile_queue.h"
#include "video_core/buffer_cache/readback_prefetch_policy.h"

static void Require(bool ok, const char* message) {
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        std::exit(1);
    }
}

int main() {
    VideoCore::PriorityCompileQueue<int> queue;
    Require(queue.Empty() && !queue.Pop(), "empty pop");
    queue.Push(1, false);
    queue.Push(2, false);
    queue.Push(3, true);
    auto e = queue.Pop();
    Require(e && e->foreground && e->value == 3, "required compute before background");
    e = queue.Pop();
    Require(e && !e->foreground && e->value == 1, "background FIFO");
    e = queue.Pop();
    Require(e && e->value == 2 && queue.Empty(), "all work retained once");

    queue.Push(100, false);
    for (int i = 0; i < 12; ++i) {
        queue.Push(i, true);
    }
    for (int i = 0; i < 8; ++i) {
        e = queue.Pop();
        Require(e && e->foreground && e->value == i, "foreground FIFO");
    }
    e = queue.Pop();
    Require(e && !e->foreground && e->value == 100, "background cannot starve");
    for (int i = 8; i < 12; ++i) {
        e = queue.Pop();
        Require(e && e->foreground && e->value == i, "foreground resumes without reordering");
    }
    Require(!queue.Pop(), "no duplicated/dropped job");
    for (int i = 0; i < 40; ++i) queue.Push(i, true);
    for (int i = 0; i < 40; ++i) {
        e = queue.Pop();
        Require(e && e->value == i, "unopposed foreground does not wrap burst counter");
    }
    queue.Push(9, true);
    queue.Push(10, false);
    e = queue.Pop();
    Require(e && e->value == 10, "new background gets bounded service");
    e = queue.Pop();
    Require(e && e->value == 9, "remaining compute preserved");
    VideoCore::PriorityCompileQueue<std::unique_ptr<int>> owned;
    owned.Push(std::make_unique<int>(7), true);
    Require(*owned.Pop()->value == 7, "queue supports owned jobs");

    using VideoCore::DemandReadbackPages;
    constexpr std::uint64_t base = 0x100000;
    constexpr std::uint64_t page = 4096;
    auto pages = DemandReadbackPages(base + 20, 4, base, page * 4, page);
    Require(pages && pages->first == base && pages->second == base + page,
            "learn whole fault page");
    pages = DemandReadbackPages(base + page - 4, 8, base, page * 4, page);
    Require(pages && pages->first == base && pages->second == base + 2 * page,
            "cross-page read retains both pages");
    pages = DemandReadbackPages(base + page, page, base, page * 2, page);
    Require(pages && pages->first == base + page && pages->second == base + page * 2,
            "exact arena end and page size");
    Require(!DemandReadbackPages(base, 0, base, page, page), "zero read");
    Require(!DemandReadbackPages(base, page + 1, base, page * 4, page), "large invalidation is not hint");
    Require(!DemandReadbackPages(base - 1, 1, base, page, page), "before arena");
    Require(!DemandReadbackPages(base + page - 1, 2, base, page, page), "outside arena");
    Require(!DemandReadbackPages(base + 1, 1, base + 1, page, page),
            "unaligned arena cannot hide missing prefix");
    Require(!DemandReadbackPages(base + page, 1, base, page + 1, page),
            "unaligned arena cannot hide missing suffix");
    Require(!DemandReadbackPages(base, 1, base, page, 0), "zero granularity");
    Require(!DemandReadbackPages(base, 1, base, page, 3), "non-power-of-two granularity");
    constexpr auto max = ~std::uint64_t{0};
    Require(!DemandReadbackPages(max - 3, 8, 0, max, page), "read wrap");
    Require(!DemandReadbackPages(max - 3, 1, max - 5, 8, page), "arena wrap");
    Require(!DemandReadbackPages(max - 3, 1, max - page * 2, page * 2, page),
            "page alignment wrap");
    std::puts("V2 compiler scheduling and readback page-policy checks passed.");
}
