# SPDX-License-Identifier: GPL-2.0-or-later
"""Measure real Windows protection calls and their command-processor context."""
import argparse
from bisect import bisect_right
from collections import Counter, defaultdict
import csv
import json
from pathlib import Path

from analyze_super_batch import load
from analyze_timing import FIELDS, stats

PHASES = (("baseline", -10, 0), ("early", 0, 3), ("peak", 3, 8), ("late", 8, 13))
PAGE_SIZE = 4096


def read_events(session, meta):
    calls, slices, samples = [], defaultdict(list), {}
    last = {}
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
            if row["event"] == "page_protect":
                if a % PAGE_SIZE or b <= 0 or b % PAGE_SIZE or not 0 <= d <= ns:
                    raise ValueError("Invalid protection range or duration")
                calls.append((ns, tid, a, b, c >> 32, c & 0xffffffff, d))
            elif row["event"] == "cp_queue_slice":
                if not 0 < a <= b <= ns or slices[tid] and a < slices[tid][-1][1]:
                    raise ValueError("Invalid CP slice")
                slices[tid].append((a, b, c))
            elif row["event"] == "frame_sample":
                if a in samples:
                    raise ValueError("Duplicate frame sample")
                samples[a] = b
            else:
                raise ValueError(f"Unexpected event: {row['event']}")
    if not calls or not samples or len(slices) != 1 or count != meta["events"]:
        raise ValueError("Missing protection/CP/frame data or wrong event count")
    return sorted(calls), slices, samples


def summarize(calls, slices, release):
    cp, = slices
    queue_slices = slices[cp]
    starts = [start for start, _, _ in queue_slices]
    times = defaultdict(list)
    changes = defaultdict(Counter)
    totals = defaultdict(Counter)
    addresses = defaultdict(Counter)
    previous = {}
    previous_call = {}
    for ns, tid, addr, size, old, new, elapsed in calls:
        phase = next((p for p, low, high in PHASES
                      if low * 10**9 <= ns - release < high * 10**9), None)
        if phase is None:
            continue
        index = bisect_right(starts, ns - elapsed) - 1
        within_slice = tid == cp and index >= 0 and ns <= queue_slices[index][1]
        context = ("compute" if queue_slices[index][2] else "graphics") if within_slice else (
            "cp_outside_slice" if tid == cp else "other_thread")
        key = phase, context
        times[key].append(elapsed)
        changes[key][(old, new)] += 1
        totals[key]["bytes_protected"] += size
        totals[key]["single_page_same_permission"] += size == PAGE_SIZE and old == new
        prev = previous_call.get(tid)
        if within_slice and prev and prev[:3] == (phase, index, new) and (
                addr + size == prev[3] or prev[3] + prev[4] == addr):
            totals[key]["adjacent_equal_permission_in_slice"] += 1
        previous_call[tid] = phase, index if within_slice else -1, new, addr, size
        # Count first-page recurrence only. VirtualProtectEx reports the old permission
        # of that page, not the original permissions throughout a multi-page call.
        addresses[key][addr] += 1
        prior = previous.get(addr)
        if prior and prior[0] == phase and old == prior[1] and new != old:
            totals[key]["first_page_permission_changed_again"] += 1
        previous[addr] = phase, new

    return {phase: {
        context: dict(call_ns=stats(times[phase, context]), **totals[phase, context],
                      unique_first_pages=len(addresses[phase, context]),
                      common_first_pages=[(hex(a), n) for a, n in
                                          addresses[phase, context].most_common(8)],
                      first_page_transitions=[(hex(old), hex(new), n) for (old, new), n in
                                              changes[phase, context].most_common()])
        for context in ("compute", "graphics", "cp_outside_slice", "other_thread")
    } for phase, _, _ in PHASES}


def analyze(session, allow_cap_stop=False):
    launch = load(session / "launch.json")
    meta = load(session / "path-trace.json")
    if (launch.get("page_protect_audit") is not True or not launch["complete"]
            or launch["exit_code"] != 0 or launch["progress_published"]
            or launch["source_game_written"] or not meta["complete"]
            or any(meta[k] for k in ("dropped", "io_failed", "cutoff_publishers",
                                     "gpu_profile_at_start", "compute_hash_at_start"))):
        raise ValueError("Expected an isolated, complete protection audit")
    cycle, = load(session / "super-rounds.json")
    partial = not cycle["warm"] or not cycle["meter_consumed"]
    if partial:
        batch = load(Path(__file__).resolve().parents[1] /
                     launch["super_capture_batch"] / "capture.json")
        if (not allow_cap_stop or batch["status"] != "failed" or
                batch["error"] != "Critical trace exceeded 64 MiB; raw data preserved." or
                not cycle["release"]["accepted"]):
            raise ValueError("Expected one warm release or an explicit trace cap stop")
    calls, slices, samples = read_events(session, meta)
    utc = cycle["release"]["applied_unix_ns"]
    with (session / "frames.csv").open(encoding="utf-8-sig", newline="") as stream:
        frame = min((row for row in csv.DictReader(stream) if int(row["frame"]) in samples),
                    key=lambda row: abs(int(row["utc_ns"]) - utc))
    anchor_delta = utc - int(frame["utc_ns"])
    if abs(anchor_delta) > 50_000_000:
        raise ValueError("Frame anchor is too far from release")
    release = samples[int(frame["frame"])] + anchor_delta
    first, last = min(samples.values()), max(samples.values())
    phases = summarize(calls, slices, release)
    coverage = {phase: max(0, min(last, release + high * 10**9) -
                           max(first, release + low * 10**9)) / 10**9
                for phase, low, high in PHASES}
    return dict(session=str(session), events=meta["events"], observed_calls=len(calls),
                release_anchor_delta_ns=anchor_delta, partial_capture=partial,
                phase_coverage_seconds=coverage, phases=phases,
                limits="Completion timestamps include trace entry overhead. CP work under 50 us "
                       "is not covered by slice tags. First-page sequences do not establish "
                       "unchanged mappings, full-range permissions or safely batchable calls.")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("session", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--allow-cap-stop", action="store_true")
    args = parser.parse_args()
    result = analyze(args.session, args.allow_cap_stop)
    args.output.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    for phase, contexts in result["phases"].items():
        print(phase, {name: context["call_ns"] for name, context in contexts.items()})
