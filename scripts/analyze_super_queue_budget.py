# SPDX-License-Identifier: GPL-2.0-or-later
"""Attribute sampled particle-label waits to ordered CP slices and dispatch CPU phases."""
import argparse
from bisect import bisect_left, bisect_right
from collections import defaultdict
import csv
import json
from itertools import islice
from pathlib import Path


def mean_ms(values):
    return sum(values) / len(values) / 1e6 if values else None


def analyze(session):
    meta = json.loads((session / 'path-trace.json').read_text(encoding='utf-8-sig'))
    if (not meta['complete'] or meta['dropped'] or meta['io_failed']
            or meta['cutoff_publishers']):
        raise ValueError('Incomplete trace')
    rounds = json.loads((session / 'super-rounds.json').read_text(encoding='utf-8-sig'))
    release, = (r['release'] for r in rounds if r.get('release'))
    clock = meta['start']
    origin = (clock['ns_before'] + clock['ns_after']) // 2
    origin += (release['qpc_after'] - clock['qpc']) * 10**9 // clock['frequency']
    names = ('cp_queue_slice', 'cp_asc_slice_work', 'cp_asc_dispatch_phases',
             'cp_asc_binding_phases', 'cp_asc_buffer_detail', 'particle_label_schedule')
    tables = {name: {} for name in names}
    writes = defaultdict(list)
    samples = []
    count = 0
    first_event = None
    last_event = None
    with (session / 'path-trace.csv').open(encoding='utf-8-sig', newline='') as stream:
        for row in csv.DictReader(stream):
            count += 1
            event_ns = int(row['ns'])
            first_event = event_ns if first_event is None else min(first_event, event_ns)
            last_event = event_ns if last_event is None else max(last_event, event_ns)
            kind = row['event']
            if kind not in tables and kind not in ('particle_label_write', 'flip_caller_sample'):
                continue
            stamp, ident, a, b, c, d = (int(row[k]) for k in ('ns', 'id', 'a', 'b', 'c', 'd'))
            if kind in tables:
                if ident in tables[kind]:
                    raise ValueError(f'Duplicate {kind} ID {ident}')
                tables[kind][ident] = (a, b, c, d)
            elif kind == 'particle_label_write':
                writes[a].append((stamp, ident, b))
            elif d:
                samples.append((stamp, d, a >> 32))
    if count != meta['events']:
        raise ValueError('Event count mismatch')
    slices = tables['cp_queue_slice']
    work = tables['cp_asc_slice_work']
    phases = tables['cp_asc_dispatch_phases']
    bindings = tables['cp_asc_binding_phases']
    detail = tables['cp_asc_buffer_detail']
    if not phases or phases.keys() != bindings.keys() or phases.keys() != work.keys():
        raise ValueError('Missing joined dispatch phase diagnostics')
    if detail and detail.keys() != phases.keys():
        raise ValueError('Missing joined buffer detail diagnostics')
    queue_slices = defaultdict(list)
    for ident, p in phases.items():
        start, end, queue, done = slices[ident]
        dispatch, release_time, calls, unused = work[ident]
        buffers, textures, raster_calls, unused = bindings[ident]
        if (queue == 0 or end < start or sum(p) > dispatch or dispatch + release_time > end - start
                or buffers + textures > p[1] or raster_calls > calls):
            raise ValueError(f'Invalid CPU phase nesting for slice {ident}')
        if detail and (sum(detail[ident][:3]) > detail[ident][3]
                       or detail[ident][3] > buffers or bindings[ident][3] > raster_calls):
            raise ValueError(f'Invalid sampled buffer nesting for slice {ident}')
        queue_slices[queue].append((start, end, ident))
    all_slices = sorted((v[0], v[1]) for v in slices.values())
    if any(b > c for (a, b), (c, d) in zip(all_slices, all_slices[1:])):
        raise ValueError('CP slices overlap')
    queue_starts = {}
    for queue, values in queue_slices.items():
        values.sort()
        queue_starts[queue] = [value[0] for value in values]
    write_stamps = {}
    for address, values in writes.items():
        values.sort()
        write_stamps[address] = [value[0] for value in values]

    pending = {}
    rejected = defaultdict(int)
    for stamp, address, threshold in sorted(samples):
        values = writes.get(address, ())
        offset = bisect_right(write_stamps.get(address, ()), stamp)
        if not offset or values[offset - 1][2] >= threshold:
            rejected['already_satisfied_or_unobserved_previous_value'] += 1
            continue
        matched = next((value for value in islice(values, offset, None)
                        if value[2] >= threshold), None)
        if not matched:
            rejected['no_qualifying_write'] += 1
            continue
        write_ns, write_id, value = matched
        schedule = tables['particle_label_schedule'].get(write_id)
        if schedule is None:
            rejected['no_producer_schedule'] += 1
            continue
        submit, resume, fence, vqid = schedule
        if not submit <= stamp < resume <= fence <= write_ns:
            rejected['not_queued_and_unstarted_at_sample'] += 1
            continue
        # SubmitAsc maps gnm_vqid to ProcessCompute(vqid=gnm_vqid-1).
        pending.setdefault((address, threshold, write_id), (stamp, resume, vqid + 1))

    windows = {}
    for lo, hi in ((-8, -1), (0, 3), (3, 8), (8, 12)):
        records = []
        for start, end, queue in pending.values():
            if not lo * 10**9 <= start - origin < hi * 10**9:
                continue
            values = queue_slices[queue]
            first = bisect_left(queue_starts[queue], start)
            last = bisect_left(queue_starts[queue], end)
            chosen = [v for v in values[first:last] if v[1] <= end]
            totals = defaultdict(int)
            totals['remaining_queue_wait'] = end - start
            for slice_start, slice_end, ident in chosen:
                totals['full_slice'] += slice_end - slice_start
                for key, value in zip(('prepare', 'resources', 'record', 'retire'), phases[ident]):
                    totals[key] += value
                for key, value in zip(('dispatch', 'release', 'dispatch_calls'), work[ident]):
                    totals[key] += value
                totals['buffers'] += bindings[ident][0]
                totals['textures'] += bindings[ident][1]
            records.append(totals)
        keys = ('remaining_queue_wait', 'full_slice', 'dispatch', 'release',
                'prepare', 'resources', 'record', 'retire', 'buffers', 'textures')
        windows[f'{lo}:{hi}'] = {
            'pending_sampled_conditions': len(records),
            'mean_ms': {key: mean_ms([r[key] for r in records]) for key in keys},
            'mean_dispatch_calls': (sum(r['dispatch_calls'] for r in records) / len(records)
                                    if records else None),
        }
    queue_work_windows = {}
    for lo, hi in ((-8, -1), (0, 3), (3, 8), (8, 12)):
        covered_seconds = max(0, min(last_event, origin + hi * 10**9)
                              - max(first_event, origin + lo * 10**9)) / 1e9
        selected = [ident for ident in phases
                    if origin + lo * 10**9 <= slices[ident][0]
                    and slices[ident][1] < origin + hi * 10**9]
        calls = sum(bindings[ident][2] for ident in selected)
        totals = {key: sum(phases[ident][slot] for ident in selected)
                  for slot, key in enumerate(('prepare', 'resources', 'record', 'retire'))}
        totals['buffers'] = sum(bindings[ident][0] for ident in selected)
        totals['textures'] = sum(bindings[ident][1] for ident in selected)
        sampled_calls = sum(bindings[ident][3] for ident in selected) if detail else 0
        queue_work_windows[f'{lo}:{hi}'] = {
            'slices': len(selected), 'raster_dispatch_calls': calls,
            'observed_window_seconds': covered_seconds,
            'cpu_ms_per_wall_second': {key: ns / covered_seconds / 1e6 if covered_seconds else None
                                       for key, ns in totals.items()},
            'mean_us_per_call': {key: ns / calls / 1e3 if calls else None
                                 for key, ns in totals.items()},
            'sampled_buffer_dispatch_calls': sampled_calls,
            'sampled_buffer_mean_us_per_call': {
                key: (sum(detail[ident][slot] for ident in selected) / sampled_calls / 1e3
                      if sampled_calls else None)
                for slot, key in enumerate(('obtain', 'image_invalidation', 'hazard_query', 'full_bind'))
            },
        }
    return {
        'session': session.name, 'events': count, 'dispatch_slices': len(phases),
        'joined_pending_conditions': len(pending), 'rejected_samples': dict(rejected),
        'sampled_wait_windows': windows, 'queue_work_windows': queue_work_windows,
        'limit': 'CPU wall time with diagnostic clock overhead; only full same-queue slices '
                 'inside a sampled wait. Waits may overlap each other, so their totals must '
                 'not be summed as a frame budget. queue_work_windows separately covers all '
                 'recorded ASC slices >=50 us, including those without a sampled wait. No '
                 'GPU busy time or FPS gain is implied.',
    }


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('session', type=Path)
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    text = json.dumps(analyze(args.session), ensure_ascii=False, indent=2) + '\n'
    if args.output:
        args.output.write_text(text, encoding='utf-8')
    else:
        print(text, end='')
