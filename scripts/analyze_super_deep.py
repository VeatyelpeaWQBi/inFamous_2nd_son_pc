"""Read-only evidence joins and bounded report requests for existing super captures."""
import bisect
import collections
import csv
from datetime import datetime, timezone
import json
from pathlib import Path
import statistics
import subprocess

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "doc/super-deep-20261031"
OUT.mkdir(exist_ok=True)
SOURCE = json.loads((ROOT / "doc/super-analysis-20261031.json").read_text(encoding="utf8"))


def load(p):
    return json.loads(p.read_text(encoding="utf-8-sig"))


def phase(t, full=False):
    bounds = [(-100, -1, "baseline"), (0, 25, "draw-rise"), (25, 42, "compute-peak"),
              (70, 110, "recovered")] if full else [(-12, 0, "baseline"), (0, 3, "early"),
              (3, 8, "middle"), (8, 12, "late"), (12, 25, "recovery")]
    return next((name for a, b, name in bounds if a <= t < b), None)


def numeric(row, key):
    try:
        return float(row[key])
    except (ValueError, KeyError):
        return 0.0


def cpu_summary(file):
    rows = list(csv.DictReader(file.open(encoding="utf-8-sig")))
    total = sum(numeric(r, "CPU Time") for r in rows)
    groups = {}
    for col in ("Thread", "Module"):
        group = collections.defaultdict(float)
        for r in rows:
            group[r.get(col, "unknown")] += numeric(r, "CPU Time")
        groups[col] = sorted(group.items(), key=lambda x: -x[1])[:20]
    return dict(cpu_s=total, groups=groups,
                functions=[{k: r.get(k) for k in ("Thread", "Function", "Module", "CPU Time",
                           "Performance-core (P-core):Memory Bound(%)",
                           "Performance-core (P-core):Memory Bound:DRAM Bound(%)")}
                           for r in sorted(rows, key=lambda r: -numeric(r, "CPU Time"))[:35]])


report = {"sessions": [], "limits": ["Per-second logs inherit last command context and straddle boundaries.",
    "Full is heavily instrumented; phase wall times are not transferable to native gameplay.",
    "Self CPU times are additive within a grouping; inclusive stacks and cross-thread waits are not.",
    "Nsight three detail frames are a local snapshot, not whole-super GPU coverage."]}
for base in SOURCE["sessions"]:
    session = ROOT / base["session"]
    cycles = load(session / "super-rounds.json")
    seq = {}
    full = "diagnostic-full" in session.name
    for cycle in cycles:
        if cycle.get("events_file"):
            for e in load(session / cycle["events_file"]):
                if "observed_unix_ms" in e:
                    seq[e["sequence"]] = (e["observed_unix_ms"] / 1000, e.get("frame_count"))
                elif "applied_unix_ns" in e:
                    seq[e["sequence"]] = (e["applied_unix_ns"] / 1e9, e.get("frame_count"))
    result = {"session": session.name, "runtime": base["runtime_sha256"], "profiles": {}, "trials": []}
    for trial in base["trials"]:
        if trial["status"] != "collected":
            continue
        release = trial["release_ns"] / 1e9
        c = next(c for c in cycles if c["cycle"] == trial["cycle"])
        rows = [r for r in base["log_samples"] if c["filled"]["sequence"] <= r["sequence"] <= c["after"]["sequence"]]
        groups = collections.defaultdict(list)
        for r in rows:
            if r["sequence"] in seq:
                label = phase(seq[r["sequence"]][0] - release, full)
                if label:
                    groups[label].append(r)
        trial_result = {"cycle": c["cycle"], "profile": c["profile"], "phases": {}}
        for name, rows in groups.items():
            values = {"log_samples": len(rows)}
            for k in sorted({k for r in rows for k in r} - {"sequence"}):
                v = [r[k] for r in rows if k in r]
                if isinstance(v[0], list):
                    a, b = [sum(x[j] for x in v) for j in (0, 1)]
                    values[k] = dict(hits=a, misses=b, hit_pct=100 * a / (a+b) if a+b else None)
                else:
                    values[k] = dict(mean=statistics.mean(v), max=max(v), min=min(v))
            trial_result["phases"][name] = values
        result["trials"].append(trial_result)
    report["sessions"].append(result)

full_session = ROOT / SOURCE["sessions"][-1]["session"]
cycle = load(full_session / "super-rounds.json")[0]
at = cycle["release"]["applied_unix_ns"] / 1e9
frame_rows = list(csv.DictReader((full_session / "frames.csv").open()))
frame_times = [int(r["utc_ns"]) / 1e9 for r in frame_rows]
summary = list(csv.reader((full_session / "diagnostics/vtune-summary.csv").open(encoding="utf-8-sig")))
stamp = next(r[2] for r in summary if len(r) == 3 and r[1] == "Collection start time")
start = datetime.strptime(stamp, "%H:%M:%S %d/%m/%Y UTC").replace(tzinfo=timezone.utc).timestamp()
vtune = Path("C:/Program Files (x86)/Intel/oneAPI/vtune/latest/bin64/vtune.exe")
report["cpu_phases"] = {}
for lo, hi, name in [(-100, -1, "baseline"), (0, 25, "draw-rise"), (25, 42, "compute-peak"), (70, 110, "recovered")]:
    a, b = at + lo + 1, at + hi - 1  # one-second summary precision, conservative edges
    args = [str(vtune), "-report", "hotspots", "-format", "csv", "-csv-delimiter", "comma",
            "-group-by", "thread,function", "-time-filter", f"{a-start:.6f}:{b-start:.6f}",
            "-result-dir", str(full_session / "diagnostics/vtune"),
            "-user-data-dir", str(full_session / "diagnostics/vtune-user")]
    target = OUT / f"cpu-{name}.csv"
    if not target.exists():
        with (OUT / f"cpu-{name}.log").open("w", encoding="utf8") as log:
            subprocess.run(args + ["-report-output", str(target)], stdout=log, stderr=log, check=True, timeout=180)
    report["cpu_phases"][name] = {"utc_begin": a, "utc_end": b, "seconds": b-a,
       "frames": bisect.bisect_left(frame_times, b)-bisect.bisect_left(frame_times, a), **cpu_summary(target)}

gpu_dir = full_session / "user/captures/nsight/BASE_UNLOCKED"
report["gpu_frame_metrics"] = {r[0]: r[1:] for r in csv.reader((gpu_dir / "GPUTRACE_FRAME.xls").open(), delimiter="\t")}
report["gpu_frame_duration_table"] = list(csv.reader((gpu_dir / "FRAME.xls").open(), delimiter="\t"))
OUT.joinpath("evidence.json").write_text(json.dumps(report, ensure_ascii=False, indent=2)+"\n", encoding="utf8")
for name, value in report["cpu_phases"].items():
    print(name, "frames", value["frames"], "wall", round(value["seconds"], 1), "CPU", round(value["cpu_s"], 2))
    print("threads", value["groups"]["Thread"][:7])
    print("modules", value["groups"]["Module"][:6])
print("Evidence:", OUT / "evidence.json")
