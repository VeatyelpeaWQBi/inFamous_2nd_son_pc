# SPDX-License-Identifier: GPL-2.0-or-later
"""Find adjacent Windows protection calls within sampled compute buffer bindings."""
import argparse
from collections import Counter, defaultdict
import csv
import json
from pathlib import Path

from analyze_super_batch import load
from analyze_timing import FIELDS


PHASES = (("baseline", -10, 0), ("early", 0, 3), ("peak", 3, 8),
          ("late", 8, 13), ("recovery", 13, 25))


def summarize(handle, meta, release):
    reader = csv.DictReader(handle)
    if reader.fieldnames != FIELDS:
        raise ValueError("Unexpected trace schema")
    threads = {}
    scope_threads = set()
    stats = defaultdict(Counter)
    shaders = defaultdict(lambda: defaultdict(Counter))
    bindings = defaultdict(lambda: defaultdict(Counter))
    pending = None
    last_binding_id = 0
    frames = []
    count = 0
    for count, row in enumerate(reader, 1):
        tid, seq, ns, ident = (int(row[k]) for k in ("thread", "seq", "ns", "id"))
        old_seq, old_ns = threads.get(tid, (-1, 0))
        if not (0 <= tid < meta["registered_threads"] and seq == old_seq + 1
                and ns >= old_ns and ident > 0):
            raise ValueError("Discontinuous trace")
        threads[tid] = seq, ns
        phase = next((name for name, begin, end in PHASES
                      if begin * 10**9 <= ns - release < end * 10**9), None)
        if pending is not None:
            if row["event"] != "compute_protect_binding_cost" or (tid, ident) != pending[:2]:
                # Other threads can interleave. Binding pairs must be adjacent on their thread.
                if tid == pending[0]:
                    raise ValueError("Missing binding cost")
            else:
                _, _, shader, index, calls, address, size = pending
                elapsed, amount, first, end = (int(row[k]) for k in "abcd")
                if (min(elapsed, amount, first, end) < 0 or first >= end or amount == 0
                        or first % 4096 or end % 4096 or amount % 4096
                        or amount > calls * (end - first)):
                    raise ValueError("Invalid protected footprint")
                if phase is not None:
                    descriptor_first = address // 4096 * 4096
                    descriptor_end = (address + size + 4095) // 4096 * 4096
                    bindings[phase][shader, index, size].update(
                        samples=1, calls=calls, call_ns=elapsed, protected_bytes=amount,
                        descriptor_pages=(descriptor_end - descriptor_first) // 4096,
                        protected_envelope_pages=(end - first) // 4096,
                        outside_descriptor_samples=int(first < descriptor_first or end > descriptor_end),
                        single_descriptor_page_samples=int(descriptor_end - descriptor_first == 4096))
                pending = None
                continue
        if row["event"] == "compute_protect_binding":
            if pending is not None or ident <= last_binding_id:
                raise ValueError("Overlapping binding records")
            shader, packed, address, size = (int(row[k]) for k in "abcd")
            index, calls = packed >> 32, packed & 0xffffffff
            if min(shader, packed, address, size) <= 0 or calls == 0 or index >= 64:
                raise ValueError("Invalid binding record")
            pending = tid, ident, shader, index, calls, address, size
            last_binding_id = ident
            continue
        if row["event"] == "frame_sample":
            frames.append(ns)
            continue
        if row["event"] != "compute_protect_scope":
            raise ValueError("Unexpected event")
        scope_threads.add(tid)
        shader, calls, packed, elapsed = (int(row[k]) for k in "abcd")
        same, other = packed >> 32, packed & 0xffffffff
        if (min(shader, calls, packed, elapsed) < 0 or same + other > max(0, calls - 1)
                or (calls == 0 and elapsed != 0)):
            raise ValueError("Invalid protection counters")
        for phase, begin, end in PHASES:
            if begin * 10**9 <= ns - release < end * 10**9:
                counters = dict(sampled_dispatches=1, dispatches_with_calls=int(calls > 0),
                                calls=calls, within_binding_pairs=same,
                                across_binding_pairs=other, call_ns=elapsed)
                stats[phase].update(counters)
                shaders[phase][shader].update(counters)
                break
    if (count != meta["events"] or len(scope_threads) != 1 or not frames or not stats
            or pending is not None):
        raise ValueError("Incomplete frame/compute trace")
    result = {}
    for phase, begin, end in PHASES:
        low, high = release + begin * 10**9, release + end * 10**9
        coverage = max(0, min(max(frames), high) - max(min(frames), low)) / 10**9
        result[phase] = dict(stats[phase], coverage_seconds=coverage,
                             bindings=[dict(metrics, shader=f"{shader:08x}", binding=index, size=size)
                                       for (shader, index, size), metrics in
                                       sorted(bindings[phase].items(),
                                              key=lambda pair: pair[1]["calls"], reverse=True)],
                             shaders=[dict(metrics, shader=f"{shader:08x}") for shader, metrics in
                                      sorted(shaders[phase].items(),
                                             key=lambda pair: pair[1]["calls"], reverse=True)])
    return result


def analyze(session):
    launch = load(session / "launch.json")
    meta = load(session / "path-trace.json")
    trial, = load(session / "super-rounds.json")
    if (launch.get("compute_protect_audit") is not True or not launch["complete"]
            or launch["exit_code"] != 0 or launch["source_game_written"]
            or launch["progress_published"] or not trial["warm"] or not trial["meter_consumed"]
            or not meta["complete"] or any(meta[k] for k in (
                "dropped", "io_failed", "cutoff_publishers", "gpu_profile_at_start",
                "compute_hash_at_start"))):
        raise ValueError("Expected an isolated, complete, warm protection audit")
    anchor = meta["start"]
    release = (anchor["ns_before"] + anchor["ns_after"]) // 2
    release += (trial["release"]["qpc_after"] - anchor["qpc"]) * 10**9 // anchor["frequency"]
    with (session / "path-trace.csv").open(encoding="utf-8-sig", newline="") as handle:
        phases = summarize(handle, meta, release)
    return dict(session=str(session), events=meta["events"], phases=phases,
                limits="Approximately one eighth of compute BindBuffers scopes. Calls are "
                       "actual VirtualProtectEx calls to PAGE_NOACCESS within writable guest "
                       "buffer acquisition. Adjacency requires the same Windows view and "
                       "consecutive calls within a scope. Adjacent ranges need a separate "
                       "ordering and version proof before they can be combined.")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("session", type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    result = analyze(args.session)
    args.output.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    for phase, data in result["phases"].items():
        print(phase, {key: value for key, value in data.items()
                      if key not in ("shaders", "bindings")})
