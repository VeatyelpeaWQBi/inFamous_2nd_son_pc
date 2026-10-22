# SPDX-License-Identifier: GPL-2.0-or-later
"""Offline critical-path analysis. Standard library only; never launch a game or profiler."""
import argparse
from collections import Counter, defaultdict
import csv
from dataclasses import dataclass
import json
import math
from pathlib import Path

KINDS = set("""read_request read_cp_begin read_cp_end read_completion_request read_dependencies
read_copy read_wait_begin read_wait_end read_fallback_begin read_fallback_end read_return
flip_request flip_prepare flip_ready flip_consume flip_present_return flip_done copy_record
copy_publish_begin copy_publish_end gpu_dispatch_result frame_sample tuning_reload""".split())
FIELDS = ['thread', 'seq', 'ns', 'guest_frame', 'event', 'id', 'a', 'b', 'c', 'd']


def stats(values):
    """Nearest-rank percentiles; never average FPS or add nested/cross-thread timers."""
    values = sorted(values)
    if not values:
        return {'count': 0}
    return {'count': len(values), 'mean_ms': sum(values) / len(values) / 1e6,
            'p50_ms': values[math.ceil(len(values) * .50) - 1] / 1e6,
            'p95_ms': values[math.ceil(len(values) * .95) - 1] / 1e6,
            'p99_ms': values[math.ceil(len(values) * .99) - 1] / 1e6,
            'max_ms': values[-1] / 1e6}


@dataclass(slots=True)
class Event:
    ns: int
    thread: int
    kind: str
    a: int = 0
    b: int = 0
    c: int = 0
    d: int = 0


def one(events, kind):
    found = [e for e in events if e.kind == kind]
    if len(found) != 1:
        raise ValueError(f'expected one {kind}, got {len(found)}')
    return found[0]


def ordered(a, b):
    if b.ns < a.ns:
        raise ValueError('negative CPU interval')
    return b.ns - a.ns


def read_path(events):
    start, end = one(events, 'read_request'), one(events, 'read_return')
    total = ordered(start, end)
    if start.thread != end.thread:
        raise ValueError('read return changed caller thread')
    cp, seen_cp = {}, set()
    initial = [e for e in events if e.kind == 'read_cp_begin' and e.a != 1]
    if len(initial) != 1 or initial[0].a != {0: 2, 1: 0, 2: 3}.get(end.a):
        raise ValueError('initial command-processor phase disagrees with read mode')
    intervals = []
    result = Counter(total_ns=total)
    for e in sorted(events, key=lambda e: e.ns):
        if not start.ns <= e.ns <= end.ns:
            raise ValueError('read event outside handler lifetime')
        if e.kind == 'read_cp_begin':
            key = (e.a, e.b)
            if key in seen_cp:
                raise ValueError('duplicate command-processor phase')
            seen_cp.add(key)
            cp[key] = e
            if e.a == 1:
                requests = [r for r in events if r.kind == 'read_completion_request' and r.a == e.b]
                if len(requests) != 1:
                    raise ValueError('completion request missing or repeated')
                request = requests[0]
                label = 'completion_queue_ns'
            else:
                request = start
                label = 'initial_queue_ns'
            if request.thread != start.thread:
                raise ValueError('completion request changed caller thread')
            result[label] += ordered(request, e)
            intervals.append((request.ns, e.ns))
        elif e.kind == 'read_cp_end':
            begin = cp.pop((e.a, e.b), None)
            if begin is None or begin.thread != e.thread:
                raise ValueError('unpaired command-processor phase')
            label = 'completion_cp_ns' if e.a == 1 else 'initial_cp_ns'
            result[label] += ordered(begin, e)
            intervals.append((begin.ns, e.ns))
    if cp:
        raise ValueError('unfinished command-processor phase')
    waits = {}
    for e in sorted(events, key=lambda e: e.ns):
        key = (e.a, e.b, e.c)  # round, semaphore domain, timeline tick
        if e.kind == 'read_wait_begin':
            if key in waits or e.thread != start.thread:
                raise ValueError('duplicate wait or wrong waiting thread')
            waits[key] = e
        elif e.kind == 'read_wait_end':
            begin = waits.pop(key, None)
            if begin is None or e.thread != start.thread:
                raise ValueError('unpaired semaphore wait')
            result['cpu_wait_ns'] += ordered(begin, e)
            intervals.append((begin.ns, e.ns))
    if waits:
        raise ValueError('unfinished semaphore wait')
    if not any(e.kind == 'read_cp_begin' for e in events):
        raise ValueError('missing initial command-processor phase')
    # These intervals partition ONE handler. Publication and fallback are nested in the CP
    # bodies and are deliberately excluded from this sum; other callers overlap in wall time.
    previous = start.ns
    for lo, hi in sorted(intervals):
        if lo < previous:
            raise ValueError('overlapping primary stages within one read request')
        previous = hi
    covered = sum(hi - lo for lo, hi in intervals)
    if covered > total:
        raise ValueError('primary stages exceed handler lifetime')
    result['handoff_and_unattributed_ns'] = total - covered
    fallback = {}
    for e in sorted(events, key=lambda e: e.ns):
        if e.kind == 'read_fallback_begin':
            if e.a in fallback:
                raise ValueError('repeated fallback')
            fallback[e.a] = e
        elif e.kind == 'read_fallback_end':
            begin = fallback.pop(e.a, None)
            if begin is None or begin.thread != e.thread:
                raise ValueError('unpaired synchronous fallback')
            result['nested_fallback'] += ordered(begin, e)
    if fallback:
        raise ValueError('unfinished synchronous fallback')
    result['mode'] = end.a
    result['retry'] = end.b
    result['nothing'] = end.c
    result['site'] = start.c
    result['copies'] = sorted({e.d for e in events if e.kind == 'read_copy' and e.d})
    return dict(result)


def flip_path(events):
    names = ('flip_request', 'flip_prepare', 'flip_ready', 'flip_consume',
             'flip_present_return', 'flip_done')
    points = [one(events, name) for name in names]
    labels = ('cp_handoff_ns', 'prepare_cpu_ns', 'ready_queue_ns',
              'present_call_cpu_ns', 'status_events_cpu_ns')
    result = {name: ordered(a, b) for name, a, b in zip(labels, points, points[1:])}
    result['total_ns'] = ordered(points[0], points[-1])
    return result


def frame_summary(session, start, duration):
    intervals, rows = [], {}
    with (session / 'frames.csv').open(encoding='utf-8-sig', newline='') as f:
        previous = -1
        previous_elapsed = 0
        for row in csv.DictReader(f):
            index, elapsed, interval = (int(row[k]) for k in ('frame', 'elapsed_ns', 'interval_ns'))
            if index != previous + 1 or elapsed < previous_elapsed or interval < 0:
                raise ValueError('frames.csv index/order/interval is invalid')
            if index and elapsed - previous_elapsed != interval:
                raise ValueError('frames.csv elapsed and interval disagree')
            previous, previous_elapsed = index, elapsed
            if elapsed >= start * 1e9 and (duration is None or elapsed < (start + duration) * 1e9):
                rows[index] = (elapsed, interval)
                if interval:
                    intervals.append(interval)
    result = stats(intervals)
    if intervals:
        result['fps'] = 1e9 * len(intervals) / sum(intervals)
        result['elapsed_s'] = sum(intervals) / 1e9
    result['scene_verified'] = False
    result['interpretation'] = 'Guest flip cadence in the selected window, including any menus/loading. Not GPU render time.'
    return result, rows


def analyze(session, start=0, duration=None):
    launch = json.loads((session / 'launch.json').read_text(encoding='utf-8-sig'))
    profile = launch.get('timing_profile')
    if profile not in ('baseline', 'critical', 'lighting'):
        raise ValueError('session is not an explicit timing profile')
    result = {'schema': 1, 'session': session.name, 'profile': profile,
              'runtime_sha256': launch.get('runtime_sha256'), 'game_sha256': launch.get('game_sha256'),
              'window': {'start_s': start, 'duration_s': duration}, 'errors': [],
              'performance_acceptance': False,
              'limits': ['CPU wait includes queueing, GPU work and CPU scheduling; it is not pure GPU time.',
                         'Publication can be nested in CP time. Never add these totals or different threads.',
                         'Read-to-frame associations are temporal overlap only; guest_frame is not a causal ID.',
                         'GPU result timestamps are CPU query-retirement time. GPU durations have a separate clock.',
                         'A/B scenes, view, warmup and input must be verified before attributing FPS changes.']}
    result['frames'], frames = frame_summary(session, start, duration)
    if not frames:
        result['errors'].append('No frames in selected window')
    if profile == 'baseline':
        result['format_usable'] = bool(result['frames']['count'])
        if not result['format_usable'] and not result['errors']:
            result['errors'].append('No nonzero frame intervals in selected window')
        result['trace_expected'] = False
        return result
    meta = json.loads((session / 'path-trace.json').read_text(encoding='utf-8'))
    result['trace_metadata'] = meta
    if meta.get('schema') != 1 or meta.get('clock') != 'steady_clock_ns':
        result['errors'].append('Unsupported trace schema or clock')
    if not meta.get('complete') or any(meta.get(k, 0) for k in ('dropped', 'cutoff_publishers', 'io_failed')):
        result['errors'].append('Trace reports losses, incomplete shutdown or I/O failure')
    expected_profile, expected_hash = (2, 0xce1675e0) if profile == 'lighting' else (0, 0)
    if (meta.get('gpu_profile_at_start'), meta.get('compute_hash_at_start')) != (expected_profile, expected_hash):
        result['errors'].append('Actual startup profiling settings disagree with launch profile')
    # Pass 1 verifies the WHOLE CSV while building only the small exact frame join.
    frame_samples, sequences, counts, row_count = {}, {}, Counter(), 0
    with (session / 'path-trace.csv').open(encoding='utf-8', newline='') as f:
        reader = csv.DictReader(f)
        if reader.fieldnames != FIELDS:
            raise ValueError('Trace CSV header mismatch')
        for row in reader:
            if None in row or any(row.get(key) is None for key in FIELDS):
                raise ValueError('Trace CSV row width mismatch')
            nums = {k: int(row[k]) for k in FIELDS if k != 'event'}
            if (any(v < 0 for v in nums.values()) or nums['id'] == 0 or
                    nums['thread'] >= meta.get('registered_threads', 0) or row['event'] not in KINDS):
                raise ValueError('Invalid trace row')
            row_count += 1
            slot, seq, ns = (nums[k] for k in ('thread', 'seq', 'ns'))
            prior_seq, prior_ns = sequences.get(slot, (-1, 0))
            if seq != prior_seq + 1 or ns < prior_ns:
                counts['sequence_or_clock_errors'] += 1
            sequences[slot] = (seq, ns)
            counts[row['event']] += 1
            if row['event'] == 'frame_sample':
                index = nums['a']
                if index in frame_samples:
                    counts['duplicate_frame_samples'] += 1
                frame_samples[index] = (nums['b'], nums['d'], nums['c'])
    result['event_counts'] = dict(counts)
    result['configuration_stable'] = counts['tuning_reload'] == 0
    if row_count != meta.get('events') or counts['sequence_or_clock_errors'] or counts['duplicate_frame_samples']:
        result['errors'].append('Trace event count/sequence/clock integrity failed')
    # The tracer stops before process teardown; frames.csv may flush a few later flips.
    # Exclude only an untraced prefix/suffix. Any interior gap is an integrity failure.
    traced_first = min(frame_samples, default=0)
    traced_last = max(frame_samples, default=-1)
    boundary_frames = {i for i in frames if i < traced_first or i > traced_last}
    joined_frames = {i: row for i, row in frames.items() if i not in boundary_frames}
    missing_frames = set(joined_frames) - frame_samples.keys()
    mismatched = [i for i, (elapsed, interval) in frames.items()
                  if i in frame_samples and frame_samples[i][1:] != (elapsed, interval)]
    result['frame_join'] = {'selected': len(frames), 'traced': len(joined_frames),
                            'untraced_boundary': len(boundary_frames),
                            'missing_interior': len(missing_frames), 'mismatched': len(mismatched)}
    if missing_frames or mismatched:
        result['errors'].append('Exact frames.csv to trace join is incomplete or inconsistent')
    stamps = [frame_samples[i][0] for i in frames if i in frame_samples]
    if not stamps:
        result['errors'].append('No exact frame timestamps')
        result['format_usable'] = False
        return result
    lo, hi = min(stamps), max(stamps)
    # Pass 2 keeps only events in the selected interval, plus the preceding interval's start.
    # The final report contains only fully paired chains; boundary chains are counted separately.
    if len(stamps) > 1:
        lo -= frame_samples[min(i for i in frames if i in frame_samples)][2]
    groups = defaultdict(list)
    with (session / 'path-trace.csv').open(encoding='utf-8', newline='') as f:
        for row in csv.DictReader(f):
            ns = int(row['ns'])
            if lo <= ns <= hi:
                groups[int(row['id'])].append(Event(ns, int(row['thread']), row['event'],
                    *(int(row[k]) for k in ('a', 'b', 'c', 'd'))))
    read_metrics, flip_metrics, publications, gpu = defaultdict(list), defaultdict(list), [], defaultdict(list)
    valid_reads, invalid, partial, sites = [], Counter(), Counter(), defaultdict(list)
    copy_links = set()
    published = set()
    producer_states, copy_queues = Counter(), Counter()
    for ident, events in groups.items():
        kinds = {e.kind for e in events}
        try:
            if any(kind.startswith('read_') for kind in kinds):
                if not {'read_request', 'read_return'} <= kinds:
                    partial['read'] += 1
                    continue
                row = read_path(events)
                row['request_id'] = ident
                valid_reads.append(row)
                for event in events:
                    if event.kind == 'read_dependencies':
                        producer_states['done' if event.d & 1 else 'submitted' if event.d & 2 else 'current' if event.d & 4 else 'queued'] += 1
                    elif event.kind == 'read_copy':
                        copy_queues['readback' if event.b else 'graphics'] += 1
                sites[row['site']].append(row['total_ns'])
                copy_links.update(row['copies'])
                for key, value in row.items():
                    if key.endswith('_ns'):
                        read_metrics[key].append(value)
            elif kinds & {'flip_request', 'flip_done', 'flip_ready', 'flip_consume'}:
                if not {'flip_request', 'flip_done'} <= kinds:
                    partial['flip'] += 1
                    continue
                for key, value in flip_path(events).items():
                    flip_metrics[key].append(value)
            elif kinds & {'copy_publish_begin', 'copy_publish_end'}:
                if not {'copy_publish_begin', 'copy_publish_end'} <= kinds:
                    partial['publication'] += 1
                    continue
                begin, end = one(events, 'copy_publish_begin'), one(events, 'copy_publish_end')
                if begin.thread != end.thread or (begin.a, begin.b, begin.c) != (end.a, end.b, end.c):
                    raise ValueError('publication changed identity, queue or version')
                publications.append(ordered(begin, end))
                published.add(ident)
            elif kinds == {'gpu_dispatch_result'}:
                e = one(events, 'gpu_dispatch_result')
                gpu[(e.a, e.d)].append(e.c)
        except (ValueError, KeyError) as exc:
            invalid[str(exc)] += 1
    result['readback'] = {'complete_chains': len(valid_reads),
        'stages': {key: stats(value) for key, value in read_metrics.items()},
        'modes': dict(Counter(row['mode'] for row in valid_reads)),
        'retries': sum(bool(row['retry']) for row in valid_reads),
        'fallback_cpu_nested': stats([row.get('nested_fallback', 0) for row in valid_reads if row.get('nested_fallback')]),
        'nothing_to_wait_for': sum(bool(row['nothing']) for row in valid_reads),
        'hot_sites': [{'rip': hex(site), **stats(times), 'summed_handler_ms_not_wall_time': sum(times) / 1e6}
                      for site, times in sorted(sites.items(), key=lambda item: sum(item[1]), reverse=True)[:12]],
        'linked_copy_ids': len(copy_links), 'linked_publications_inside_window': len(copy_links & published),
        'producer_states_for_new_copies': dict(producer_states), 'request_copy_semaphore_domains': dict(copy_queues),
        'slowest_requests': sorted(valid_reads, key=lambda row: row['total_ns'], reverse=True)[:12]}
    result['flip'] = {key: stats(value) for key, value in flip_metrics.items()}
    result['publication_cpu'] = stats(publications)
    result['gpu_dispatch_intervals'] = [{'shader': f'{shader:016x}', 'scheduler': queue,
        **stats(values)} for (shader, queue), values in sorted(gpu.items())]
    result['partial_boundary_or_shutdown_chains'] = dict(partial)
    result['invalid_chains'] = dict(invalid)
    if invalid:
        result['errors'].append('Invalid complete correlation chains')
    if not valid_reads or not flip_metrics:
        result['errors'].append('No complete readback/flip chains; gameplay evidence insufficient')
    if profile == 'lighting' and not gpu:
        result['errors'].append('Targeted GPU timestamps absent; shader may not have run or profiler unavailable')
    result['format_usable'] = not result['errors']
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--session', type=Path, required=True)
    parser.add_argument('--start', type=float, help='Seconds from first flip; default: last 60 seconds')
    parser.add_argument('--duration', type=float, default=60)
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    if (args.start is not None and (not math.isfinite(args.start) or args.start < 0)) or (
            not math.isfinite(args.duration) or args.duration <= 0):
        parser.error('start must be finite/nonnegative and duration finite/positive')
    try:
        if args.start is None:
            last_elapsed = 0
            with (args.session / 'frames.csv').open(encoding='utf-8-sig', newline='') as stream:
                for row in csv.DictReader(stream):
                    last_elapsed = int(row['elapsed_ns'])
            args.start = max(0, last_elapsed / 1e9 - args.duration)
        result = analyze(args.session, args.start, args.duration)
    except (ValueError, OSError, KeyError, TypeError, csv.Error) as exc:
        result = {'schema': 1, 'format_usable': False, 'performance_acceptance': False, 'errors': [str(exc)]}
    target = args.output or args.session / 'timing-summary.json'
    try:
        target.write_text(json.dumps(result, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
    except OSError as exc:
        print(f'Cannot persist timing summary: {exc}')
        return 1
    print(f'Timing evidence: {"usable" if result["format_usable"] else "incomplete/invalid"}; {target.name}')
    for error in result.get('errors', []):
        print(f'  {error}')
    return 0 if result['format_usable'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
