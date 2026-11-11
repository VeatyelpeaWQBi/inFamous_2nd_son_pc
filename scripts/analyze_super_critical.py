# SPDX-License-Identifier: GPL-2.0-or-later
"""Read-only per-release correlation; CPU timeline waits are not pure GPU durations."""
import argparse
import bisect
from collections import Counter, defaultdict
import csv
import json
from pathlib import Path
from analyze_timing import Event, read_path, flip_path, stats


def analyze(session):
    meta = json.loads((session / 'path-trace.json').read_text())
    if not meta['complete'] or meta['dropped'] or meta['io_failed']:
        raise ValueError('Incomplete/lost trace cannot establish a complete causal chain')
    frames = list(csv.DictReader((session / 'frames.csv').open(encoding='utf-8-sig')))
    groups, samples, queued, submitted = defaultdict(list), {}, {}, {}
    counts = Counter()
    with (session / 'path-trace.csv').open() as f:
        for r in csv.DictReader(f):
            e = Event(int(r['ns']), int(r['thread']), r['event'],
                      *(int(r[k]) for k in ('a', 'b', 'c', 'd')))
            groups[int(r['id'])].append(e)
            counts[e.kind] += 1
            if e.kind == 'frame_sample':
                samples[e.a] = e.b
            elif e.kind == 'submit_queued':
                queued[e.a, e.b] = e.ns
            elif e.kind == 'submit_driver_end':
                submitted[e.a, e.b] = e.ns
    if sum(counts.values()) != meta['events']:
        raise ValueError('Metadata event count mismatch')
    stamps = [int(f['utc_ns']) for f in frames]
    cycles = json.loads((session / 'super-rounds.json').read_text(encoding='utf-8-sig'))
    result = {'session': str(session), 'trace_metadata': meta, 'event_counts': dict(counts),
              'limits': ['Per-handler sums overlap between threads; do not treat them as wall time.',
                         'Semaphore wait after submit includes GPU and CPU scheduling.',
                         'Read-to-flip overlap is temporal, not proof of a guest dependency.'],
              'cycles': []}
    for c in cycles:
        if not c.get('release'):
            continue
        utc = c['release']['applied_unix_ns']
        i = bisect.bisect_left(stamps, utc)
        anchor = next(f for f in frames[max(0, i-3):i+4] if int(f['frame']) in samples)
        release = samples[int(anchor['frame'])] + utc - int(anchor['utc_ns'])
        cycle = {'cycle': c['cycle'], 'warm': c['warm'], 'release_ns': release, 'phases': {}}
        for name, begin, end in [('baseline', -10, 0), ('early', 0, 3), ('middle', 3, 8),
                                 ('late', 8, 13), ('recover', 13, 23)]:
            lo, hi = release + int(begin*1e9), release + int(end*1e9)
            metrics, flips, producers = defaultdict(list), defaultdict(list), Counter()
            waits, largest = [], []
            for ident, events in groups.items():
                if not all(lo <= e.ns < hi for e in events):
                    continue
                kinds = {e.kind for e in events}
                if {'read_request', 'read_return'} <= kinds:
                    row = read_path(events)
                    for k, value in row.items():
                        if k.endswith('_ns'):
                            metrics[k].append(value)
                    start = next(e for e in events if e.kind == 'read_request')
                    largest.append({'id': ident, 'ms': row['total_ns']/1e6,
                                    'site': hex(row['site']), 'address': hex(start.a),
                                    'size': start.b, 'stages_ms':
                                    {k: v/1e6 for k, v in row.items() if k.endswith('_ns')}})
                    for e in events:
                        if e.kind == 'read_dependencies':
                            producers['done' if e.d & 1 else 'submitted' if e.d & 2
                                      else 'current' if e.d & 4 else 'queued'] += 1
                        if e.kind == 'read_wait_begin' and e.b == 0:
                            end_wait = next((x for x in events if x.kind == 'read_wait_end'
                                             and (x.a,x.b,x.c) == (e.a,e.b,e.c)), None)
                            submit = submitted.get((0, e.c))
                            if end_wait and submit is not None:
                                waits.append({'before_submit_ms': max(0, submit-e.ns)/1e6,
                                              'after_submit_ms': max(0, end_wait.ns-max(submit,e.ns))/1e6})
                elif {'flip_request', 'flip_done'} <= kinds:
                    for k, v in flip_path(events).items():
                        flips[k].append(v)
            submit_delay = [submitted[key]-q for key, q in queued.items()
                            if lo <= q < hi and key in submitted and submitted[key] >= q]
            cycle['phases'][name] = {
                'read_stages': {k: {**stats(v), 'sum_ms_not_wall': sum(v)/1e6} for k,v in metrics.items()},
                'flip_stages': {k: stats(v) for k,v in flips.items()},
                'producer_states': dict(producers), 'queue_to_submit': stats(submit_delay),
                'graphics_copy_wait': {'n': len(waits),
                    **{k: sum(w[k] for w in waits) for k in ('before_submit_ms','after_submit_ms')}},
                'largest_reads': sorted(largest, key=lambda x: -x['ms'])[:8]}
        result['cycles'].append(cycle)
    return result


if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('session', type=Path)
    p.add_argument('--output', type=Path, required=True)
    args = p.parse_args()
    data = analyze(args.session)
    args.output.write_text(json.dumps(data, indent=2) + '\n')
    for c in data['cycles']:
        print('cycle', c['cycle'], 'warm', c['warm'])
        for name, phase in c['phases'].items():
            print(name, 'producer', phase['producer_states'], 'submit', phase['queue_to_submit'],
                  'reads', {k: round(v['sum_ms_not_wall'], 2) for k,v in phase['read_stages'].items()},
                  'flip', phase['flip_stages'])
