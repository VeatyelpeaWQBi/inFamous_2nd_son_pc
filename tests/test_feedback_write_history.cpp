// SPDX-License-Identifier: GPL-2.0-or-later
// Bounded diagnostic evidence only; no device, game, tracing runtime or test framework.
#include <array>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include "video_core/buffer_cache/feedback_write_history.h"

static void Require(bool ok, const char* message) {
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", message); std::exit(1); }
}
int main() {
    using History = VideoCore::FeedbackWriteHistory<4>;
    History h;
    Require(h.Cursor()==0 && h.OldestSequence()==0 && !h.Uncertain(), "empty history");
    const auto add = [&](unsigned seq, unsigned address, unsigned queue, unsigned command) {
        h.Record({address,8,9,seq,command,0x123,10,0,queue,3});
    };
    add(1,100,0,11);
    const auto before=h.Cursor();
    add(2,120,0,12); // newest window write is not the requested byte producer
    add(3,100,1,13);
    std::array<unsigned,4> sequences{}, scopes{};unsigned count=0;
    const auto collect = [&](const auto& w,unsigned scope) {
        sequences[count]=static_cast<unsigned>(w.sequence);scopes[count++]=scope;
    };
    h.VisitLatest(101,1,96,64,collect);
    Require(count==3 && sequences[0]==1 && scopes[0]==1 && sequences[1]==2 && scopes[1]==2 &&
            sequences[2]==3 && scopes[2]==3, "latest per queue/request and window, deduplicated");
    h.Commit(11,before,20); // must not touch writes preceding the command cursor
    bool old_untouched=false;
    h.VisitLatest(101,1,100,8,[&](const auto& w,unsigned) {
        if(w.sequence==1)old_untouched=w.recorded_ns==0;
    });
    Require(old_untouched, "commit is bounded by starting cursor");
    h.Commit(13,before,20);
    h.VisitLatest(101,1,100,8,[&](const auto& w,unsigned) {
        Require(w.recorded_ns==(w.command==13 ? 20U:0U), "only matching command is committed");
    });
    add(4,132,0,14);add(5,144,0,15);
    Require(h.Overwritten()==1 && h.OldestSequence()==2, "evicted evidence is explicit");
    count=0;h.VisitLatest(100,1,100,8,collect);
    Require(count==1 && sequences[0]==3, "evicted graphics writer must not be invented");
    const auto valid_count=h.Cursor();
    h.Record({std::numeric_limits<std::uint64_t>::max(),2,9,6});
    h.Record({100,8,9,7,0,0,0,0,2});
    Require(h.Cursor()==valid_count && h.Uncertain(), "overflow/unknown queue cannot corrupt history");
    count=0;h.VisitLatest(100,0,100,8,collect);
    Require(count==0, "zero-length reads have no candidates");
    h.InvalidateCoverage();Require(h.Uncertain(), "BDA/alias coverage cannot silently be trusted");
}
