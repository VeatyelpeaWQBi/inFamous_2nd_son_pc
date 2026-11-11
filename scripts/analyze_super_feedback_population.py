"""Offline population audit of ALL retained read handlers, including untimed copies.

Never launches/attaches to the game or GPU tools. Request stages are partitions of
one handler only; their sums are not a rendered frame's critical path.
"""
import argparse
from collections import Counter, defaultdict
import csv
import json
from pathlib import Path

from analyze_timing import Event, read_path, stats


def union_ns(intervals):
    total, last = 0, None
    for lo, hi in sorted(intervals):
        if hi < lo:
            raise ValueError("Negative interval")
        total += hi - lo if last is None else max(0, hi - max(lo, last))
        last = max(hi, hi if last is None else last)
    return total


def grouped_summary(rows, compact=False):
    columns = ("total_ns", "initial_queue_ns", "initial_cp_ns", "cpu_wait_ns",
               "completion_queue_ns", "completion_cp_ns", "handoff_and_unattributed_ns")
    selected_columns = ("total_ns",) if compact else columns
    result = {"requests": len(rows), "stages": {
        key: dict(stats([r.get(key, 0) for r in rows]),
                  sum_ms_not_wall=sum(r.get(key, 0) for r in rows) / 1e6) for key in selected_columns}}
    result["initial_writer_state"] = dict(Counter(r["initial_writer_state"] for r in rows))
    result["copy_timing_class"] = dict(Counter(r["copy_timing_class"] for r in rows))
    result["semaphore_domains"] = dict(Counter(r["semaphore_domains"] for r in rows))
    result["caller_thread_counts"] = dict(Counter(r["thread"] for r in rows))
    result["retry_requests"] = sum(bool(r["retry"]) for r in rows)
    result["nothing_to_download"] = sum(bool(r["nothing"]) for r in rows)
    if compact:
        return result
    # Each union is per caller thread. Do not add those unions across threads.
    by_thread = defaultdict(list)
    for row in rows:
        by_thread[row["thread"]].append((row["begin_ns"], row["end_ns"]))
    result["handler_union_ms_by_caller_thread"] = {
        str(thread): union_ns(intervals) / 1e6 for thread, intervals in by_thread.items()}
    result["largest_reads"] = [{key: r[key] for key in (
        "id", "site", "address", "size", "release_s", "copy_timing_class",
        "initial_writer_state", "thread", *columns) if key in r}
        for r in sorted(rows, key=lambda r: -r["total_ns"])[:8]]
    return result


def adjacency_model(clusters, cap=2 * 1024 * 1024):
    """Observed adjacency only; same record sequence is NOT an immutability proof."""
    by_sequence = defaultdict(list)
    unknown = 0
    for cluster in clusters:
        if cluster["record"] is None:
            unknown += 1
        else:
            by_sequence[cluster["record"]["sequence"]].append(cluster)
    batches = []
    for sequence, records in by_sequence.items():
        current = None
        for cluster in sorted(records, key=lambda c: c["record"]["address"]):
            record = cluster["record"]
            if (current is not None and record["address"] == current["end"]
                    and record["end"] - current["address"] <= cap):
                current["end"] = record["end"]
                current["copy_ids"].append(cluster["copy_id"])
            else:
                current = dict(sequence=sequence, address=record["address"], end=record["end"],
                               copy_ids=[cluster["copy_id"]])
                batches.append(current)
    return dict(cap_bytes=cap, observed_copy_ids=len(clusters), missing_records=unknown,
                same_sequence_groups=len(by_sequence), adjacent_batches=len(batches),
                copy_ids_in_multiwindow_batches=sum(len(b["copy_ids"]) for b in batches
                                                  if len(b["copy_ids"]) > 1),
                largest_batches=sorted(batches, key=lambda b: -len(b["copy_ids"]))[:12],
                interpretation="Hindsight grouping of adjacent observed windows only. "
                               "No proof of availability at first fault, immutable data, "
                               "valid guest bounds, removable latency, or FPS gain.")


def analyze(session):
    meta = json.loads((session / "path-trace.json").read_text())
    if not meta["complete"] or any(meta[k] for k in ("dropped", "io_failed", "cutoff_publishers")):
        raise ValueError("Trace is incomplete or lost events")
    groups, counts, samples = defaultdict(list), Counter(), {}
    timed_copies, copy_records = set(), {}
    with (session / "path-trace.csv").open(encoding="utf-8-sig", newline="") as stream:
        for row in csv.DictReader(stream):
            event = Event(int(row["ns"]), int(row["thread"]), row["event"],
                          *(int(row[k]) for k in ("a", "b", "c", "d")))
            ident = int(row["id"])
            counts[event.kind] += 1
            if event.kind.startswith("read_"):
                groups[ident].append(event)
            elif event.kind == "frame_sample":
                samples[event.a] = event.b
            elif event.kind == "gpu_copy_bounds":
                timed_copies.add(ident)
            elif event.kind == "copy_record":
                copy_records[ident] = dict(address=event.a, end=event.b,
                                           sequence=event.c, copied_bytes=event.d)
    if sum(counts.values()) != meta["events"]:
        raise ValueError("Trace metadata/event count mismatch")
    rounds = json.loads((session / "super-rounds.json").read_text(encoding="utf-8-sig"))
    releases = [r["release"]["applied_unix_ns"] for r in rounds if r.get("release")]
    if len(releases) != 1:
        raise ValueError("This audit requires exactly one recorded release")
    with (session / "frames.csv").open(encoding="utf-8-sig", newline="") as stream:
        frame = min((r for r in csv.DictReader(stream) if int(r["frame"]) in samples),
                    key=lambda r: abs(int(r["utc_ns"]) - releases[0]))
    release = samples[int(frame["frame"])] + releases[0] - int(frame["utc_ns"])
    rows, incomplete = [], []
    for ident, events in groups.items():
        events.sort(key=lambda e: e.ns)
        starts = [e for e in events if e.kind == "read_request"]
        returns = [e for e in events if e.kind == "read_return"]
        if not starts or not returns:
            incomplete.append(ident)
            continue
        metrics = read_path(events)  # Existing strict stage/lifetime/thread checks.
        request, returned = starts[0], returns[0]
        copies = [e for e in events if e.kind == "read_copy"]
        deps = [e for e in events if e.kind == "read_dependencies"
                and (not copies or e.ns <= copies[0].ns)]
        dependency = deps[-1] if deps else None
        state = ("done" if dependency.d & 1 else "submitted" if dependency.d & 2
                 else "current" if dependency.d & 4 else "queued") if dependency else (
                    "reuse-no-new-dependency-event" if copies else "no-copy")
        copy_ids = set(metrics["copies"])
        profiled = copy_ids & timed_copies
        timing_class = ("no-copy" if not copy_ids else "all-timed" if profiled == copy_ids
                        else "mixed" if profiled else "untimed")
        domains = {"readback" if e.b else "graphics" for e in events if e.kind == "read_wait_begin"}
        rows.append(dict(metrics, id=ident, site=hex(request.c), address=hex(request.a),
                         size=request.b, access_flags=request.d, thread=request.thread,
                         begin_ns=request.ns, end_ns=returned.ns,
                         release_s=(request.ns - release) / 1e9,
                         initial_writer_state=state, copy_timing_class=timing_class,
                         semaphore_domains="+".join(sorted(domains)) or "none",
                         copy_ids=sorted(copy_ids)))
    phases = {}
    for name, lo, hi in (("baseline", -10, 0), ("early", 0, 3), ("peak", 3, 8),
                         ("late", 8, 13), ("recover", 13, 23)):
        selected = [r for r in rows if lo <= r["release_s"] < hi]
        phase = grouped_summary(selected)
        phase["observed_flip_samples"] = sum(release + lo * 1e9 <= ns < release + hi * 1e9
                                              for ns in samples.values())
        phase["requests_crossing_phase_end"] = sum(r["end_ns"] > release + hi * 1e9 for r in selected)
        phase["groups"] = []
        address_groups, site_groups = defaultdict(list), defaultdict(list)
        for row in selected:
            address_groups[row["site"], row["address"], row["size"], row["access_flags"]].append(row)
            site_groups[row["site"]].append(row)
        for (site, address, size, flags), group in sorted(address_groups.items()):
            phase["groups"].append(dict(site=site, address=address, size=size,
                                        access_flags=flags, **grouped_summary(group, compact=True)))
        phase["groups"].sort(key=lambda r: -r["stages"]["total_ns"]["sum_ms_not_wall"])
        phase["by_site"] = []
        for site, group in sorted(site_groups.items()):
            refs = defaultdict(list)
            for row in group:
                for copy in row["copy_ids"]:
                    refs[copy].append(row)
            clusters = []
            for ident, requests in refs.items():
                record = copy_records.get(ident)
                clusters.append(dict(copy_id=ident, handler_references=len(requests),
                                     caller_threads=sorted({r["thread"] for r in requests}),
                                     first_request_s=min(r["release_s"] for r in requests),
                                     last_request_s=max(r["release_s"] for r in requests),
                                     record=record))
            phase["by_site"].append(dict(site=site, **grouped_summary(group),
                distinct_addresses=len({r["address"] for r in group}),
                distinct_4k_pages=len({int(r["address"], 16) // 4096 for r in group}),
                unique_copy_ids=len(refs), handler_copy_references=sum(map(len, refs.values())),
                shared_copy_ids=sum(len(v) > 1 for v in refs.values()),
                references_to_shared_copies=sum(len(v) for v in refs.values() if len(v) > 1),
                adjacent_window_model=adjacency_model(clusters),
                largest_copy_clusters=sorted(clusters, key=lambda r: -r["handler_references"])[:12]))
        phase["by_site"].sort(key=lambda r: -r["stages"]["total_ns"]["sum_ms_not_wall"])
        phases[name] = phase
    return dict(schema=1, session=str(session), offline_only=True, trace_metadata=meta,
                release_anchor=dict(steady_ns=release, frame=int(frame["frame"]),
                                    utc_distance_ns=abs(int(frame["utc_ns"]) - releases[0])),
                event_counts=dict(counts), all_completed_requests=len(rows),
                incomplete_ids=incomplete, copy_records=len(copy_records),
                all_requests_summary=grouped_summary(rows), phases=phases,
                limits=[
                    "Includes all complete handlers; GPU bounds are optional and classified explicitly.",
                    "Groups by request start; handlers crossing phase end are counted explicitly.",
                    "Stage sums partition one request, not frame time; caller-thread unions are not added.",
                    "Writer state is a conservative window snapshot, not the final shader or actual write.",
                    "Reuse without a new dependency event is unknown, never inherited from another request.",
                    "Timing classes describe captured query coverage, not the absence of GPU work.",
                    "This diagnostic run is not a performance A/B and includes a known allocation outlier.",
                ])


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("session", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    report = analyze(args.session)
    args.output.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print("Completed handlers:", report["all_completed_requests"], "incomplete:", len(report["incomplete_ids"]))
    for name, phase in report["phases"].items():
        print(name, "requests", phase["requests"], "coverage", phase["copy_timing_class"],
              "writers", phase["initial_writer_state"])
        for group in phase["groups"][:3]:
            print(" ", group["site"], group["address"], group["size"], group["requests"],
                  "mean ms", round(group["stages"]["total_ns"]["mean_ms"], 4),
                  "no copy", group["nothing_to_download"])
