"""Offline association audit, not a causal scheduling or residency measurement."""
import argparse
from collections import defaultdict
import json
from pathlib import Path
import re
import statistics


def load(path):
    return json.loads(path.read_text(encoding="utf-8-sig"))


def main():
    parser = argparse.ArgumentParser(__doc__)
    parser.add_argument("--topology", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    classes = {lp: c["efficiency_class"] for c in load(args.topology)["cores"]
               if c["group"] == 0 for lp in c["logical_processors"]}
    results = []
    pattern = re.compile(r"Perf: ([\d.]+) fps.*?\(cpu (\d+)\).*?draws/frame (\d+) dispatches/frame (\d+)")
    for rounds in sorted((root / "state/sessions").glob("*/super-rounds.json")):
        session = rounds.parent
        launch = load(session / "launch.json")
        log = session / "log/CUSA00309.log"
        if launch.get("super_capture_batch"):
            mirror = root / launch["super_capture_batch"] / "launcher-output.log"
            if mirror.exists() and (not log.exists() or mirror.stat().st_size > log.stat().st_size + 1024):
                log = mirror
        if not log.exists():
            continue
        cycles = load(rounds)
        times = {}
        for cycle in cycles:
            event_path = session / cycle.get("events_file", "missing")
            if not event_path.is_file():
                continue
            for e in load(event_path):
                if "sequence" in e and "applied_unix_ns" in e:
                    times[e["sequence"]] = e["applied_unix_ns"] / 1e9
        samples, pending, previous = [], [], None
        with log.open(errors="replace") as file:
            for line in file:
                command = re.search(r"Benchmark command (\d+):", line)
                if command:
                    following = int(command[1])
                    for row in pending:
                        row["following_sequence"] = following
                    pending.clear()
                    previous = following
                match = pattern.search(line)
                if match:
                    fps, cpu, draws, dispatches = match.groups()
                    row = dict(fps=float(fps), cpu=int(cpu), draws=int(draws), dispatches=int(dispatches),
                               preceding_sequence=previous, following_sequence=None)
                    samples.append(row)
                    pending.append(row)
        if not samples:
            continue
        groups = defaultdict(list)
        included = []
        for row in samples:
            low, high = (times.get(row[k]) for k in ("preceding_sequence", "following_sequence"))
            if low is None or high is None or high < low or high-low > 15:
                continue
            # Reports summarize roughly one second. Require the whole possible report
            # interval, including the preceding second, to stay inside one phase.
            for cycle in cycles:
                release = cycle.get("release")
                if not release or not cycle.get("warm"):
                    continue
                at = release["applied_unix_ns"] / 1e9
                phase = next((label for label, start, end in
                              (("baseline", -10, 0), ("early", 0, 3), ("peak", 3, 8),
                               ("late", 8, 12), ("recovery", 12, 25), ("tail", 25, 30),
                               ("release-unsplit", 0, 30))
                              if start <= low-at-1 and high-at < end), None)
                if phase:
                    row.update(cycle=cycle["cycle"], phase=phase, efficiency_class=classes.get(row["cpu"]),
                               seconds_bracket=[low-at, high-at], profile=cycle["profile"])
                    included.append(row)
                    groups[(cycle["profile"], phase, row["efficiency_class"])].append(row)
                    break
        aggregate = []
        for (profile, phase, cls), rows in groups.items():
            aggregate.append(dict(profile=profile, phase=phase, efficiency_class=cls, n=len(rows),
                                  median_fps=statistics.median(r["fps"] for r in rows),
                                  median_draws=statistics.median(r["draws"] for r in rows),
                                  median_dispatches=statistics.median(r["dispatches"] for r in rows)))
        class_counts = {str(cls): sum(classes.get(r['cpu']) == cls for r in samples)
                        for cls in sorted(set(classes.values()))}
        results.append(dict(session=str(session.relative_to(root)), log=str(log.relative_to(root)),
                            runtime_sha256=launch["runtime_sha256"], total_log_samples=len(samples),
                            all_log_samples_by_efficiency_class=class_counts,
                            included=len(included), aggregates=aggregate, samples=included))
    output = dict(schema=1, topology=str(args.topology), sessions=results,
                  limitation="CPU is the CP's last GetCurrentProcessorNumber sample at the end of its outer work loop, not residency or recorder placement. Time brackets come from adjacent control receipts; logs summarize about one second. Profiles, versions and phases remain separate; association cannot establish causality or remove thermal/foreground/workload confounding.")
    args.output.write_text(json.dumps(output, indent=2) + "\n", encoding="utf-8")
    print(json.dumps([dict(session=s["session"], total=s["total_log_samples"], included=s["included"],
                           aggregates=s["aggregates"]) for s in results if s["included"]], indent=2))


if __name__ == "__main__":
    main()
