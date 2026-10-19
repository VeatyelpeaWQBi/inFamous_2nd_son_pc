"""Summarize recorded guest flips. Hardware presentation and image correctness are separate."""
import argparse
import csv
import json
import math
from pathlib import Path
import statistics
import sys
from project_paths import project_child, write_json
from launcher_settings import RESOLUTION_CONFIG, resolution_config

def summarize(path, start, end, mode, confirmations):
    width, height, _, _ = resolution_config(mode)
    if not (math.isfinite(start) and math.isfinite(end) and 0 <= start < end):
        raise ValueError("Invalid measurement window or mode")
    frames = []
    previous_elapsed, previous_index = None, None
    with path.open(newline="", encoding="utf-8-sig") as stream:
        for row in csv.DictReader(stream):
            index, elapsed, interval = (int(row[k]) for k in ("frame", "elapsed_ns", "interval_ns"))
            if previous_index is None:
                if (index, elapsed, interval) != (0, 0, 0):
                    raise ValueError("Frame timeline must begin at zero")
            elif index != previous_index + 1 or elapsed <= previous_elapsed:
                raise ValueError("Frame timeline is not contiguous and strictly increasing")
            elif elapsed - previous_elapsed != interval:
                raise ValueError("Interval and elapsed timestamps disagree")
            if previous_elapsed is not None and previous_elapsed >= start * 1e9 and elapsed <= end * 1e9:
                frames.append(interval / 1e6)
            previous_index, previous_elapsed = index, elapsed
    if previous_elapsed is None or previous_elapsed < end * 1e9:
        raise ValueError("The recording does not cover the requested measurement window")
    if len(frames) < 2:
        raise ValueError("Measurement contains fewer than two complete frame intervals")
    ordered = sorted(frames)
    mean = statistics.fmean(frames)
    def percentile(p):
        return ordered[math.ceil(len(ordered) * p) - 1]
    fps = 1000 / mean
    numeric_pass = None
    if mode == "1080p":
        numeric_pass = fps >= 59 and percentile(.95) <= 17.5
    elif mode == "pro4k":
        numeric_pass = fps >= 40 and percentile(.95) <= 25
    visual_pass = all(confirmations.get(key) is True for key in (
        "scene", "brightness", "particles", "resolution"))
    return {
        "schema": 2, "mode": mode, "counter_kind": "guest_flip",
        "resolution": {"width": width, "height": height},
        "baseline_target_applicable": mode in ("1080p", "pro4k"),
        "frames": len(frames), "duration_seconds": sum(frames) / 1000,
        "fps": fps, "mean_ms": mean, "p95_ms": percentile(.95),
        "p99_ms": percentile(.99), "max_ms": max(frames),
        "frames_over_50ms": sum(v > 50 for v in frames),
        "frames_over_100ms": sum(v > 100 for v in frames),
        "numeric_screen_pass": numeric_pass, "confirmations": confirmations,
        "baseline_acceptance": "candidate-pass" if numeric_pass and visual_pass else "not-verified",
        "hardware_present_verified": False,
        "note": "Guest flips do not prove scanout, no tearing, or a full-game stable framerate.",
    }

def main():
    p = argparse.ArgumentParser()
    p.add_argument("csv", type=Path)
    p.add_argument("--start", type=float, required=True)
    p.add_argument("--end", type=float, required=True)
    p.add_argument("--mode", choices=tuple(RESOLUTION_CONFIG), required=True)
    p.add_argument("--scene-confirmed", action="store_true")
    p.add_argument("--brightness-confirmed", action="store_true")
    p.add_argument("--particles-confirmed", action="store_true")
    p.add_argument("--resolution-confirmed", action="store_true")
    p.add_argument("--output", type=Path)
    a = p.parse_args()
    confirmations = {k: getattr(a, k + "_confirmed") for k in (
        "scene", "brightness", "particles", "resolution")}
    try:
        result = summarize(project_child(a.csv), a.start, a.end, a.mode, confirmations)
    except (OSError, ValueError, KeyError) as error:
        print(f"Measurement failed: {error}", file=sys.stderr)
        return 1
    if a.output:
        output = project_child(a.output)
        if not output.is_relative_to(project_child("state/sessions")):
            p.error("Measurement output must stay below state/sessions")
        write_json(output, result)
    print(json.dumps(result, ensure_ascii=False, indent=2))
    return 0
if __name__ == "__main__":
    sys.exit(main())
