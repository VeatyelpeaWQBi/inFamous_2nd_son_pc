# SPDX-License-Identifier: GPL-2.0-or-later
"""Bound the observed cost of the particle index gather using retained native traces."""
import argparse
from bisect import bisect_left, bisect_right
from collections import Counter, defaultdict
import csv
import json
from pathlib import Path
import re

from analyze_super_batch import load
from analyze_super_feedback_population import union_ns
from analyze_timing import Event, FIELDS, one, read_path, stats
from project_paths import ROOT


WORKER_RVA = 0x3FF6D0
FEEDBACK_RVA = 0x3F3EFF
PRIMARY = (0x273530404, 0x275330404)
PAGE_BYTES = 4096


def merge(spans):
    merged = []
    for lo, hi in sorted(spans):
        if merged and lo <= merged[-1][1]:
            merged[-1] = merged[-1][0], max(merged[-1][1], hi)
        else:
            merged.append((lo, hi))
    return merged


def clip(spans, left, right):
    first = bisect_right(spans, left, key=lambda x: x[1])
    for i in range(first, len(spans)):
        lo, hi = spans[i]
        if lo >= right:
            break
        yield max(lo, left), min(hi, right)


def primary_page_timing(primary, copies, published, graphics_end, addresses=PRIMARY):
    """Classify preceding records by window, independently of their packed payload size."""
    result = []
    for address in addresses:
        page = address & -PAGE_BYTES
        records = sorted((stamp, ident) for ident, (stamp, lo, hi, _) in copies.items()
                         if lo == page and hi == page + PAGE_BYTES)
        stamps = [stamp for stamp, _ in records]
        previous_end = 0
        requests = sorted((row for row in primary if row[2] == address), key=lambda row: row[0])
        for start, end, _, timing in requests:
            i = bisect_left(stamps, start) - 1
            prior = records[i] if i >= 0 and stamps[i] >= previous_end else None
            previous_end = end
            counts = Counter(primary_requests=1)
            lead, graphics_gap = None, None
            if prior is not None:
                stamp, ident = prior
                lead = start - stamp
                counts["prior_page_window"] += 1
                if ident in published and published[ident][0] <= start:
                    counts["prior_page_published_before_request"] += 1
                else:
                    counts["prior_page_pending_at_request"] += 1
                    cp_end = start + timing["initial_queue_ns"] + timing["initial_cp_ns"]
                    if ident in published and published[ident][0] <= cp_end:
                        counts["prior_page_published_by_end_of_initial_cp"] += 1
                counts["prior_page_referenced_by_request"] += ident in timing["copies"]
            wider = []
            for ident in timing["copies"]:
                if ident in copies:
                    stamp, lo, hi, _ = copies[ident]
                    if stamp >= start and lo <= page < hi and hi - lo > PAGE_BYTES:
                        wider.append((stamp, ident))
            counts["recorded_new_wider_copy"] += bool(wider)
            if prior is not None and wider:
                narrow_id, wide_id = prior[1], min(wider)[1]
                if narrow_id in published and wide_id in published:
                    _, narrow_tick, narrow_domain = published[narrow_id]
                    _, wide_tick, wide_domain = published[wide_id]
                    if narrow_domain == wide_domain == 0 and narrow_tick < wide_tick:
                        counts["prior_page_on_earlier_graphics_tick"] += 1
                        if narrow_tick in graphics_end and wide_tick in graphics_end:
                            gap = graphics_end[wide_tick] - graphics_end[narrow_tick]
                            if gap < 0:
                                raise ValueError("Graphics completion timestamps out of order")
                            graphics_gap = gap
            result.append((start, counts, lead, graphics_gap))
    return result


def warm_prefix_evidence(cycle, samples, end_seconds, frequency):
    """Qualify only a prefix proven unchanged by its native status/ack bracket.

    A later compilation still excludes the whole capture from warm performance acceptance.
    This opt-in never changes that raw status, skips a compilation, or qualifies a suffix.
    """
    keys = ("shader_compile_starts", "pipeline_compile_starts")
    if (type(end_seconds) not in (int, float) or not 0 < end_seconds <= 30 or
            cycle.get("status") != "captured-new-compilation" or cycle.get("warm") or
            not cycle.get("meter_consumed") or frequency <= 0):
        raise ValueError("Expected an explicit diagnostic-only cold capture prefix")
    before = tuple(cycle["before"][key] for key in keys)
    if (before != tuple(cycle["filled"][key] for key in keys) or
            any(cycle[phase]["compilations_active"] for phase in ("filled", "before")) or
            not any(cycle["after"][key] > before[i] for i, key in enumerate(keys))):
        raise ValueError("Compilation counters do not establish a warm prefix")
    acks = {e["sequence"]: e for e in samples
            if e.get("action") == "status" and e.get("accepted") is True}
    previous = None
    last_proven, last_sequence = None, None
    release_qpc = cycle["release"]["qpc_after"]
    for sample in samples:
        if "shader_compile_starts" not in sample:
            continue
        counts = tuple(sample[key] for key in keys)
        if (any(type(v) is not int or v < 0 for v in counts) or
                (previous is not None and any(v < old for v, old in zip(counts, previous)))):
            raise ValueError("Nonmonotonic compilation counters")
        previous = counts
        ack = acks.get(sample["sequence"])
        if not ack or ack["qpc_after"] < ack["qpc_before"]:
            raise ValueError("Missing native status acknowledgment bracket")
        if counts == before and sample["compilations_active"] == 0:
            # qpc_before precedes the counter read; use it, not the later file observation.
            proven = (ack["qpc_before"] - release_qpc) / frequency
            if last_proven is None or proven > last_proven:
                last_proven, last_sequence = proven, sample["sequence"]
    if last_proven is None or end_seconds > last_proven - .05:
        raise ValueError("Requested prefix extends beyond the native no-compilation evidence")
    return dict(diagnostic_only=True, whole_capture_warm=False, end_seconds=end_seconds,
                last_native_no_compilation_seconds=last_proven,
                last_native_status_sequence=last_sequence, guard_seconds=.05)


def analyze(session, primary_addresses=PRIMARY, warm_prefix_end=None):
    if (len(primary_addresses) != 2 or len(set(primary_addresses)) != 2 or
            any(type(address) is not int or not 0 < address < 2**40 or address % 4
                for address in primary_addresses)):
        raise ValueError("Expected two distinct aligned guest feedback addresses")
    launch = load(session / "launch.json")
    preflight = load(session / "preflight.json")
    meta = load(session / "path-trace.json")
    project = load(ROOT / "project.json")
    cycle, = load(session / "super-rounds.json")
    prefix = (warm_prefix_evidence(cycle, load(session / cycle["events_file"]), warm_prefix_end,
                                  meta["start"]["frequency"])
              if warm_prefix_end is not None else None)
    if (not launch["complete"] or launch["exit_code"] != 0 or launch["source_game_written"]
            or launch["game_sha256"] != project["game_sha256"]
            or preflight["game_sha256"] != launch["game_sha256"]
            or preflight["runtime_sha256"] != launch["runtime_sha256"]
            or preflight["identity"]["TITLE_ID"] != project["title"]
            or preflight["identity"]["APP_VER"] != project["version"]
            or (prefix is None and not cycle["warm"]) or not cycle["meter_consumed"]
            or (prefix is None and any(cycle["filled"][k] != cycle["before"][k] or
                   cycle["before"][k] != cycle["after"][k]
                   for k in ("shader_compile_starts", "pipeline_compile_starts")))
            or not meta["complete"] or any(meta[k] for k in
                ("dropped", "io_failed", "cutoff_publishers"))):
        raise ValueError(f"Expected a complete, pinned and warm native trace: {session}")
    log = (session / "log/CUSA00309.log").read_text(encoding="utf-8-sig", errors="replace")
    bases = set(re.findall(r"Loading module eboot\.bin to (0x[0-9a-fA-F]+)\b", log))
    if len(bases) != 1:
        raise ValueError("Missing unique eboot mapping")
    base = int(bases.pop(), 16)
    clock = meta["start"]
    origin = (clock["ns_before"] + clock["ns_after"]) // 2
    release = cycle["release"]
    release_ns = origin + (release["qpc_after"] - clock["qpc"]) * 10**9 // clock["frequency"]
    groups, flips, copy_records, published = defaultdict(list), defaultdict(list), {}, {}
    graphics_end = {}
    last = {}
    first_event, last_event = None, None
    with (session / "path-trace.csv").open(encoding="utf-8-sig", newline="") as stream:
        reader = csv.DictReader(stream)
        if reader.fieldnames != FIELDS:
            raise ValueError("Unexpected trace schema")
        for count, row in enumerate(reader, 1):
            kind = row["event"]
            thread, seq, ns, ident = (int(row[k]) for k in ("thread", "seq", "ns", "id"))
            prev_seq, prev_ns = last.get(thread, (-1, 0))
            if (seq != prev_seq + 1 or ns < prev_ns or ident <= 0
                    or not 0 <= thread < meta["registered_threads"]):
                raise ValueError("Trace discontinuity")
            last[thread] = seq, ns
            first_event = ns if first_event is None else min(first_event, ns)
            last_event = ns if last_event is None else max(last_event, ns)
            if kind.startswith("read_"):
                groups[ident].append(Event(ns, thread, kind, *(int(row[k]) for k in "abcd")))
            elif kind == "copy_record":
                address, end, size = (int(row[k]) for k in "abd")
                if ident in copy_records or end <= address or size <= 0:
                    raise ValueError("Malformed copy record")
                copy_records[ident] = ns, address, end, size
            elif kind == "copy_publish_end":
                if ident in published:
                    raise ValueError("Duplicate copy publication")
                published[ident] = ns, int(row["a"]), int(row["b"])
            elif kind == "gpu_session_bounds" and int(row["d"]) == 0:
                tick, begin, end = (int(row[k]) for k in "abc")
                if tick in graphics_end or end < begin:
                    raise ValueError("Malformed graphics session timing")
                graphics_end[tick] = end
            elif kind == "flip_eop_register":
                flips[thread].append(ns)
    if count != meta["events"] or len(flips) != 1:
        raise ValueError("Unexpected event count or multiple EOP registration threads")
    (registration, stamps), = flips.items()
    if stamps != sorted(set(stamps)):
        raise ValueError("Nonmonotonic EOP registrations")
    worker, primary, feedback, incomplete = [], [], [], []
    copy_sites = defaultdict(set)
    for events in groups.values():
        starts = [e for e in events if e.kind == "read_request"]
        if not starts:  # May have begun before trace activation.
            continue
        request = one(events, "read_request")
        rva = request.c - base
        for e in events:
            if e.kind == "read_copy" and e.d:
                copy_sites[e.d].add(rva)
        if request.d & 1:
            continue
        is_feedback = rva == FEEDBACK_RVA and request.thread == registration
        is_primary = is_feedback and request.a in primary_addresses
        if (rva == WORKER_RVA or is_feedback) and not any(e.kind == "read_return" for e in events):
            incomplete.append(dict(ns=request.ns, rva=hex(rva), thread=request.thread))
            continue
        if is_feedback:
            returned = one(events, "read_return")
            if returned.thread != request.thread or returned.ns < request.ns:
                raise ValueError("Invalid feedback read lifetime")
            feedback.append((request.ns, returned.ns, request.a))
        if rva == WORKER_RVA:
            record = read_path(events)
            worker.append((request.ns, one(events, "read_return").ns, request.thread, record))
        elif is_primary:
            primary.append((request.ns, one(events, "read_return").ns, request.a,
                            read_path(events)))
    if not worker or not primary:
        raise ValueError("No observed worker gather or primary feedback")
    worker_spans = merge((lo, hi) for lo, hi, _, _ in worker)
    primary_spans = merge((lo, hi) for lo, hi, _, _ in primary)
    page_records = primary_page_timing(primary, copy_records, published, graphics_end, primary_addresses)
    feedback_spans = merge((lo, hi) for lo, hi, _ in feedback)
    by_address = defaultdict(list)
    for lo, hi, address in feedback:
        by_address[address].append((lo, hi))
    address_spans = {address: merge(spans) for address, spans in by_address.items()}
    phases = {}
    for label, lo, hi in (("baseline", -8, -1), ("early", 0, 3), ("peak", 3, 8), ("late", 8, 12)):
        if prefix is not None and hi > prefix["end_seconds"]:
            continue
        left, right = release_ns + lo * 10**9, release_ns + hi * 10**9
        per_flip, worker_only, primary_per_flip, feedback_per_flip, intervals = [], [], [], [], []
        frame_bounds = []
        for start, end in zip(stamps, stamps[1:]):
            if left <= start < end <= right:
                w, p = list(clip(worker_spans, start, end)), list(clip(primary_spans, start, end))
                w_ns, p_ns = union_ns(w), union_ns(p)
                per_flip.append(w_ns)
                primary_per_flip.append(p_ns)
                feedback_per_flip.append(union_ns(clip(feedback_spans, start, end)))
                worker_only.append(union_ns(w + p) - p_ns)
                intervals.append(end - start)
                frame_bounds.append((start, end))
        address_waits = []
        for address, spans in address_spans.items():
            duration = sum(union_ns(clip(spans, start, end)) for start, end in frame_bounds)
            if duration:
                address_waits.append(dict(address=hex(address), wait_ns=duration,
                                          mean_ms_per_interval=duration / len(intervals) / 1e6,
                                          primary=address in primary_addresses))
        address_waits.sort(key=lambda item: item["wait_ns"], reverse=True)
        if sum(item["wait_ns"] for item in address_waits) != sum(feedback_per_flip):
            raise ValueError("Feedback reads overlap on the EOP caller thread")
        requests = [x for x in worker if left <= x[0] and x[1] <= right]
        copies = {i for _, _, _, record in requests for i in record["copies"]}
        slow = [i for i, duration in enumerate(intervals) if duration > 40_000_000]
        page_counts, page_leads, graphics_gaps = Counter(), [], []
        for start, counts, lead, graphics_gap in page_records:
            if left <= start < right:
                page_counts.update(counts)
                if lead is not None:
                    page_leads.append(lead)
                if graphics_gap is not None:
                    graphics_gaps.append(graphics_gap)
        phases[label] = dict(
            complete_trace_window=(first_event <= left < right <= last_event
                                   and not any(x["ns"] < right for x in incomplete)),
            intervals=stats(intervals), worker_wall_union=stats(per_flip),
            worker_wall_union_outside_primary_read=stats(worker_only),
            primary_read_on_eop_thread=stats(primary_per_flip),
            all_feedback_read_on_eop_thread=stats(feedback_per_flip),
            feedback_address_waits=address_waits,
            primary_page_timing=dict(page_counts, record_lead=stats(page_leads),
                                     graphics_end_timestamp_gap=stats(graphics_gaps)),
            worker_wall_union_in_over_40ms_intervals=stats([per_flip[i] for i in slow]),
            over_40ms_intervals=len(slow),
            over_40ms_after_direct_worker_wait_subtraction=sum(
                duration - wait > 40_000_000 for duration, wait in zip(intervals, worker_only)),
            intervals_with_worker_wait=sum(n > 0 for n in per_flip),
            intervals_with_worker_outside_primary=sum(n > 0 for n in worker_only),
            worker_requests=len(requests),
            worker_threads=dict(Counter(t for _, _, t, _ in requests)),
            worker_copies=len(copies),
            worker_copy_ids_referenced_from_other_sites=sum(copy_sites[i] != {WORKER_RVA}
                                                           for i in copies),
            worker_copy_records_missing=len(copies - copy_records.keys()),
            worker_staging_bytes=sum(copy_records[i][3] for i in copies if i in copy_records),
            worker_staging_bytes_referenced_from_other_sites=sum(
                copy_records[i][3] for i in copies
                if i in copy_records and copy_sites[i] != {WORKER_RVA}),
            worker_cp_body_ms_per_interval=(
                sum(row["initial_cp_ns"] + row.get("completion_cp_ns", 0)
                    for _, _, _, row in requests) / len(intervals) / 1e6 if intervals else None))
    return dict(session=str(session), runtime_sha256=launch["runtime_sha256"], eboot_base=hex(base),
                diagnostic_warm_prefix=prefix,
                primary_addresses=[hex(address) for address in primary_addresses],
                events=count, eop_thread=registration, release_ns=release_ns, phases=phases,
                incomplete_relevant_requests=incomplete,
                limits="Handler unions are wall-clock coverage within adjacent EOP registrations. "
                       "Overlap does not establish job joins or a causal frame. The union cannot "
                       "price untraced CPU computation, CPU-to-GPU uploads or GPU work avoided. "
                       "Direct wait subtraction is a fixed-timeline sensitivity check. "
                       "Copy references can omit later accesses to a page already downloaded. "
                       "Staging sizes include copy alignment and are not DMA byte counts. "
                       "Prior page-window record timing does not prove a requested word was "
                       "copied or current. It excludes records before the last same-address return. "
                       "GPU gaps use two measured graphics submission end timestamps only.")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("sessions", nargs="+", type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--primary-address", action="append", type=lambda value: int(value, 0))
    parser.add_argument("--warm-prefix-end", type=float,
                        help="Diagnostic only: exclude all phases beyond native no-compile evidence")
    args = parser.parse_args()
    results = [analyze(session, args.primary_address or PRIMARY, args.warm_prefix_end)
               for session in args.sessions]
    args.output.write_text(json.dumps(results, indent=2) + "\n", encoding="utf-8")
    for result in results:
        print(result["session"])
        for name, phase in result["phases"].items():
            print(name, phase["worker_requests"], phase["worker_copies"],
                  phase["worker_wall_union"]["mean_ms"],
                  phase["worker_wall_union_outside_primary_read"]["mean_ms"])
