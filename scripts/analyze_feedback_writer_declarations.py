"""Join bounded writer declarations to demand-copy dependency snapshots."""

import argparse
import bisect
import csv
import json
import re
from collections import Counter
from pathlib import Path


def analyze(session, log, address):
    meta = json.loads((session / "path-trace.json").read_text(encoding="utf-8-sig"))
    if not meta["complete"] or any(meta[k] for k in ("dropped", "io_failed", "cutoff_publishers")):
        raise ValueError("Incomplete path trace")
    pattern = re.compile(r"PTRACE writer-declaration (\d+) range ([0-9a-f]+)\+(\d+) "
                         r"shader ([0-9a-f]+) queue (\d+) tick (\d+) sequence (\d+)")
    declarations = []
    cap_sequence = None
    for match in pattern.finditer(log.read_text(encoding="utf-8-sig", errors="replace")):
        serial, start, size, shader, queue, tick, seq = match.groups()
        if int(serial) == 8192:
            cap_sequence = int(seq)
        if int(start, 16) < address + 4 and address < int(start, 16) + int(size):
            declarations.append((int(seq), int(serial), shader, int(queue), int(tick), start, int(size)))
    declarations.sort()
    if not declarations:
        raise ValueError("No declarations cover the selected word")
    sequences = [row[0] for row in declarations]
    requests, dependencies = set(), []
    with (session / "path-trace.csv").open(encoding="utf-8-sig", newline="") as stream:
        for row in csv.DictReader(stream):
            if row["event"] == "read_request" and int(row["a"]) == address:
                requests.add(row["id"])
            elif row["event"] == "read_dependencies":
                dependencies.append(row)
    joined, exclusions = [], Counter()
    for row in dependencies:
        if row["id"] not in requests:
            continue
        snapshot = int(row["c"])
        if cap_sequence is not None and snapshot >= cap_sequence:
            exclusions["possible_log_cap"] += 1
            continue
        i = bisect.bisect_right(sequences, snapshot)
        if not i:
            exclusions["no_earlier_declaration"] += 1
            continue
        seq, _, shader, queue, tick, start, size = declarations[i - 1]
        joined.append(dict(id=row["id"], shader=shader, sequence=seq, queue=queue,
                           dependency_sequence=snapshot,
                           writer_tick=tick, dependency_tick=int(row[("a", "b")[queue]]),
                           range_start=start, range_size=size))
    return dict(session=session.name, address=hex(address), requests=len(requests),
                declarations=len(declarations),
                shader_declarations=dict(Counter(d[2] for d in declarations)),
                joined_shader_counts=dict(Counter(r["shader"] for r in joined)),
                same_tick=sum(r["writer_tick"] == r["dependency_tick"] for r in joined),
                exclusions=dict(exclusions), joined=joined,
                limits="Latest recorded overlapping declaration, not proof of an executed store. "
                       "A zero shader denotes an unattributed write path.")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("session", type=Path)
    parser.add_argument("log", type=Path)
    parser.add_argument("address", type=lambda value: int(value, 16))
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    result = analyze(args.session, args.log, args.address)
    args.output.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({k: v for k, v in result.items() if k != "joined"}))
