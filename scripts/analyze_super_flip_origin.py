# SPDX-License-Identifier: GPL-2.0-or-later
"""Measure flip cadence before and after the command processor receives an EOP flip."""

import argparse
from bisect import bisect_right
import csv
import json
from collections import defaultdict
from pathlib import Path

from analyze_timing import stats


def analyze(session):
    meta = json.loads((session / "path-trace.json").read_text(encoding="utf-8-sig"))
    if not meta["complete"] or meta["dropped"] or meta["io_failed"] or meta["cutoff_publishers"]:
        raise ValueError("Trace is incomplete")
    rounds = json.loads((session / "super-rounds.json").read_text(encoding="utf-8-sig"))
    release, = (r["release"] for r in rounds if r.get("release"))
    clock = meta["start"]
    ns = (clock["ns_before"] + clock["ns_after"]) // 2
    release_ns = ns + (release["qpc_after"] - clock["qpc"]) * 10**9 // clock["frequency"]
    flip_events = ("flip_eop_register", "flip_request", "flip_ready", "flip_consume",
                   "flip_present_return", "flip_done")
    wait_events = ("gnm_wait_begin", "gnm_wait_end", "read_request", "read_return")
    groups = {}
    read_addresses = {}
    registration_cpu = {}
    registration_submit = {}
    word_loads = defaultdict(list)
    census = []
    with (session / "path-trace.csv").open(encoding="utf-8-sig", newline="") as f:
        count = 0
        for row in csv.DictReader(f):
            count += 1
            kind = row["event"]
            if kind == "gfx_command_census":
                stamp, dcb, ccb, eligible, longest = (
                    int(row[k]) for k in ("ns", "a", "b", "c", "d"))
                if not 0 <= longest <= eligible <= dcb:
                    raise ValueError("Invalid graphics command census")
                census.append((stamp, dcb, ccb, eligible, longest))
            if kind == "word_load_result" and int(row["a"]) in (0x273530404, 0x275330404):
                stamp, duration, thread = (int(row[k]) for k in ("ns", "c", "thread"))
                if not 0 <= duration <= stamp:
                    raise ValueError("Invalid precise word load duration")
                word_loads[thread].append((stamp - duration, stamp))
            if kind in flip_events or kind in wait_events:
                events = groups.setdefault(int(row["id"]), {})
                if kind in events:
                    raise ValueError(f"Duplicate {kind} for ID {row['id']}")
                events[kind] = int(row["ns"]), int(row["thread"])
                if kind == "read_request":
                    read_addresses[int(row["id"])] = int(row["a"])
                elif kind == "flip_eop_register":
                    if int(row["c"]) > 0:
                        registration_cpu[int(row["id"])] = int(row["c"]) - 1
                    if int(row.get("d", 0)) > 0:
                        registration_submit[int(row["id"])] = int(row["d"]) - 1
    if count != meta["events"]:
        raise ValueError("Trace event count does not match metadata")
    flips = []
    waits = defaultdict(list)
    reads = defaultdict(list)
    main_reads = defaultdict(list)
    for ident, events in groups.items():
        if all(k in events for k in flip_events):
            times = [events[k][0] for k in flip_events]
            if times != sorted(times):
                raise ValueError("EOP events are out of order")
            flips.append((times, events[flip_events[0]][1], registration_cpu.get(ident),
                          registration_submit.get(ident)))
        for pair in (wait_events[:2], wait_events[2:]):
            if all(k in events for k in pair):
                (start, thread), (end, end_thread) = (events[k] for k in pair)
                if end < start or thread != end_thread:
                    raise ValueError(f"Invalid span for {pair[0]}")
                if pair[0] == "gnm_wait_begin":
                    waits[thread].append((start, end))
                else:
                    reads[thread].append((start, end))
                    if read_addresses[ident] in (0x273530404, 0x275330404):
                        main_reads[thread].append((start, end))
    flips.sort(key=lambda flip: flip[0][-1])
    if not flips or not waits:
        raise ValueError("Missing joined EOP flip or GNM wait events")
    windows = {}
    census_windows = {}
    main_feedback = {tid: sorted(main_reads[tid] + word_loads[tid])
                     for tid in main_reads.keys() | word_loads.keys()}
    known_stalls = {tid: sorted(main_feedback.get(tid, []) + waits.get(tid, []))
                    for tid in main_feedback.keys() | waits.keys()}
    joined_wait_count = sum(map(len, waits.values()))
    for timeline in (reads, main_reads, waits, main_feedback, known_stalls):
        for tid, spans in timeline.items():
            merged = []
            for start, end in sorted(spans):
                if merged and start <= merged[-1][1]:
                    merged[-1] = (merged[-1][0], max(end, merged[-1][1]))
                else:
                    merged.append((start, end))
            timeline[tid] = merged

    def overlap(spans, left, right):
        total = 0
        last = left
        first = bisect_right(spans, left, key=lambda span: span[1])
        for index in range(first, len(spans)):
            start, end = spans[index]
            if start >= right:
                break
            low, high = max(last, start), min(right, end)
            if high > low:
                total += high - low
                last = high
        return total

    for lo, hi in [(-8, -1), (0, 3), (3, 8), (8, 12), (12, 18)]:
        for with_ccb in (False, True):
            spans = [row for row in census if lo <= (row[0] - release_ns) / 1e9 < hi
                     and bool(row[2]) == with_ccb]
            if spans:
                total = sum(row[1] for row in spans)
                lengths = sorted(row[4] for row in spans)
                census_windows[f"{lo}:{hi} ccb={int(with_ccb)}"] = {
                    "sampled_graphics_buffers": len(spans),
                    "sampled_dcb_bytes": 4 * total,
                    "simple_dword_share": sum(row[3] for row in spans) / total if total else None,
                    "median_longest_simple_run_bytes": 4 * lengths[(len(lengths) - 1) // 2],
                    "max_longest_simple_run_bytes": 4 * lengths[-1],
                }
        for slow_only in (False, True):
            series = {name: [] for name in (
                "register_gap", "request_gap", "flip_done_gap", "register_to_request",
                "ready_to_consume", "present_call", "same_thread_gnm_wait_during_register_gap",
                "same_thread_read_during_register_gap",
                "main_word_read_during_register_gap", "register_gap_outside_main_word_reads",
                "main_feedback_during_register_gap", "register_gap_outside_main_feedback",
                "gnm_wait_outside_main_feedback", "register_gap_outside_known_stalls")}
            cpu_ns = 0
            cpu_wall_ns = 0
            cpu_samples = 0
            submit_deltas = []
            for (prev, prior_thread, prev_cpu, prev_submit), (cur, thread, cur_cpu, cur_submit) in zip(flips, flips[1:]):
                if not lo <= (cur[-1] - release_ns) / 1e9 < hi:
                    continue
                if prior_thread != thread or prev[0] > cur[0]:
                    continue
                if slow_only and cur[-1] - prev[-1] < 42_000_000:
                    continue
                if prev_cpu is not None and cur_cpu is not None and cur_cpu >= prev_cpu:
                    cpu_ns += cur_cpu - prev_cpu
                    cpu_wall_ns += cur[0] - prev[0]
                    cpu_samples += 1
                if prev_submit is not None and cur_submit is not None and cur_submit >= prev_submit:
                    submit_deltas.append(cur_submit - prev_submit)
                same_thread_wait = overlap(waits[thread], prev[0], cur[0])
                same_thread_read = overlap(reads[thread], prev[0], cur[0])
                main_read = overlap(main_reads[thread], prev[0], cur[0])
                feedback = overlap(main_feedback.get(thread, ()), prev[0], cur[0])
                stalls = overlap(known_stalls.get(thread, ()), prev[0], cur[0])
                for key, value in zip(series, (
                    cur[0] - prev[0], cur[1] - prev[1], cur[-1] - prev[-1], cur[1] - cur[0],
                    cur[3] - cur[2], cur[4] - cur[3], same_thread_wait, same_thread_read,
                    main_read, cur[0] - prev[0] - main_read,
                    feedback, cur[0] - prev[0] - feedback,
                    stalls - feedback, cur[0] - prev[0] - stalls,
                )):
                    series[key].append(value)
            label = f"{lo}:{hi}" + (" slow" if slow_only else "")
            windows[label] = {key: stats(values) for key, values in series.items()}
            if submit_deltas:
                windows[label]["guest_gnm_submit_wall"] = stats(submit_deltas)
            if cpu_samples:
                windows[label]["registration_thread_cpu_budget"] = {
                    "samples": cpu_samples,
                    "mean_cpu_ms": cpu_ns / cpu_samples / 1e6,
                    "mean_wall_ms": cpu_wall_ns / cpu_samples / 1e6,
                    "mean_wall_minus_cpu_ms": (cpu_wall_ns - cpu_ns) / cpu_samples / 1e6,
                }
    baseline = windows["-8:-1"]["main_word_read_during_register_gap"].get("mean_ms")
    if baseline is not None:
        for phases in windows.values():
            outside = phases["register_gap_outside_main_word_reads"].get("mean_ms")
            if outside is not None:
                phases["modeled_register_ms_at_baseline_main_read_cost"] = outside + baseline
    return {"session": session.name, "release_ns": release_ns, "joined_flips": len(flips),
            "joined_gnm_waits": joined_wait_count, "windows": windows,
            "gfx_command_census": census_windows,
            "limit": "The arithmetic counterfactual holds all other intervals fixed. "
                     "It is not a display FPS prediction or a GPU dependency proof."}


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("session", type=Path)
    args = parser.parse_args()
    print(json.dumps(analyze(args.session), indent=2))
