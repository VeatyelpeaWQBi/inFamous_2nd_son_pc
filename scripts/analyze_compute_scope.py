# SPDX-License-Identifier: GPL-2.0-or-later
"""Read the bounded counts for barriers that only see already-synchronized buffers."""
import argparse
from collections import Counter, defaultdict
import csv
import json
from pathlib import Path
from statistics import median

from analyze_super_batch import load
from analyze_timing import FIELDS


def summarize_output_ranges(records):
    counts = Counter(records=len(records))
    sizes, excess = [], []
    for address, size, packed, adjust in records:
        groups_x, active = packed >> 32, packed & 0xffffffff
        if not address or not size or adjust % 2 or address % 2 or not 0 < groups_x <= 65535:
            counts["unsupported"] += 1
            continue
        size_written = min(groups_x * 64, active, size // 2) * 2
        def pages(length):
            return ((address % 4096 + length + 4095) // 4096) if length else 0
        extra_pages = pages(size) - pages(size_written)
        counts["eligible_for_estimate"] += 1
        counts["empty_output"] += size_written == 0
        counts["smaller_byte_ranges"] += size_written < size
        counts["smaller_page_ranges"] += extra_pages > 0
        counts["declared_bytes"] += size
        counts["possible_written_bytes"] += size_written
        counts["declared_page_visits"] += pages(size)
        counts["possible_written_page_visits"] += pages(size_written)
        sizes.append(size)
        excess.append(extra_pages)
    return dict(**counts, median_declared_size=median(sizes) if sizes else None,
                max_extra_pages=max(excess, default=0),
                extra_page_histogram=dict(sorted(Counter(excess).items())))


def analyze(session):
    launch = load(session / "launch.json")
    meta = load(session / "path-trace.json")
    cycle, = load(session / "super-rounds.json")
    if (launch.get("compute_scope_audit") is not True or not launch["complete"]
            or launch["exit_code"] != 0 or launch["progress_published"]
            or launch["source_game_written"] or not cycle["warm"] or not cycle["meter_consumed"]
            or not meta["complete"] or any(meta[k] for k in
                ("dropped", "io_failed", "cutoff_publishers", "gpu_profile_at_start",
                 "compute_hash_at_start"))
            or not all(cycle[key]["compute_barrier_scope"] for key in ("filled", "before", "after"))):
        raise ValueError("Expected a warm, isolated compute scope audit")
    batches, frames, last = [], {}, {}
    pair_batches, shader_batches = {}, defaultdict(dict)
    output_ranges, draw_batches, indirect_batches = [], [], []
    indirect_categories = (
        "static_zero", "backing_zero_count", "backing_zero_draw", "backing_nonzero_draw",
        "gpu_count", "gpu_draw", "backing_multi_unchecked", "unknown",
    )
    with (session / "path-trace.csv").open(encoding="utf-8-sig", newline="") as handle:
        reader = csv.DictReader(handle)
        if reader.fieldnames != FIELDS:
            raise ValueError("Unexpected trace schema")
        for count, row in enumerate(reader, 1):
            tid, seq, ns, ident, a, b, c, d = (
                int(row[k]) for k in ("thread", "seq", "ns", "id", "a", "b", "c", "d"))
            prev_seq, prev_ns = last.get(tid, (-1, 0))
            if not (0 <= tid < meta["registered_threads"] and ident > 0
                    and seq == prev_seq + 1 and ns >= prev_ns):
                raise ValueError("Discontinuous trace")
            last[tid] = seq, ns
            if row["event"] == "compute_scope_audit":
                if not 0 < a <= ns or b != 256 or not 0 <= d <= c <= b:
                    raise ValueError("Malformed barrier counts")
                batches.append((a, ns, b, c, d))
            elif row["event"] == "dispatch_pair_census":
                same, single = c & 0xffffffff, c >> 32
                independent, gds, opaque, independent_single = (
                    d >> shift & 0xffff for shift in range(0, 64, 16))
                if (ident in pair_batches or not 0 < a <= ns or b != 256
                        or not 0 <= independent_single <= min(single, independent)
                        or not 0 <= single <= same <= b or independent + gds + opaque > same):
                    raise ValueError("Malformed dispatch pair census")
                pair_batches[ident] = (a, ns, dict(
                    dispatches=b, same_pipeline=same, both_single_group=single,
                    no_declared_hazard=independent, gds_only_hazard=gds, opaque=opaque,
                    both_single_no_declared_hazard=independent_single,
                    other_buffer_hazard=same-independent-gds-opaque))
            elif row["event"] == "dispatch_pair_shader":
                if ident not in pair_batches or a in shader_batches[ident] or not 0 < b+c <= 256:
                    raise ValueError("Malformed shader attribution")
                shader_batches[ident][a] = b, c
            elif row["event"] == "compute_output_range":
                if d > 255:
                    raise ValueError("Output offset exceeds push data field")
                output_ranges.append((ns, a, b, c, d))
            elif row["event"] == "draw_batch_census":
                draws, direct = b & 0xffffffff, b >> 32
                indexed, adjacent_indexed = c & 0xffffffff, c >> 32
                adjacent_nonindexed, longest = d & 0xffffffff, d >> 32
                adjacent = adjacent_indexed + adjacent_nonindexed
                if (not 0 < a <= ns or draws != 1024 or direct > draws
                        or indexed > draws or adjacent_indexed > indexed
                        or adjacent_nonindexed > draws - indexed or adjacent > draws - direct
                        or longest > 256 or (longest == 0) != (direct == draws)
                        or (longest > 1) != (adjacent > 0)):
                    raise ValueError("Malformed draw batch census")
                draw_batches.append((a, ns, longest, dict(
                    draws=draws, indexed=indexed, inline=direct,
                    adjacent_indexed=adjacent_indexed, adjacent_nonindexed=adjacent_nonindexed)))
            elif row["event"] == "indirect_args_census":
                draws, indexed = b & 0xffffffff, b >> 32
                counts = [value >> shift & 0xffff for value in (c, d)
                          for shift in range(0, 64, 16)]
                if not 0 < a <= ns or draws != 128 or sum(counts) != draws or indexed > draws:
                    raise ValueError("Malformed indirect argument census")
                indirect_batches.append((a, ns, dict(
                    draws=draws, indexed=indexed, **dict(zip(indirect_categories, counts)))))
            elif row["event"] == "frame_sample" and a not in frames:
                frames[a] = b
            else:
                raise ValueError("Unexpected trace row")
    if count != meta["events"] or not batches or not frames:
        raise ValueError("Missing required trace records")
    utc = cycle["release"]["applied_unix_ns"]
    with (session / "frames.csv").open(encoding="utf-8-sig", newline="") as handle:
        frame = min((row for row in csv.DictReader(handle) if int(row["frame"]) in frames),
                    key=lambda row: abs(int(row["utc_ns"]) - utc))
    delta = utc - int(frame["utc_ns"])
    if abs(delta) > 50_000_000:
        raise ValueError("Frame anchor too far from release")
    release = frames[int(frame["frame"])] + delta
    phases = {}
    pair_phases = {}
    output_phases = {}
    draw_phases = {}
    indirect_phases = {}
    for phase, lo, hi in (("baseline", -10, 0), ("early", 0, 3),
                          ("peak", 3, 8), ("late", 8, 13)):
        counts = Counter()
        for begin, end, attempts, comparable, old_only in batches:
            if lo * 10**9 <= begin - release and end - release < hi * 10**9:
                counts.update(attempts=attempts, narrowed_after_prior=comparable,
                              only_previous_conflicts=old_only)
        phases[phase] = dict(counts)
        pair_counts, independent_shaders, gds_shaders = Counter(), Counter(), Counter()
        for ident, (begin, end, pair) in pair_batches.items():
            if lo * 10**9 <= begin - release and end - release < hi * 10**9:
                pair_counts.update(pair)
                for shader, (independent, gds) in shader_batches[ident].items():
                    independent_shaders[f"{shader:08x}"] += independent
                    gds_shaders[f"{shader:08x}"] += gds
        pair_phases[phase] = dict(
            **pair_counts, independent_shaders=+independent_shaders, gds_shaders=+gds_shaders)
        if output_ranges:
            output_phases[phase] = summarize_output_ranges(
                [record[1:] for record in output_ranges if lo * 10**9 <= record[0] - release < hi * 10**9])
        if draw_batches:
            draw_counts = Counter()
            longest = 0
            for begin, end, run, draws in draw_batches:
                if lo * 10**9 <= begin - release and end - release < hi * 10**9:
                    draw_counts.update(draws)
                    longest = max(longest, run)
            draw_phases[phase] = dict(draw_counts, longest_run=longest)
        if indirect_batches:
            indirect_counts = Counter()
            for begin, end, draws in indirect_batches:
                if lo * 10**9 <= begin - release and end - release < hi * 10**9:
                    indirect_counts.update(draws)
            indirect_phases[phase] = dict(indirect_counts)
    return dict(session=str(session), events=count, batches=len(batches),
                release_anchor_delta_ns=delta, phases=phases,
                dispatch_pairs=pair_phases if pair_batches else {},
                draw_batches=dict(
                    batches=len(draw_batches), phases=draw_phases,
                    limits="Counts runs of same kind, instance count and first instance among "
                           "consecutive commands in one recorder stream. Inline draws and "
                           "indirect draws are not tested for batching. Counts precede shader "
                           "draw-index, query and render compatibility checks."),
                indirect_args=dict(
                    batches=len(indirect_batches), phases=indirect_phases,
                    limits="Records indirect draws reaching argument-buffer preparation. GPU "
                           "ownership is checked at page granularity. Backing results describe "
                           "a diagnostic snapshot and do not prove the eventual command input "
                           "or eligibility to skip attachment and resource side effects."),
                output_range_5b1e2375=dict(
                    records=len(output_ranges), reached_cap=len(output_ranges) >= 131072,
                    phases=output_phases,
                    limits="Derived from a pinned shader family's store formula and the final "
                           "binding. Page visits include repetitions and do not measure actual "
                           "protection changes. Indirect and independent queue dispatches excluded."),
                limits="Includes only complete 256-attempt batches wholly inside each phase. "
                       "Conflicts describe registered buffer ranges. An old-only observation "
                       "does not establish that a barrier can be safely skipped. Dispatch pairs "
                       "share a pipeline and submission and have no intervening draw or indirect "
                       "dispatch observed. Other commands may intervene. Address disjointness "
                       "is not an assertion of mergeability. Shader attributions are bounded.")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("session", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    result = analyze(args.session)
    args.output.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(result["phases"], indent=2))
    if result["dispatch_pairs"]:
        print(json.dumps(result["dispatch_pairs"], indent=2))
    if result["output_range_5b1e2375"]["records"]:
        print(json.dumps(result["output_range_5b1e2375"], indent=2))
    if result["draw_batches"]["batches"]:
        print(json.dumps(result["draw_batches"], indent=2))
    if result["indirect_args"]["batches"]:
        print(json.dumps(result["indirect_args"], indent=2))
