// SPDX-License-Identifier: GPL-2.0-or-later
#include <cstdio>
#include <cstdlib>
#include <limits>
#include "video_core/buffer_cache/ifss_sort_write_bounds.h"

static void Require(bool ok, const char* why) {
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", why); std::exit(1); }
}
int main() {
    using namespace VideoCore;
    IfssSortDispatch dispatch{{242, 1, 1}, {64, 1, 1}, {}, 14, 1, false};
    std::array<IfssSortBuffer, 5> buffers{{
        {0x275230000, 16, 0, false}, {0x275330100, 16, 4, true, true},
        {0x24bf16c00, 16, 0, false}, {0x275230100, 1048576, 8, true},
        {0x276430000, 1048576, 8, true}}};
    const auto plan = [&] { return IfssSortWritePrefixes(dispatch, buffers); };
    auto result = plan();
    Require(result && (*result)[3] == 123904 && (*result)[4] == 123904 && (*result)[1] == 16,
            "bound every possible array store, retain the entire formatted state");
    // Independent destination model, including last lane/group and repeated sorting passes.
    for (unsigned groups = 1; groups <= 2048; groups += 7) {
        dispatch.groups[0] = groups;
        result = plan();
        Require(bool(result), "valid bounded dispatch");
        for (unsigned lane = 0; lane < 64; ++lane) {
            const std::uint64_t last_store = ((groups - 1ULL) * 64 + lane) * 8;
            Require(last_store + 8 <= (*result)[3] && last_store + 8 <= (*result)[4],
                    "all stores fit the prefix without reading a GPU count on the CPU");
        }
    }
    dispatch.groups[0] = 242;
    const auto valid = dispatch;
    dispatch.start[0] = 1; Require(!plan(), "nonzero dispatch start falls back"); dispatch = valid;
    dispatch.groups[1] = 2; Require(!plan(), "additional dimensions fall back"); dispatch = valid;
    dispatch.threads[0] = 32; Require(!plan(), "different local size falls back"); dispatch = valid;
    dispatch.user_regs = 10; Require(!plan(), "different scalar register mapping falls back"); dispatch = valid;
    dispatch.tgid_mask = 7; Require(!plan(), "different workgroup register mapping falls back"); dispatch = valid;
    dispatch.scratch = true; Require(!plan(), "scratch addressing falls back"); dispatch = valid;
    dispatch.groups[0] = std::numeric_limits<std::uint32_t>::max();
    Require(!plan(), "32-bit destination arithmetic overflow falls back"); dispatch = valid;
    const auto originals = buffers;
    buffers[4].size = 123903; Require(!plan(), "insufficient capacity falls back"); buffers = originals;
    buffers[4].stride = 4; Require(!plan(), "different addressing stride falls back"); buffers = originals;
    buffers[4].swizzled = true; Require(!plan(), "swizzled addresses fall back"); buffers = originals;
    buffers[4].add_tid = true; Require(!plan(), "lane-dependent addressing falls back"); buffers = originals;
    buffers[4].formatted = true; Require(!plan(), "formatted array stores fall back"); buffers = originals;
    buffers[4].address = buffers[3].address; Require(!plan(), "aliased descriptors fall back"); buffers = originals;
    buffers[4].address = (1ULL << 40) - 4; Require(!plan(), "address range overflow falls back");
    Require(!IsPinnedIfssSortCode({}), "missing code is never pinned");
    std::array<std::uint32_t, 177> unrelated{};
    Require(!IsPinnedIfssSortCode(unrelated), "length or short shader ID alone cannot pin code");
    std::puts("R9 sorting store bounds and conservative fallback checks passed.");
}
