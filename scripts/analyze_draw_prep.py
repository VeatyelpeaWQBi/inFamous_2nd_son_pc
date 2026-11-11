"""Read-only sampled direct-draw CPU preparation budget. Never launches tools or the game."""
import argparse
from collections import Counter, defaultdict
import csv
import json
from pathlib import Path

from analyze_super_batch import analyze as analyze_session, load
from analyze_timing import FIELDS
from project_paths import ROOT


PHASES = ("prelude", "pipeline_selection", "render_state", "resources",
          "attachments_vertex_index_barriers", "flat_upload_descriptors",
          "dynamic_state_rendering", "draw_record_and_retire")
KINDS = ("draw_prep_slice", "draw_prep_phases_0", "draw_prep_phases_1",
         "draw_prep_resources", "draw_prep_buffer_detail", "draw_prep_identity")
KINDS_V2 = ("draw_prep_slice", "draw_prep_phase", "draw_prep_identity")


def sampling_target(ordinal):
    """Mirror the unsigned native rotating nine-block sampling schedule."""
    mask = 2**64 - 1
    block = ordinal // 256
    selected = ((block * 0x9e3779b97f4a7c15) & mask) >> 56
    group = block // 9
    rotation = (((group * 0xd1342543de82ef95) & mask) >> 56) % 9
    target = ((block % 9) * (2 if group & 1 else 4) + rotation) % 9
    return selected, target


def decode_sample_v2(events):
    if len(events) != 3 or Counter(e['event'] for e in events) != Counter(KINDS_V2):
        raise ValueError('Incomplete or duplicated single-phase sample')
    if any(type(e[k]) is not int or not 0 <= e[k] < 2**64
           for e in events for k in ('thread', 'ns', 'id', 'a', 'b', 'c', 'd')):
        raise ValueError('Invalid unsigned single-phase field')
    if len({e['thread'] for e in events}) != 1 or len({e['id'] for e in events}) != 1 or not events[0]['id']:
        raise ValueError('Single-phase sample crossed producer threads or IDs')
    rows = {e['event']: e for e in events}
    whole, phase, identity = (rows[k] for k in KINDS_V2)
    begin, end, ordinal, flags = (whole[k] for k in 'abcd')
    selected, target = sampling_target(ordinal)
    if (flags >> 32 != 256 or flags & 0xffffffff not in (target, target | 256) or
            ordinal % 256 != selected or end < begin or end > min(e['ns'] for e in events)):
        raise ValueError('Invalid interval or deterministic phase sampling schedule')
    a, b, actual, reached = (phase[k] for k in 'abcd')
    if (actual != target or reached not in (0, 1) or not begin <= a <= b <= end or
            (not reached and (a != end or b != end)) or
            (target == 8 and (not reached or a != begin or b != end)) or identity['d'] not in (0, 1)):
        raise ValueError('Phase is outside its enclosing draw or has invalid reachability')
    return dict(begin=begin, end=end, ordinal=ordinal, thread=whole['thread'],
                recorded=bool(flags & 256), target=target, reached=bool(reached),
                measured_ns=b-a, total_ns=end-begin,
                vs=identity['a'], ps=identity['b'], indexed=bool(identity['d']))


def summarize_v2(samples, eop_count):
    if not samples or eop_count <= 0:
        raise ValueError('Missing single-phase samples or EOP intervals')
    targets = {i: [s for s in samples if s['target'] == i] for i in range(9)}
    if any(not rows for rows in targets.values()):
        raise ValueError('Window is missing a phase or the independent whole-draw control')
    scale = 256 * 9 / eop_count / 1e6
    estimate = {name: sum(s['measured_ns'] for s in targets[i]) * scale
                for i, name in enumerate(PHASES)}
    control = sum(s['measured_ns'] for s in targets[8]) * scale
    groups = defaultdict(dict)
    for s in samples:
        group = groups[s['ordinal'] // (256 * 9)]
        if s['target'] in group:
            raise ValueError('Duplicated target in a nine-block sampling group')
        group[s['target']] = s
    balanced = [g for g in groups.values() if len(g) == 9]
    balanced_phase = sum(sum(g[i]['measured_ns'] for i in range(8)) for g in balanced)
    balanced_whole = sum(g[8]['measured_ns'] for g in balanced)
    largest = []
    shaders = defaultdict(list)
    for s in targets[8]:
        shaders[s['vs'], s['ps']].append(s)
    for (vs, ps), rows in shaders.items():
        largest.append(dict(vs=hex(vs), ps=hex(ps), whole_control_samples=len(rows),
                            estimated_cpu_ms_per_eop=sum(s['measured_ns'] for s in rows) * scale))
    largest.sort(key=lambda row: row['estimated_cpu_ms_per_eop'], reverse=True)
    return dict(samples=len(samples), recorded_samples=sum(s['recorded'] for s in samples),
                complete_eop_intervals=eop_count, samples_per_target={i: len(r) for i, r in targets.items()},
                estimated_direct_draw_cpu_ms_per_eop=control,
                estimated_phase_cpu_ms_per_eop=estimate,
                phase_sum_to_whole_control_ratio=sum(estimate.values()) / control if control else None,
                balanced_nine_block_groups=len(balanced),
                balanced_phase_sum_to_whole_ratio=balanced_phase/balanced_whole if balanced_whole else None,
                largest_shader_pairs=largest[:16])


def decode_sample(events):
    if len(events) != 6 or Counter(e["event"] for e in events) != Counter(KINDS):
        raise ValueError("Incomplete or duplicated draw sample")
    if any(type(e[k]) is not int or not 0 <= e[k] < 2**64
           for e in events for k in ("thread", "ns", "id", "a", "b", "c", "d")):
        raise ValueError("Invalid unsigned draw sample field")
    by_kind = {e["event"]: e for e in events}
    threads = {e["thread"] for e in events}
    identifiers = {e["id"] for e in events}
    if len(threads) != 1 or len(identifiers) != 1 or 0 in identifiers:
        raise ValueError("Draw sample crossed producer threads or IDs")
    whole = by_kind["draw_prep_slice"]
    begin, end, ordinal, flags = (whole[k] for k in "abcd")
    if end < begin or end > whole["ns"] or flags >> 32 != 256 or flags & 0xffffffff not in (0, 1):
        raise ValueError("Invalid draw interval or sampling weight")
    phases = [by_kind[kind][key] for kind in KINDS[1:3] for key in "abcd"]
    resources = by_kind["draw_prep_resources"]
    detail = by_kind["draw_prep_buffer_detail"]
    if (sum(phases) != end - begin or resources["a"] + resources["b"] > phases[3] or
            detail["d"] > resources["a"] or sum(detail[k] for k in "abc") > detail["d"]):
        raise ValueError("Draw phases overlap or do not partition their enclosing CPU interval")
    identity = by_kind["draw_prep_identity"]
    if identity["d"] not in (0, 1):
        raise ValueError("Invalid indexed-draw identity")
    return dict(begin=begin, end=end, ordinal=ordinal, thread=whole["thread"],
                recorded=bool(flags & 1), phases=dict(zip(PHASES, phases)),
                total_ns=end-begin, buffers_ns=resources["a"], textures_ns=resources["b"],
                buffer_descriptors=resources["c"], texture_descriptors=resources["d"],
                buffer_detail=dict(zip(("obtain", "image_invalidation", "hazard_query", "full_bind"),
                                       (detail[k] for k in "abcd"))),
                vs=identity["a"], ps=identity["b"], indexed=bool(identity["d"]))


def summarize(samples, eop_count):
    if not samples or eop_count <= 0:
        raise ValueError("No sampled direct draws or complete EOP intervals")
    scale = 256 / eop_count / 1e6
    estimated = {key: sum(s["phases"][key] for s in samples) * scale for key in PHASES}
    resources = {key: sum(s[key] for s in samples) * scale for key in ("buffers_ns", "textures_ns")}
    detail = {key: sum(s["buffer_detail"][key] for s in samples) * scale
              for key in samples[0]["buffer_detail"]}
    shaders = defaultdict(list)
    for sample in samples:
        shaders[(sample["vs"], sample["ps"])].append(sample)
    largest = []
    for (vs, ps), rows in shaders.items():
        largest.append(dict(vs=hex(vs), ps=hex(ps), samples=len(rows),
                            estimated_cpu_ms_per_eop=sum(s["total_ns"] for s in rows) * scale,
                            mean_us_per_sample=sum(s["total_ns"] for s in rows) / len(rows) / 1e3))
    largest.sort(key=lambda row: row["estimated_cpu_ms_per_eop"], reverse=True)
    return dict(samples=len(samples), recorded_samples=sum(s["recorded"] for s in samples),
                complete_eop_intervals=eop_count,
                estimated_direct_draw_cpu_ms_per_eop=sum(estimated.values()),
                estimated_phase_cpu_ms_per_eop=estimated,
                inclusive_resource_cpu_ms_per_eop=resources,
                inclusive_buffer_detail_cpu_ms_per_eop=detail,
                mean_us_per_direct_draw=sum(s["total_ns"] for s in samples) / len(samples) / 1e3,
                mean_buffer_descriptors=sum(s["buffer_descriptors"] for s in samples) / len(samples),
                mean_texture_descriptors=sum(s["texture_descriptors"] for s in samples) / len(samples),
                largest_shader_pairs=largest[:16])


def analyze(session):
    summary = analyze_session(session)
    launch, preflight, meta = (load(session / name) for name in
                              ("launch.json", "preflight.json", "path-trace.json"))
    cycle, = load(session / "super-rounds.json")
    trial, = summary["trials"]
    request = load(ROOT / launch["super_capture_batch"] / "capture.json")
    if (not launch.get("super_capture") or (ROOT / request["session"]).resolve() != session.resolve() or
            request.get("status") != "collected" or not request.get("critical_capture") or
            not request.get("draw_prep_audit") or not request.get("normal_rendering") or
            request.get("candidate") != "feedback-arena-pool" or request.get("candidate_baseline") is not False or
            request.get("rounds_per_profile") != 1 or request.get("profiles") != ["lightweight"]):
        raise ValueError("Expected the matching owned one-release draw-audit batch")
    if (not summary.get("complete") or summary.get("exit_code") != 0 or
            launch.get("source_game_written") or launch.get("progress_published") or
            not launch.get("draw_prep_audit") or
            not launch.get("feedback_arena_pool_candidate") or
            preflight["runtime_sha256"] != launch["runtime_sha256"] or
            preflight["game_sha256"] != launch["game_sha256"] or
            trial.get("status") != "collected" or not trial.get("warm") or
            not trial.get("meter_consumed") or not trial.get("tuning_verified") or
            any(trial.get("compile_delta", {}).values()) or
            trial["requested_tuning"].get("compute_barrier_scope") != 1):
        raise ValueError("Expected a pinned, complete, warm compact-pool draw diagnostic")
    protocol = meta.get('draw_prep_protocol', 1)
    if protocol not in (1, 2):
        raise ValueError('Unknown draw diagnostic protocol')
    kinds = KINDS_V2 if protocol == 2 else KINDS
    decode = decode_sample_v2 if protocol == 2 else decode_sample
    aggregate = summarize_v2 if protocol == 2 else summarize
    if (not meta.get("complete") or any(meta.get(k, True) for k in
            ("dropped", "io_failed", "cutoff_publishers", "draw_prep_cap_hit")) or
            not meta.get("draw_prep_audit") or meta.get("draw_prep_sample_period") != 256 or
            meta.get("draw_prep_sample_cap") != 65536):
        raise ValueError("Draw audit is missing, incomplete or over its budget")
    for phase in ("before", "after"):
        point = cycle[phase]
        if (point.get("feedback_arena_pool_group_records") != 2048 or
                point.get("feedback_arena_pool_stride") != 256 or
                point.get("feedback_arena_pool_capacity") != 2048 or
                any(point.get("feedback_arena_pool_overflows", [1, 1])) or
                not all(v > 0 for v in point.get("feedback_arena_pool_moves", [0, 0]))):
            raise ValueError("Compact pool geometry/capacity/use was not verified")
    clock = meta["start"]
    origin = (clock["ns_before"] + clock["ns_after"]) // 2
    release = origin + (cycle["release"]["qpc_after"] - clock["qpc"]) * 10**9 // clock["frequency"]
    groups, flips, previous = defaultdict(list), defaultdict(list), {}
    first_ns, last_ns, rows = None, None, 0
    with (session / "path-trace.csv").open(encoding="utf-8-sig", newline="") as stream:
        reader = csv.DictReader(stream)
        if reader.fieldnames != FIELDS:
            raise ValueError("Unexpected path-trace schema")
        for rows, raw in enumerate(reader, 1):
            row = {k: int(v) if k != "event" else v for k, v in raw.items()}
            thread, seq, ns = (row[k] for k in ("thread", "seq", "ns"))
            old_seq, old_ns = previous.get(thread, (-1, 0))
            if seq != old_seq + 1 or ns < old_ns or not 0 <= thread < meta["registered_threads"]:
                raise ValueError("Discontinuous producer trace")
            previous[thread] = seq, ns
            first_ns = ns if first_ns is None else min(first_ns, ns)
            last_ns = ns if last_ns is None else max(last_ns, ns)
            if row["event"] == "draw_prep_limit":
                raise ValueError("Draw audit sample budget exhausted")
            if row["event"] in kinds:
                groups[row["id"]].append(row)
            elif row["event"] == "flip_eop_register":
                flips[thread].append(ns)
    if rows != meta["events"] or len(flips) != 1:
        raise ValueError("Event count or EOP registration thread mismatch")
    samples, incomplete = [], []
    for ident, events in groups.items():
        if len(events) != len(kinds):
            incomplete.append(dict(id=ident, first_ns=min(e["ns"] for e in events)))
            continue
        samples.append(decode(events))
    samples.sort(key=lambda s: s["begin"])
    if not samples or len({s["thread"] for s in samples}) != 1:
        raise ValueError("Missing samples or multiple direct-draw caller threads")
    for old, new in zip(samples, samples[1:]):
        if old["end"] > new["begin"] or old["ordinal"] >= new["ordinal"]:
            raise ValueError("Overlapping or nonmonotonic direct draws")
        if new["ordinal"] // 256 != old["ordinal"] // 256 + 1:
            raise ValueError("Missing or duplicated sampling block")
    stamps, = flips.values()
    if stamps != sorted(set(stamps)):
        raise ValueError("Nonmonotonic EOP registration")
    phases = {}
    for name, lo, hi in (("baseline", -8, -1), ("early", 0, 3), ("peak", 3, 8),
                          ("late", 8, 12), ("recovery", 12, 25)):
        left, right = release + lo * 10**9, release + hi * 10**9
        pairs = [(a, b) for a, b in zip(stamps, stamps[1:]) if left <= a < b <= right]
        if not pairs or first_ns > left or last_ns < right or any(e["first_ns"] < right for e in incomplete):
            raise ValueError("Incomplete diagnostic phase: " + name)
        window_start, window_end = pairs[0][0], pairs[-1][1]
        selected = [s for s in samples if window_start <= s["begin"] <= s["end"] <= window_end]
        phases[name] = aggregate(selected, len(pairs))
        phases[name].update(window_start_ns=window_start, window_end_ns=window_end,
                            eop_mean_interval_ms=(window_end-window_start) / len(pairs) / 1e6)
    return dict(session=str(session), runtime_sha256=launch["runtime_sha256"], events=rows,
                sample_count=len(samples), direct_draw_thread=samples[0]["thread"],
                release_ns=release, phases=phases, incomplete_boundary_samples=incomplete,
                performance_acceptance=False, protocol=protocol,
                budget_acceptance=False,
                limitation="Only direct-draw CPU wall time, sampled once per 256-call block. "
                           "Per-EOP numbers are weighted estimates, not exact self time or "
                           "a rendered frame critical path. Buffer/texture and buffer-detail "
                           "times are nested and must not be added to their parent phases. "
                           "Indirect draws, compute dispatches and unsampled outliers remain outside "
                           "this budget. Protocol 1 enables nested clocks and is too intrusive for "
                           "budget acceptance. Protocol 2 times one phase per selected call, plus "
                           "an independent whole-draw control once per nine sampling blocks. "
                           "Phase estimates are from different calls: compare their sum with the control, "
                           "do not assume equality. Clock/trace overhead and sampling variation remain. "
                           "This diagnostic is never a lightweight FPS acceptance trial.")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("session", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    result = analyze(args.session)
    args.output.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    for name, phase in result["phases"].items():
        print(name, phase["samples"], phase["estimated_direct_draw_cpu_ms_per_eop"],
              phase["estimated_phase_cpu_ms_per_eop"])
