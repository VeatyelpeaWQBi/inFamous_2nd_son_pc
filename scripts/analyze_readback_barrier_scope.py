# SPDX-License-Identifier: GPL-2.0-or-later
"""Join observed demand copies to the buffer-access scope recorded before them."""
import argparse
from collections import Counter, defaultdict
import csv
import json
from pathlib import Path

from analyze_super_batch import load
from analyze_timing import Event, read_path, stats

PHASES = (("baseline", -10, 0), ("early", 0, 3), ("peak", 3, 8), ("late", 8, 13))


def summarize(scopes):
    masks = Counter()
    counts = Counter(copies=len(scopes))
    for event in scopes:
        source, destination, image, complete, ranges = (
            bool(event.d & (1 << bit)) for bit in range(5))
        if event.d >= 32 or (ranges and not complete):
            raise ValueError("Invalid scope completeness flags")
        if complete and event.b & ~event.a or ranges and event.c & ~event.b:
            raise ValueError("Write stages exceed the tracked scope")
        counts["flush_requested"] += source or destination
        counts["source_conflict"] += source
        counts["destination_conflict"] += destination
        counts["image_barriers_pending"] += image
        counts["complete_write_stages"] += complete
        counts["complete_sparse_ranges"] += ranges
        if source and not destination and not image and ranges and event.c:
            counts["isolated_source_conflicts"] += 1
            counts["different_write_mask"] += event.b != event.a
            counts["different_overlapping_write_mask"] += event.c != event.a
        masks[(hex(event.a), hex(event.b), hex(event.c), event.d)] += 1
    return {**counts, "stage_masks": [
        dict(global_scope=a, all_buffer_writes=b, intersecting_sparse_writes=c,
             flags=d, count=n) for (a, b, c, d), n in masks.most_common()]}


def analyze(session, addresses):
    launch = load(session / "launch.json")
    meta = load(session / "path-trace.json")
    if (launch.get("readback_barrier_audit") is not True or not launch["complete"]
            or launch["exit_code"] != 0 or launch["progress_published"]
            or launch["source_game_written"] or not meta["complete"]
            or any(meta[key] for key in ("dropped", "io_failed", "cutoff_publishers"))):
        raise ValueError("Unqualified or incomplete barrier audit")
    rounds = load(session / "super-rounds.json")
    if len(rounds) != 1 or not rounds[0]["warm"] or not rounds[0]["meter_consumed"]:
        raise ValueError("Expected one warm release with confirmed meter consumption")
    cycle = rounds[0]
    if any(cycle[k]["gpu_profile"] for k in ("filled", "before", "after")):
        raise ValueError("Barrier scope capture must omit GPU timestamp profiling")
    release_utc = cycle["release"]["applied_unix_ns"]
    reads, samples, scopes, copies = defaultdict(list), {}, {}, set()
    count = 0
    with (session / "path-trace.csv").open(encoding="utf-8-sig", newline="") as stream:
        for row in csv.DictReader(stream):
            kind, ident = row["event"], int(row["id"])
            count += 1
            if kind.startswith("read_") or kind == "readback_barrier_scope":
                event = Event(int(row["ns"]), int(row["thread"]), kind,
                              *(int(row[k]) for k in "abcd"))
                if kind == "readback_barrier_scope":
                    if ident in scopes:
                        raise ValueError("Duplicate copy scope")
                    scopes[ident] = event
                else:
                    reads[ident].append(event)
            elif kind == "copy_record":
                copies.add(ident)
            elif kind == "frame_sample":
                samples[int(row["a"])] = int(row["b"])
    if count != meta["events"] or not scopes or set(scopes) - copies:
        raise ValueError("Trace event counts or copy IDs disagree")
    with (session / "frames.csv").open(encoding="utf-8-sig", newline="") as stream:
        frame = min((r for r in csv.DictReader(stream) if int(r["frame"]) in samples),
                    key=lambda r: abs(int(r["utc_ns"]) - release_utc))
    anchor_delta = release_utc - int(frame["utc_ns"])
    release = samples[int(frame["frame"])] + anchor_delta

    selections = defaultdict(set)
    handlers = defaultdict(list)
    matched = Counter()
    for events in reads.values():
        request = next((e for e in events if e.kind == "read_request"), None)
        if not request or request.a not in addresses or not any(
                e.kind == "read_return" for e in events):
            continue
        rel_s = (request.ns - release) / 1e9
        phase = next((tag for tag, low, high in PHASES if low <= rel_s < high), None)
        if phase is None:
            continue
        timing = read_path(events)
        handlers[phase].append(timing["total_ns"])
        ids = {e.d for e in events if e.kind == "read_copy" and not e.b and e.d in scopes}
        if ids:
            matched[phase] += 1
            selections[phase].update(ids)
    if not sum(matched.values()):
        raise ValueError("No observed graphics copy scopes linked to the selected addresses")
    return dict(session=str(session), addresses=sorted(hex(a) for a in addresses), events=count,
                scope_events=len(scopes), release_anchor_delta_ns=anchor_delta,
                phases={tag: dict(requests=stats(handlers[tag]), requests_with_scope=matched[tag],
                                  **summarize([scopes[i] for i in sorted(selections[tag])]))
                        for tag, _, _ in PHASES},
                limits="Stage masks describe access declarations in the pending scope. "
                       "Mask differences do not measure removable GPU time or certify alias safety.")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("session", type=Path)
    parser.add_argument("--address", type=lambda x: int(x, 16), action="append")
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    result = analyze(args.session, set(args.address or (0x273530404, 0x275330404)))
    args.output.write_text(json.dumps(result, indent=2) + "\n", encoding="utf8")
    for name, phase in result["phases"].items():
        print(name, {key: value for key, value in phase.items() if key != "stage_masks"})
