# SPDX-License-Identifier: GPL-2.0-or-later
"""Inspect retained native counter logs for allocation tails; never launches a process."""
import argparse
from collections import Counter
import json
from pathlib import Path
import re

SLOW = re.compile(r'Perf slow frame #(\d+): ([\d.]+) ms')
ALLOC = re.compile(r'staging allocations (\d+) \(([\d.]+) ms\)')
READ = re.compile(r'readbacks R\d+ W\d+ D\d+ \(([\d.]+) ms stalled')


def analyze(root):
    result = []
    for directory in sorted((root/'state/super-captures').iterdir()):
        capture = directory/'capture.json'
        log = directory/'launcher-output.log'
        if not directory.is_dir() or not capture.is_file() or not log.is_file():
            continue
        batch = json.loads(capture.read_text(encoding='utf-8-sig'))
        if batch.get('collector_mode') != 'native':
            continue
        slow, observed, rows = 0, Counter(), []
        with log.open(encoding='utf-8-sig',errors='replace') as f:
            for line in f:
                match = SLOW.search(line)
                if not match:
                    continue
                slow += 1
                frame, ms = int(match[1]), float(match[2])
                allocation, read = ALLOC.search(line), READ.search(line)
                if ms >= 100: observed['slow_frames_ge_100ms'] += 1
                if not allocation or not int(allocation[1]):
                    continue
                n, alloc_ms = int(allocation[1]), float(allocation[2])
                observed['slow_frames_with_allocations'] += 1
                observed['allocation_count_in_slow_frames'] += n
                observed['allocation_ms_in_slow_frames_not_wall'] += alloc_ms
                if alloc_ms >= 10: observed['allocation_frames_ge_10ms'] += 1
                if alloc_ms >= 100: observed['allocation_frames_ge_100ms'] += 1
                rows.append(dict(frame=frame,frame_ms=ms,allocation_count=n,
                    allocation_ms=alloc_ms,readback_handler_ms=float(read[1]) if read else None))
        result.append(dict(batch=directory.name,session=batch.get('session'),status=batch.get('status'),
            profiles=batch.get('profiles'),log_bytes=log.stat().st_size,
            slow_frames_logged=slow,observed=dict(observed),
            largest_allocations=sorted(rows,key=lambda row:-row['allocation_ms'])[:8]))
    return dict(scope='retained native slow-frame counter reports only; not Full, not phase-resolved',
                limits=['Nested readback/allocation timers must not be added.',
                        'Only frames printed as slow have per-frame allocation details.',
                        'Perf log frame number differs from benchmark/native frame counter.',
                        'Lightweight captures with counters disabled cannot prove absence of allocation stalls.',
                        'No source of allocation size/type is recorded in the old timer.'],batches=result)


if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--output',type=Path,required=True)
    args = p.parse_args()
    report = analyze(Path(__file__).resolve().parents[1])
    args.output.write_text(json.dumps(report,indent=2)+'\n')
    for batch in report['batches']:
        if batch['slow_frames_logged']:
            print(batch['batch'],batch['slow_frames_logged'],batch['observed'],
                  batch['largest_allocations'][:2])
