# SPDX-License-Identifier: GPL-2.0-or-later
"""Read-only per-release correlation; CPU timeline waits are not pure GPU durations."""
import argparse
import bisect
from collections import Counter, defaultdict
import csv
import json
from pathlib import Path
from analyze_timing import Event, read_path, flip_path, stats

READ_PATH_EVENTS = {
    'read_request', 'read_cp_begin', 'read_cp_end', 'read_completion_request',
    'read_dependencies', 'read_copy', 'read_wait_begin', 'read_wait_end',
    'read_fallback_begin', 'read_fallback_end', 'read_return',
}
FLIP_PATH_EVENTS = {
    'flip_request', 'flip_prepare', 'flip_ready', 'flip_consume',
    'flip_present_return', 'flip_done',
}


def graphics_waits(events, copy_records, submitted, request):
    """Join graphics waits to their own copy and successful driver submission."""
    ends = {(e.a, e.b, e.c): e for e in events if e.kind == 'read_wait_end'}
    copies = [e for e in events if e.kind == 'read_copy']
    dependencies = [e for e in events if e.kind == 'read_dependencies']
    for start in events:
        if start.kind != 'read_wait_begin' or start.b != 0:
            continue
        end = ends[start.a, start.b, start.c]
        submit = submitted.get((0, start.c))
        row = {'round': start.a, 'total_ns': end.ns - start.ns, 'origin': 'unknown'}
        if submit is not None:
            boundary = min(end.ns, max(start.ns, submit))
            row.update(before_submit_ns=boundary - start.ns, after_submit_ns=end.ns - boundary,
                       completed_before_driver_return=end.ns < submit)
        matching = [c for c in copies if c.a == start.c and c.b == 0 and c.ns <= start.ns]
        if start.a == 0 and len(matching) == 1 and matching[0].d:
            copy = matching[0]
            records = copy_records.get(copy.d, ())
            if len(records) == 1 and records[0].c == copy.c and records[0].ns <= copy.ns:
                if records[0].ns < request.ns:
                    row['origin'] = 'reused'
                else:
                    deps = [d for d in dependencies if d.c == copy.c
                            and records[0].ns <= d.ns <= copy.ns]
                    if len(deps) == 1:
                        flags = deps[0].d
                        row['origin'] = ('new_done' if flags & 1 else 'new_submitted' if flags & 2
                                         else 'new_current' if flags & 4 else 'new_queued')
        yield row


def summarize_graphics_waits(rows):
    joined = [row for row in rows if 'before_submit_ns' in row]
    return {
        'n': len(joined),
        'before_submit_ms': sum(row['before_submit_ns'] for row in joined) / 1e6,
        'after_submit_ms': sum(row['after_submit_ns'] for row in joined) / 1e6,
        'unmatched_driver_return': len(rows) - len(joined),
        'completed_before_driver_return': sum(row['completed_before_driver_return']
                                              for row in joined),
        'origins': dict(Counter(row['origin'] for row in rows)),
        'first_wait_ms': stats(row['total_ns'] for row in rows if row['round'] == 0),
    }


def analyze(session, addresses=()):
    meta = json.loads((session / 'path-trace.json').read_text())
    if (not meta['complete'] or meta['dropped'] or meta['io_failed']
            or meta.get('cutoff_publishers')):
        raise ValueError('Incomplete/lost trace cannot establish a complete causal chain')
    frames = list(csv.DictReader((session / 'frames.csv').open(encoding='utf-8-sig')))
    read_groups, flip_groups, copy_records = defaultdict(list), defaultdict(list), defaultdict(list)
    samples, queued, submitted = {}, {}, {}
    counts = Counter()
    with (session / 'path-trace.csv').open() as f:
        for r in csv.DictReader(f):
            e = Event(int(r['ns']), int(r['thread']), r['event'],
                      *(int(r[k]) for k in ('a', 'b', 'c', 'd')))
            counts[e.kind] += 1
            # Prefetch batch IDs can overlap handler IDs. Group by the event's protocol.
            if e.kind in READ_PATH_EVENTS:
                read_groups[int(r['id'])].append(e)
            elif e.kind in FLIP_PATH_EVENTS:
                flip_groups[int(r['id'])].append(e)
            elif e.kind == 'copy_record':
                copy_records[int(r['id'])].append(e)
            elif e.kind == 'frame_sample':
                samples[e.a] = e.b
            elif e.kind == 'submit_queued':
                queued[e.a, e.b] = e.ns
            elif e.kind == 'submit_driver_end' and e.b != 0 and e.c == 0:
                submitted[e.a, e.b] = e.ns
    if sum(counts.values()) != meta['events']:
        raise ValueError('Metadata event count mismatch')
    stamps = [int(f['utc_ns']) for f in frames]
    cycles = json.loads((session / 'super-rounds.json').read_text(encoding='utf-8-sig'))
    result = {'session': str(session), 'trace_metadata': meta, 'event_counts': dict(counts),
              'limits': ['Per-handler sums overlap between threads; do not treat them as wall time.',
                         'Semaphore wait after submit includes GPU and CPU scheduling.',
                         'Submit driver return is a CPU observation, not GPU execution start.',
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
            target_waits = {address: [] for address in addresses}
            for ident, events in read_groups.items():
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
                    request_waits = list(graphics_waits(events, copy_records, submitted, start))
                    waits.extend(request_waits)
                    if start.a in target_waits:
                        target_waits[start.a].extend(request_waits)
            for events in flip_groups.values():
                if not all(lo <= e.ns < hi for e in events):
                    continue
                if {'flip_request', 'flip_done'} <= {e.kind for e in events}:
                    for k, v in flip_path(events).items():
                        flips[k].append(v)
            submit_delay = [submitted[key]-q for key, q in queued.items()
                            if lo <= q < hi and key in submitted and submitted[key] >= q]
            cycle['phases'][name] = {
                'read_stages': {k: {**stats(v), 'sum_ms_not_wall': sum(v)/1e6} for k,v in metrics.items()},
                'flip_stages': {k: stats(v) for k,v in flips.items()},
                'producer_states': dict(producers), 'queue_to_submit': stats(submit_delay),
                'graphics_copy_wait': summarize_graphics_waits(waits),
                'graphics_copy_wait_by_address': {
                    hex(address): summarize_graphics_waits(rows)
                    for address, rows in target_waits.items()},
                'largest_reads': sorted(largest, key=lambda x: -x['ms'])[:8]}
        result['cycles'].append(cycle)
    return result


if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('session', type=Path)
    p.add_argument('--output', type=Path, required=True)
    p.add_argument('--address', type=lambda value: int(value, 16), action='append', default=[],
                   help='Hex guest read address to report separately. May be repeated.')
    args = p.parse_args()
    data = analyze(args.session, args.address)
    args.output.write_text(json.dumps(data, indent=2) + '\n')
    for c in data['cycles']:
        print('cycle', c['cycle'], 'warm', c['warm'])
        for name, phase in c['phases'].items():
            print(name, 'producer', phase['producer_states'], 'submit', phase['queue_to_submit'],
                  'reads', {k: round(v['sum_ms_not_wall'], 2) for k,v in phase['read_stages'].items()},
                  'flip', phase['flip_stages'])
