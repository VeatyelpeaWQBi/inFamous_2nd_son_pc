# SPDX-License-Identifier: GPL-2.0-or-later
"""Measure overlap between actual copy ranges at a feedback read."""
import argparse
from bisect import bisect_left, bisect_right
from collections import Counter, defaultdict
import csv
import json
from pathlib import Path

from analyze_super_batch import load
from analyze_super_worker_gather import PAGE_BYTES, PRIMARY
from analyze_timing import FIELDS


FLAGS = (
    "earlier_overlapping_window_on_other_timeline",
    "later_pending_copy_touches_page",
    "new_wide_copy_touches_page",
    "prior_prefetch_on_readback_queue",
    "new_copy_on_readback_queue",
    "keep_ranges",
    "already_clean_after_retirement",
    "arena_identity_differs",
)


def classify(narrow_tick, wide_tick, flags):
    if flags & ~0xff or wide_tick <= 0 or narrow_tick < 0:
        raise ValueError("Malformed readback wait scope")
    if not narrow_tick and flags & 0x8b:
        raise ValueError("No prior copy for prior-copy flags")
    counts = Counter(requests=1)
    counts.update(name for bit, name in enumerate(FLAGS) if flags & (1 << bit))
    if narrow_tick:
        counts["has_prior_prefetch"] += 1
    if 0 < narrow_tick < wide_tick and flags == 0:
        counts["earlier_graphics_tick_without_new_page_copy"] += 1
    if flags & 0x40 and not flags & 0x04:
        counts["clean_and_new_copy_avoids_page"] += 1
    return counts


def mask_bytes(lo, hi):
    return ((1 << (hi - lo)) - 1) << lo


def summarize_layout(address, wide, narrow):
    offset = address % PAGE_BYTES
    requested = mask_bytes(offset, offset + 4)
    counts = Counter(requests=1, wide_bytes=wide.bit_count(), narrow_bytes=narrow.bit_count(),
                     wide_touches_requested_word=bool(wide & requested),
                     narrow_contains_requested_word=(narrow & requested == requested))
    if offset % 256 != 4:
        counts["unknown_record_alignment"] += 1
        return counts
    record = offset - 4
    target = mask_bytes(record, record + 48)
    prefix = mask_bytes(0, 256) & ~target
    neighbors = 0
    for slot in range(256, PAGE_BYTES, 256):
        if slot != record:
            neighbors |= mask_bytes(slot, slot + 48)
    regions = dict(target_record=target, page_prefix=prefix, neighboring_slots=neighbors,
                   other=~(target | prefix | neighbors))
    for name, region in regions.items():
        count = (wide & region).bit_count()
        counts[name + "_bytes"] += count
        counts["touches_" + name] += count > 0
    counts["prefix_only"] += bool(wide and wide & ~prefix == 0)
    if counts["narrow_contains_requested_word"] and not counts["wide_touches_requested_word"]:
        counts["requested_word_only_in_narrow"] += 1
        if wide and not wide & target:
            counts["target_record_avoids_wide"] += 1
            counts["wide_would_remain_after_prefix_removed"] += bool(wide & ~prefix)
    return counts


def attribute_bytes(wide, writers, start, end, narrow_graphics_tick):
    """Latest observed declaration per byte, restricted to the interval between copies."""
    first = bisect_right(writers, start, key=lambda row: row[0])
    last = bisect_left(writers, end, key=lambda row: row[0])
    pending = wide
    result = Counter()
    timelines = Counter()
    for i in range(last - 1, first - 1, -1):
        _, mask, shader, packed_timeline = writers[i]
        overlap = pending & mask
        if overlap:
            size = overlap.bit_count()
            result[f"{shader:08x}"] += size
            if narrow_graphics_tick is None or packed_timeline & 1:
                timelines["other_queue"] += size
            elif packed_timeline >> 1 > narrow_graphics_tick:
                timelines["later_tick"] += size
            elif packed_timeline >> 1 == narrow_graphics_tick:
                timelines["same_tick"] += size
            else:
                timelines["earlier_tick"] += size
            pending &= ~overlap
    if pending:
        result["no_intercopy_declaration"] += pending.bit_count()
        timelines["no_intercopy_declaration"] += pending.bit_count()
    return result, timelines


def analyze(session):
    launch = load(session / "launch.json")
    meta = load(session / "path-trace.json")
    cycle, = load(session / "super-rounds.json")
    if (launch.get("readback_wait_scope_audit") is not True or not launch["complete"]
            or launch["exit_code"] != 0 or launch["progress_published"]
            or launch["source_game_written"] or not cycle["warm"] or not cycle["meter_consumed"]
            or not meta["complete"] or any(meta[k] for k in (
                "dropped", "io_failed", "cutoff_publishers", "gpu_profile_at_start",
                "compute_hash_at_start"))):
        raise ValueError("Expected an isolated, complete, warm readback wait audit")
    anchor = meta["start"]
    release = (anchor["ns_before"] + anchor["ns_after"]) // 2
    release += (cycle["release"]["qpc_after"] - anchor["qpc"]) * 10**9 // anchor["frequency"]
    windows = (("baseline", -10, 0), ("early", 0, 3), ("peak", 3, 8),
               ("late", 8, 13), ("recovery", 13, 25))
    phases = {name: defaultdict(Counter) for name, _, _ in windows}
    layout_phases = {name: defaultdict(Counter) for name, _, _ in windows}
    writer_phases = {name: defaultdict(Counter) for name, _, _ in windows}
    outside_prefix_writer_ticks = {name: defaultdict(Counter) for name, _, _ in windows}
    primary_pages = {address & -PAGE_BYTES for address in PRIMARY}
    last, requests, frames = {}, {}, 0
    layouts, copy_records, writers = {}, {}, defaultdict(list)
    spans = defaultdict(dict)

    def phase(stamp):
        return next((name for name, lo, hi in windows
                     if lo * 10**9 <= stamp - release < hi * 10**9), None)

    with (session / "path-trace.csv").open(encoding="utf-8-sig", newline="") as handle:
        reader = csv.DictReader(handle)
        if reader.fieldnames != FIELDS:
            raise ValueError("Unexpected trace schema")
        for count, row in enumerate(reader, 1):
            thread, seq, ns, ident = (int(row[k]) for k in ("thread", "seq", "ns", "id"))
            prev_seq, prev_ns = last.get(thread, (-1, 0))
            if (seq != prev_seq + 1 or ns < prev_ns or ident <= 0
                    or not 0 <= thread < meta["registered_threads"]):
                raise ValueError("Trace discontinuity")
            last[thread] = seq, ns
            kind = row["event"]
            if kind == "frame_sample":
                frames += 1
                continue
            a, b, c, d = (int(row[k]) for k in "abcd")
            if kind == "readback_wait_scope":
                if a <= 0 or a % PAGE_BYTES or ident in requests:
                    raise ValueError("Malformed readback wait scope record")
                name = phase(ns)
                requests[ident] = a, b, d, name
                counts = classify(b, c, d)
                scope = "primary_pages" if a in primary_pages else "other_feedback_pages"
                if name:
                    phases[name][scope].update(counts)
            elif kind == "readback_wait_layout":
                if ident in layouts or ident not in requests or (a & -PAGE_BYTES) != requests[ident][0]:
                    raise ValueError("Layout has no unique matching scope")
                layouts[ident] = a, b, c
            elif kind == "readback_wait_span":
                if ident not in layouts or a != layouts[ident][0] or d not in (0, 1):
                    raise ValueError("Span does not match the layout")
                if not 0 <= b < b + c <= PAGE_BYTES:
                    raise ValueError("Copy span outside its page")
                spans[ident][d] = spans[ident].get(d, 0) | mask_bytes(b, b + c)
            elif kind == "copy_record":
                if ident in copy_records or not 0 < a < b or d <= 0:
                    raise ValueError("Malformed copy record")
                copy_records[ident] = (ns, seq, thread), a, b
            elif kind == "readback_wait_writer":
                if a <= 0 or b <= 0 or not d >> 1:
                    raise ValueError("Malformed writer declaration")
                for page in primary_pages:
                    lo, hi = max(a, page), min(a + b, page + PAGE_BYTES)
                    if lo < hi:
                        writers[page].append(((ns, seq, thread), mask_bytes(lo - page, hi - page), c, d))
            else:
                raise ValueError("Unexpected trace event")
    if count != meta["events"] or not requests or not frames:
        raise ValueError("Missing trace records")
    for page in writers:
        writers[page].sort()
    for ident, (address, prior, current) in layouts.items():
        page, narrow_tick, flags, name = requests[ident]
        wide, narrow = (spans[ident].get(k, 0) for k in (0, 1))
        if bool(wide) != bool(flags & 4) or bool(narrow) != bool(narrow_tick):
            raise ValueError("Copy spans disagree with scope receipt")
        if current not in copy_records or not copy_records[current][1] <= page < copy_records[current][2]:
            raise ValueError("Missing later copy record")
        if not name:
            continue
        role = "primary_words" if address in PRIMARY else "other_words_on_watched_pages"
        layout_phases[name][role].update(summarize_layout(address, wide, narrow))
        if prior:
            if prior not in copy_records or copy_records[prior][1:] != (page, page + PAGE_BYTES):
                raise ValueError("Missing earlier single-page copy record")
            start, finish = copy_records[prior][0], copy_records[current][0]
            if start >= finish:
                raise ValueError("Copy record order reversed")
            # This audit expects declarations from the same command processor as the copy.
            if start[2] != finish[2] or any(row[0][2] != finish[2] for row in writers[page]):
                raise ValueError("Writer/copy records use multiple command processors")
            layout_phases[name][role]["requests_with_both_copy_records"] += 1
            narrow_graphics_tick = None if flags & 8 else narrow_tick
            attribution, _ = attribute_bytes(wide, writers[page], start, finish, narrow_graphics_tick)
            writer_phases[name][role].update(attribution)
            _, outside_ticks = attribute_bytes(wide & ~mask_bytes(0, 256), writers[page], start,
                                               finish, narrow_graphics_tick)
            outside_prefix_writer_ticks[name][role].update(outside_ticks)
            if outside_ticks["later_tick"]:
                layout_phases[name][role]["later_tick_outside_prefix_requests"] += 1
    return dict(session=str(session), events=count, frame_samples=frames, readbacks=len(requests),
                phases=phases, layout_records=len(layouts), layout_phases=layout_phases,
                latest_intercopy_writer_declaration_bytes=writer_phases,
                latest_intercopy_writer_tick_bytes_outside_page_prefix=outside_prefix_writer_ticks,
                limits="Only feedback MOVs for which a wider asynchronous copy was newly "
                       "recorded. Page intersections are from actual recorded copy regions. "
                       "Flags describe direct retirement guards, not a proven early return. "
                       "A separate completion check must still verify current page state. "
                       "Layout classifies the four-byte MOV, modeled 48-byte record, first "
                       "256 page bytes and other 48-byte slots separately. Writer bytes refer "
                       "to recorded declarations, not instructions that actually stored values.")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("session", type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    result = analyze(args.session)
    args.output.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(result, indent=2))
