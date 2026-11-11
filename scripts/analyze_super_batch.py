"""Offline super evidence join. Keeps raw sessions unchanged; uses only stdlib."""
import argparse
from bisect import bisect_left
from collections import defaultdict
import csv
import json
import math
from pathlib import Path
import re
import statistics
import subprocess


def load(path):
    return json.loads(path.read_text(encoding="utf-8-sig"))


def stats(values):
    if not values:
        return {}
    ordered = sorted(values)
    return dict(n=len(values), fps=1000 / statistics.mean(values),
                mean_ms=statistics.mean(values), p95_ms=ordered[math.ceil(len(values)*.95)-1],
                p99_ms=ordered[math.ceil(len(values)*.99)-1], max_ms=max(values),
                over50=sum(v > 50 for v in values), over100=sum(v > 100 for v in values))


FEEDBACK_ACTIVITY = ("feedback_checkpoint_attempts", "feedback_checkpoint_submits",
                     "feedback_early_complete_submits", "feedback_early_complete_fallbacks",
                     "writer_footprint_proven", "writer_footprint_fallbacks",
                     "writer_footprint_narrowed",
                     "feedback_writer_hits", "feedback_writer_fallbacks", "feedback_writer_reductions",
                     "feedback_word_attempts", "feedback_word_served", "feedback_word_fallbacks",
                     "feedback_word_bulk_queued",
                     "feedback_word_cluster_served",
                     "written_sync_skips",
                     "image_sync_queries_skipped", "image_sync_copies_skipped",
                     "readback_copy_stage_narrowed",
                     "native_compute", "native_graphics_compute", "gds_scope_bindings",
                     "gds_scope_conflicts_reduced", "gds_scope_barriers_avoided", "compute_recorder_submits")


PATTERNS = {
    "feedback_checkpoint_attempts": r"Perf feedback: checkpoint attempts (\d+)",
    "feedback_checkpoint_submits": r"Perf feedback: checkpoint attempts \d+ submits (\d+)",
    "feedback_writer_hits": r"Perf feedback:.*?writer hits (\d+)",
    "feedback_writer_fallbacks": r"Perf feedback:.*?writer hits \d+ fallback (\d+)",
    "feedback_writer_reductions": r"Perf feedback:.*?reduced (\d+)",
    "compute_barriers": r"Perf compute barriers: narrowed (\d+) fallback (\d+)",
    "command_sharps": r"Perf sharps: decoded (\d+) reused (\d+) fallback (\d+)",
    "fps": r"Perf: ([\d.]+) fps", "gpu_busy": r"gpu busy ([\d.]+)%",
    "draws": r"draws/frame (\d+)", "dispatches": r"dispatches/frame (\d+)",
    "shaders": r"shaders (\d+)", "pipelines": r"pipelines (\d+)",
    "compile_stall_ms": r"ms compile, ([\d.]+) ms stall",
    "readback_stall_ms": r"readbacks.*?\(([\d.]+) ms stalled",
    "readback_async_ms": r"async \d+ waiting ([\d.]+) ms",
    "readback_reads": r"readbacks R(\d+)",
    "readback_downloads": r"readbacks R\d+ W\d+ D(\d+)",
    "readback_mb": r"gpu sync [\d.]+ ms, ([\d.]+) MB",
    "readback_prefetches": r"prefetch (\d+) flushes",
    "readback_prefetch_flushes": r"prefetch \d+ flushes (\d+)",
    "asc_start_delay_ms": r"asc \d+ \(start delay ([\d.]+) ms",
    "asc_busy_percent": r"asc \d+ \(start delay [\d.]+ ms, busy ([\d.]+)%",
    "readback_queue_ms": r"ms stalled: queue ([\d.]+) ms",
    "history_evictions": r"readback history evicted (\d+)",
    "history_demand_evictions": r"readback history evicted \d+ demand (\d+)",
    "gpu_word_summary": r"gpu word summary (\d+)/(\d+)",
    "readback_order_avoidable": r"readback page order avoidable (\d+)",
    "readback_order_bypasses": r"readback page order avoidable \d+ bypassed (\d+)",
    "protect_ms": r"protect \d+ \(([\d.]+) ms\)",
    "protect_calls": r"protect (\d+) \(",
    "cp_work_pct": r"gpu thread work (\d+)%",
    "recorder_commands": r"recorder jobs \d+ commands (\d+)",
    "recorder_sync_ms": r"recorder jobs \d+ commands \d+ syncs \d+ \(([\d.]+) ms\)",
    "recorder_busy_pct": r"recorder jobs \d+ commands \d+ syncs \d+ \([\d.]+ ms\) busy (\d+)%",
    "producer_done": r"readback producers done (\d+)",
    "producer_current": r"readback producers done \d+ submitted \d+ queued \d+ current (\d+)",
    "dependency_checked": r"readback dependencies: checked (\d+)",
    "dependency_reduced": r"readback dependencies: checked \d+ reduced (\d+)",
    "dependency_fallback": r"reduced \d+ fallback (\d+)",
    "queue_promotions": r"queue promotions (\d+)",
    "recorder_backlog_ms": r"recorder backlog wait ([\d.]+) ms",
    "completion_requests": r"completion requests (\d+)",
    "completion_queue_ms": r"completion requests \d+ queue ([\d.]+) ms",
    "barrier_calls": r"image barriers (\d+) calls", "barrier_fast": r"calls (\d+) fast",
    "barrier_memo": r"fast \d+ memo (\d+) dirty", "barrier_emitted": r"subresources (\d+) emitted",
    "params": r"shader params memo (\d+)/(\d+)",
    "selection": r"selection (\d+)/(\d+)", "tex": r" spec \d+/\d+ tex (\d+)/(\d+)",
    "spec": r"memo sync \d+/\d+ spec (\d+)/(\d+)", "rt": r"rt memo (\d+)/(\d+)",
    "flat": r"flat uploads (\d+)/(\d+)",
}


def analyze(session):
    launch = load(session / "launch.json")
    owner = session.resolve().parents[2] / launch.get('super_capture_batch', 'state/nonexistent-batch')
    capture = load(owner/'capture.json') if (owner/'capture.json').is_file() else {}
    observe_environment = capture.get('observe_environment') is True
    result = dict(session=str(session), runtime_sha256=launch["runtime_sha256"],
                  game_sha256=launch.get("game_sha256"),
                  mode=launch["mode"], complete=launch["complete"],
                  exit_code=launch.get("exit_code"), saves=launch.get("save_source_files"),
                  trials=[], log_samples=[], profiles=[])
    if (session / "diagnostics/status.json").exists():
        state = load(session / "diagnostics/status.json")
        result["full_status"] = {k: state.get(k) for k in
                                 ("status", "errors", "window_started", "window_finished", "elapsed_s")}
    if not (session / "super-rounds.json").exists():
        result["limitation"] = "No super-round records; cannot attribute this session to a measured super release."
        return result
    cycles = load(session / "super-rounds.json")
    frames = list(csv.DictReader((session / "frames.csv").open(encoding="utf-8-sig")))
    timestamps = [int(f["utc_ns"]) for f in frames]
    intervals = [int(f["interval_ns"])/1e6 for f in frames]
    sequence = 0
    log = session / "log/CUSA00309.log"
    if launch.get("super_capture_batch"):
        mirror = session.resolve().parents[2] / launch["super_capture_batch"] / "launcher-output.log"
        if mirror.exists() and (not log.exists() or mirror.stat().st_size > log.stat().st_size + 1024):
            log = mirror  # The console mirror survives the ordinary file logger's size cap.
    result["log_source"] = str(log)
    result["renderer_thread_affinities"] = {}
    with log.open(errors="replace") as file:
        for line in file:
            affinity = re.search(r"Perf CPU: thread (shadPS4:\w+) on logical processors ([0-9,-]+)", line)
            if affinity:
                mask = 0
                for span in affinity[2].split(","):
                    ends = [int(v) for v in span.split("-")]
                    for cpu in range(ends[0], ends[-1] + 1):
                        mask |= 1 << cpu
                result["renderer_thread_affinities"][affinity[1]] = mask
            match = re.search(r"Benchmark command (\d+):", line)
            if match:
                sequence = int(match[1])
            if "OnFlip: Perf:" in line:
                row = {"sequence": sequence}
                for name, pattern in PATTERNS.items():
                    match = re.search(pattern, line)
                    if match:
                        row[name] = ([int(v) for v in match.groups()] if len(match.groups()) > 1
                                     else float(match[1]))
                result["log_samples"].append(row)
            elif ("OnFlip: Perf V1:" in line or "OnFlip: Perf V2:" in line or "OnFlip: Perf sharps:" in line or "OnFlip: Perf compute barriers:" in line or "OnFlip: Perf feedback:" in line) and result["log_samples"]:
                # The immediately following lines complete the same periodic snapshot.
                row = result["log_samples"][-1]
                if row["sequence"] == sequence:
                    for name, pattern in PATTERNS.items():
                        match = re.search(pattern, line)
                        if match:
                            row[name] = ([int(v) for v in match.groups()] if len(match.groups()) > 1
                                         else float(match[1]))
            match = re.search(r"Perf (profile|inclusive) \[shadPS4:GpuCommandProcessor\]\s+([\d.]+)% (.+)", line)
            if match:
                result["profiles"].append(dict(sequence=sequence, kind=match[1],
                                               percent=float(match[2]), symbol=match[3].strip()))
    for cycle in cycles:
        trial = dict(cycle=cycle["cycle"], kind=cycle["kind"], profile=cycle["profile"],
                     status=cycle["status"], warm=cycle["warm"], meter_consumed=cycle["meter_consumed"],
                     variant=cycle.get("variant", "baseline"))
        trial["scene_before"] = {k: (cycle.get("before") or {}).get(k)
                                 for k in ("draws_per_frame", "dispatches_per_frame")}
        trial["requested_tuning"] = cycle.get("requested_tuning", {})
        trial["requested_ini_tuning"] = cycle.get("requested_ini_tuning")
        if not cycle.get("release"):
            result["trials"].append(trial)
            continue
        events = load(session / cycle["events_file"])
        release = cycle["release"]
        at = int(release["applied_unix_ns"])
        samples = [e for e in events if "draws_per_frame" in e]
        tuning_samples = [cycle.get("filled"), cycle.get("before"), *samples, cycle.get("after")]
        trial["tuning_verified"] = bool(trial["requested_tuning"]) and all(
            sample is not None and all(sample.get(k) == value
                                       for k, value in trial["requested_tuning"].items())
            for sample in tuning_samples)
        trial["release_ns"] = at
        trial["release_sequence"] = release["sequence"]
        trial["compile_delta"] = {k: cycle["after"][k]-cycle["filled"][k]
                                  for k in ("shader_compile_starts", "pipeline_compile_starts")}
        trial["mechanism_delta"] = {k: cycle["after"][k] - cycle["before"][k]
                                    for k in FEEDBACK_ACTIVITY
                                    if k in cycle["after"] and k in (cycle.get("before") or {})}
        trial["snapshots"] = [dict(seconds=(e["observed_unix_ms"]*1e6-at)/1e9,
                                    draws=e["draws_per_frame"], dispatches=e["dispatches_per_frame"],
                                    meter=e["karma_fill_count"], gpu_profile=e["gpu_profile"],
                                    thread_cpu=e.get("thread_cpu"),
                                    feedback_activity={k: e[k] for k in FEEDBACK_ACTIVITY if k in e})
                              for e in samples]
        trial["windows"] = {}
        for label, begin, end in [("baseline", -10, 0), ("super_0_12", 0, 12),
                                  ("early", 0, 3), ("peak", 3, 8),
                                  ("late", 8, 12), ("recovery", 12, 25), ("tail", 25, 30)]:
            low, high = bisect_left(timestamps, at+begin*1e9), bisect_left(timestamps, at+end*1e9)
            trial["windows"][label] = stats(intervals[low:high])
        first_flip = bisect_left(timestamps, at)
        trial["input_to_first_flip_ms"] = (timestamps[first_flip]-at)/1e6
        # Supplement wall-time phases with complete post-input frame intervals.
        # These are a sensitivity check, not proof of deterministic guest simulation.
        trial["frame_windows"] = {
            f"first_{count}": stats(intervals[first_flip+1:first_flip+1+count])
            for count in (120, 240, 360, 480, 600)
            if first_flip+count < len(intervals)
        }
        trial["seconds"] = []
        for second in range(30):
            low, high = bisect_left(timestamps, at+second*1e9), bisect_left(timestamps, at+(second+1)*1e9)
            trial["seconds"].append(stats(intervals[low:high]))
        before_seq = cycle["filled"]["sequence"]
        after_seq = cycle["after"]["sequence"]
        trial["log_context"] = {}
        for label, begin, end in [("baseline", before_seq, release["sequence"]),
                                  ("release", release["sequence"], next((e["sequence"] for e in samples
                                           if e["observed_unix_ms"]*1e6-at >= 12e9), after_seq)),
                                  ("all_release", release["sequence"], after_seq)]:
            rows = [r for r in result["log_samples"] if begin <= r["sequence"] < end]
            data = {"n": len(rows)}
            for key in PATTERNS:
                numbers = [r[key] for r in rows if key in r]
                if not numbers:
                    continue
                if isinstance(numbers[0], list):
                    hits = sum(v[0] for v in numbers)
                    misses = sum(v[1] for v in numbers)
                    if key == "command_sharps":
                        reused = sum(v[1] for v in numbers)
                        fallback = sum(v[2] for v in numbers)
                        data[key] = dict(decoded=hits, reused=reused, fallback=fallback,
                                         reuse_percent=100*reused/(reused+fallback) if reused+fallback else None)
                    elif key == "compute_barriers":
                        data[key] = dict(narrowed=hits, fallback=misses,
                                         narrow_percent=100*hits/(hits+misses) if hits+misses else None)
                    elif key == "gpu_word_summary":
                        data[key] = dict(hits=hits, checks=misses,
                                         hit_percent=100*hits/misses if misses else None)
                    else:
                        data[key] = dict(hits=hits, misses=misses,
                                         hit_percent=100*hits/(hits+misses) if hits+misses else None)
                else:
                    data[key] = dict(mean=statistics.mean(numbers), min=min(numbers), max=max(numbers))
            trial["log_context"][label] = data
        trial["gpu_busy_available"] = cycle["profile"] in ("compute", "graphics", "full")
        if observe_environment:
            from analyze_super_environment import analyze as environment_analysis
            trial['environment'] = environment_analysis(owner, launch, cycle['release']['qpc_after'])
            if capture.get('session') != launch.get('session'):
                trial['environment'] = dict(qualified=False, issues=['Environment batch/session mismatch'])
        result["trials"].append(trial)
    result["comparison_warnings"] = []
    experiment = None
    batch_state = {}
    if launch.get("super_capture_batch"):
        batch = session.resolve().parents[2] / launch["super_capture_batch"] / "capture.json"
        if batch.exists():
            batch_state = load(batch)
            experiment = batch_state.get("experiment", "none")
    result["experiment"] = experiment
    result["variant_order"] = batch_state.get("variant_order")
    for trial in result["trials"]:
        trial["comparison_eligible"] = trial["status"] == "collected"
        if observe_environment and not trial.get('environment', {}).get('qualified'):
            trial['comparison_eligible'] = False
            result['comparison_warnings'].append('External load observation did not qualify; no candidate elimination or performance acceptance.')
    if experiment and experiment != "none":
        for profile in {t["profile"] for t in result["trials"]}:
            measured = [t for t in result["trials"]
                        if t["profile"] == profile and t["status"] == "collected"]
            if not measured:
                continue
            first = min(t["cycle"] for t in measured)
            interrupted = any(t["profile"] == profile and t["cycle"] > first and
                              any(t.get("compile_delta", {}).values())
                              for t in result["trials"])
            if batch_state.get("simple_native"):
                interrupted = any(t["profile"] == profile and t["kind"] == "measurement" and
                                  (not t["warm"] or any(t.get("compile_delta", {}).values()))
                                  for t in result["trials"])
            if interrupted:
                result["comparison_warnings"].append(
                    f"{profile}: the pair contains new compilation; retained for inspection, excluded from warm A/B comparison.")
                for trial in measured:
                    trial["comparison_eligible"] = False
            if batch_state.get("simple_native"):
                # Compilation qualification alone does not prove equivalent scenes.
                # These rolling counters are a conservative drift alert, not an
                # image-equivalence test or a performance acceptance threshold.
                paired = sorted((t for t in result["trials"]
                                 if t["profile"] == profile and t["kind"] == "measurement"),
                                key=lambda t: t["cycle"])
                for offset in range(0, len(paired), 2):
                    pair = paired[offset:offset+2]
                    issues = []
                    if len(pair) != 2 or any(t["status"] not in ("collected", "captured-new-compilation")
                                             for t in pair):
                        issues.append("incomplete A/B pair")
                    if any(not t.get("tuning_verified") for t in pair):
                        issues.append("requested tuning not verified throughout the release")
                    if len(pair) == 2:
                        for key in ("draws_per_frame", "dispatches_per_frame"):
                            a, b = (t["scene_before"].get(key) for t in pair)
                            if a is None or b is None or a <= 0:
                                issues.append(f"missing valid baseline {key}")
                            elif abs(b/a-1) > .10:
                                issues.append(f"baseline {key} changed {100*(b/a-1):+.1f}% (>10% drift alert)")
                    if issues:
                        result["comparison_warnings"].append(
                            f"{profile}, cycles {[t['cycle'] for t in pair]}: " + "; ".join(issues) +
                            "; raw FPS retained, excluded from optimization attribution.")
                        for trial in pair:
                            trial["comparison_eligible"] = False
    return result


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("sessions", type=Path, nargs="+")
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    results = [analyze(s) for s in args.sessions]
    args.output.write_text(json.dumps(dict(sessions=results, limitations=[
        "Log contexts follow the last logged command; samples can straddle boundaries.",
        "Inclusive CPU stack percentages overlap and must never be added.",
        "Heavy diagnostic FPS is not a normal-game performance baseline."
    ]), ensure_ascii=False, indent=2)+"\n", encoding="utf-8")
    for session in results:
        print(Path(session["session"]).name, session.get("full_status", {}).get("status", "native"))
        groups = defaultdict(list)
        for trial in session["trials"]:
            if trial["status"] == "collected" and trial.get("comparison_eligible", True):
                groups[(trial["profile"], trial["variant"])].append(trial)
        for warning in session.get("comparison_warnings", []):
            print("COMPARISON EXCLUDED:", warning)
        for profile, trials in groups.items():
            print(profile, len(trials), {key: round(statistics.mean(t["windows"][key]["fps"] for t in trials), 2)
                  for key in ("baseline", "early", "peak", "late", "recovery")},
                  "compile deltas", [t["compile_delta"] for t in trials])


if __name__ == "__main__":
    main()
