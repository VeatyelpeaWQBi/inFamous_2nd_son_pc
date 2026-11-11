"""Join super-move input receipts with frames and bounded diagnostic log windows.

Per-second log groups follow the last acknowledged command logged by the engine;
they are contextual samples, not exact per-frame attribution or additive timings.
"""
import argparse
import csv
import json
import math
from pathlib import Path
import re
import statistics


def summarize(session):
    frames = list(csv.DictReader((session / 'frames.csv').open(encoding='utf-8-sig')))
    records, sequence = [], 0
    for line in (session / 'log/CUSA00309.log').read_text(errors='replace').splitlines():
        match = re.search(r'Benchmark command (\d+):', line)
        if match:
            sequence = int(match[1])
        if 'OnFlip: Perf:' not in line:
            continue
        row = {'sequence_context': sequence}
        for name, pattern in {
            'fps': r'Perf: ([\d.]+) fps', 'shaders': r'shaders (\d+)',
            'pipelines': r'pipelines (\d+)', 'stall_ms': r'compile, ([\d.]+) ms stall',
            'gpu_busy_pct': r'gpu busy ([\d.]+)%',
            'draws': r'draws/frame (\d+)', 'dispatches': r'dispatches/frame (\d+)',
        }.items():
            match = re.search(pattern, line)
            if match:
                row[name] = float(match[1])
        for name, pattern in {
            'shader_params': r'shader params memo (\d+)/(\d+)',
            'selection': r'selection (\d+)/(\d+)',
            'texture': r' tex (\d+)/(\d+)',
            'specialization': r' spec (\d+)/(\d+)',
            'render_target': r'rt memo (\d+)/(\d+)',
        }.items():
            match = re.search(pattern, line)
            if match:
                row[name] = list(map(int, match.groups()))
        records.append(row)
    results = []
    for trial in sorted(session.glob('super-trial-*.json')):
        events = json.loads(trial.read_text(encoding='utf-8-sig'))
        button = next(e for e in events if e.get('action') == 'button')
        at, seq = int(button['applied_unix_ns']), button['sequence']
        first = next((e['sequence'] for e in events if e.get('action') == 'karma-fill'), seq)
        samples = [e for e in events if 'draws_per_frame' in e]
        end_seq = next((e['sequence'] for e in samples
                        if e['observed_unix_ms'] * 1000000 - at >= 12e9), samples[-1]['sequence'])
        end = max(e['sequence'] for e in samples)
        windows = {}
        for label, start, finish in [('before', -10, 0), ('release_0_8', 0, 8),
                                     ('release_8_12', 8, 12), ('recovery_12_25', 12, 25)]:
            intervals = [int(f['interval_ns']) / 1e6 for f in frames
                         if start <= (int(f['utc_ns']) - at) / 1e9 < finish]
            if intervals:
                ordered = sorted(intervals)
                windows[label] = {'frames': len(intervals), 'fps': 1000 / statistics.mean(intervals),
                                  'p95_ms': ordered[math.ceil(len(ordered) * .95) - 1],
                                  'p99_ms': ordered[math.ceil(len(ordered) * .99) - 1],
                                  'max_ms': max(intervals)}
        contexts = {}
        for label, begin, finish in [('before', first, seq), ('release', seq, end_seq),
                                     ('recovery', end_seq, end)]:
            rows = [r for r in records if begin <= r['sequence_context'] < finish]
            result = {'samples': len(rows)}
            for key in ('shaders', 'pipelines', 'stall_ms'):
                result[key] = sum(r.get(key, 0) for r in rows)
            for key in ('gpu_busy_pct', 'draws', 'dispatches'):
                values = [r[key] for r in rows if key in r]
                if values:
                    result[key + '_range'] = [min(values), max(values)]
            for key in ('shader_params', 'selection', 'texture', 'specialization', 'render_target'):
                hits = sum(r.get(key, [0, 0])[0] for r in rows)
                misses = sum(r.get(key, [0, 0])[1] for r in rows)
                if hits + misses:
                    result[key + '_hit_pct'] = 100 * hits / (hits + misses)
            contexts[label] = result
        results.append({'trial': trial.name, 'button_sequence': seq, 'windows': windows,
                        'log_contexts': contexts,
                        'warm_release': contexts['release']['samples'] > 0 and all(
                            contexts['release'][k] == 0 for k in ('shaders', 'pipelines', 'stall_ms'))})
    return {'session': str(session), 'timing_source': 'native applied_unix_ns + frame utc_ns',
            'log_attribution': 'last logged command context; not exact per-frame joins',
            'trials': results}


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('session', type=Path)
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    result = summarize(args.session)
    text = json.dumps(result, ensure_ascii=False, indent=2)
    if args.output:
        args.output.write_text(text + '\n', encoding='utf-8')
    print(text)
