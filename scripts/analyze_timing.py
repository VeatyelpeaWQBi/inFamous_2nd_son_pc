# SPDX-License-Identifier: GPL-2.0-or-later
"""Offline critical-path analysis. Standard library only; never launch a game or profiler."""
import argparse
from collections import Counter, defaultdict
import csv
from dataclasses import dataclass
from datetime import datetime
import json
import math
import re
import struct
from pathlib import Path

KINDS = set("""read_request read_cp_begin read_cp_end read_completion_request read_dependencies
read_copy read_wait_begin read_wait_end read_fallback_begin read_fallback_end read_return
flip_request flip_prepare flip_ready flip_consume flip_present_return flip_done copy_record
copy_publish_begin copy_publish_end gpu_dispatch_result frame_sample tuning_reload host_phase_begin host_phase_end
read_producer read_producer_range read_producer_time read_producer_domain read_producer_coverage
read_producer_window submit_queued submit_driver_begin submit_driver_end feedback_fence""".split())
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
    crossing_start = 0
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
                if interval and elapsed - interval < start * 1e9:
                    crossing_start += 1
                    continue
                rows[index] = (elapsed, interval)
                if interval:
                    intervals.append(interval)
    result = stats(intervals)
    result['intervals_crossing_start_excluded'] = crossing_start
    if intervals:
        result['fps'] = 1e9 * len(intervals) / sum(intervals)
        result['elapsed_s'] = sum(intervals) / 1e9
    result['scene_verified'] = False
    result['interpretation'] = 'Guest flip cadence in the selected window, including any menus/loading. Not GPU render time.'
    return result, rows


HOST_PHASE_NAMES = ('acquire', 'present_flush', 'present_mutex', 'queue_present',
                    'submit_mutex', 'queue_submit', 'swapchain_recreate')


def host_phase_path(events):
    begin, end = one(events, 'host_phase_begin'), one(events, 'host_phase_end')
    if begin.thread != end.thread or begin.a != end.a or not 0 <= begin.a < len(HOST_PHASE_NAMES):
        raise ValueError('host phase identity changed')
    return HOST_PHASE_NAMES[begin.a], ordered(begin, end)


U64_MAX = (1 << 64) - 1
PRODUCER_KINDS = {'read_producer', 'read_producer_range', 'read_producer_time',
                  'read_producer_domain'}


def valid_range(address, size):
    return 0 <= address <= U64_MAX and 0 < size <= U64_MAX - address


def overlaps(a, size, b, span):
    return a < b + span and b < a + size


def producer_path(events):
    """Latest DECLARED overlap per queue/range, never proof of actual shader stores."""
    start = one(events, 'read_request')
    coverage = one(events, 'read_producer_coverage')
    window = one(events, 'read_producer_window')
    cp_begin = one([e for e in events if e.kind == 'read_cp_begin' and e.a == 0], 'read_cp_begin')
    cp_end = one([e for e in events if e.kind == 'read_cp_end' and e.a == 0], 'read_cp_end')
    for e in events:
        if e.kind.startswith('read_producer') and (e.thread != cp_begin.thread or
                not cp_begin.ns <= e.ns <= cp_end.ns):
            raise ValueError('producer metadata outside initial command-processor phase')
    if (not valid_range(start.a, start.b) or not valid_range(window.a, window.b) or
            (window.c, window.d) != (start.a, start.b) or
            not window.a <= start.a < start.a + start.b <= window.a + window.b):
        raise ValueError('producer window does not cover the requested bytes')
    if coverage.a > coverage.b or coverage.d not in (4, 5):
        raise ValueError('invalid declared-producer coverage metadata')
    by_sequence = defaultdict(list)
    for e in events:
        if e.kind in PRODUCER_KINDS:
            by_sequence[e.a].append(e)
    if len(by_sequence) > 4:
        raise ValueError('producer history emitted more than four candidates')
    candidates, occupied = [], set()
    for sequence, items in by_sequence.items():
        p, r, t, d = (one(items, kind) for kind in
                      ('read_producer', 'read_producer_range', 'read_producer_time', 'read_producer_domain'))
        queue, command_kind, scopes = p.d & 255, (p.d >> 8) & 255, p.d >> 16
        if (not coverage.a <= sequence <= coverage.b or not sequence or
                queue > 1 or command_kind > 4 or scopes not in (1, 2, 3) or
                t.d != p.b or bool(command_kind) != bool(p.b) or
                not valid_range(r.b, r.c) or not r.d or
                t.b > t.ns or (t.c and (t.c < t.b or t.c > t.ns))):
            raise ValueError('invalid declared-producer identity/range/time')
        if scopes & 1 and not overlaps(r.b, r.c, start.a, start.b):
            raise ValueError('producer request-range candidate does not overlap')
        if scopes & 2 and not overlaps(r.b, r.c, window.a, window.b):
            raise ValueError('producer window-range candidate does not overlap')
        for scope in (1, 2):
            if scopes & scope:
                if (queue, scope) in occupied:
                    raise ValueError('duplicate producer for queue/range')
                occupied.add((queue, scope))
        candidates.append({'sequence': sequence, 'command': p.b, 'shader': p.c,
            'kind': command_kind, 'queue': queue, 'scopes': scopes, 'tick': r.d,
            'domain': d.b, 'submitted_tick_snapshot': d.c, 'known_gpu_tick_snapshot': d.d,
            'declared_ns': t.b, 'recorded_ns': t.c, 'observed_ns': d.ns,
            'address': r.b, 'size': r.c})
    return {'oldest_sequence': coverage.a, 'write_sequence': coverage.b,
            'overwritten': coverage.c, 'blind_spot': bool(coverage.d & 1),
            'candidates': candidates, 'site': start.c}


def submission_paths(groups):
    """Separate CPU enqueue, driver-call start and CPU return. None is GPU completion."""
    by_key, partial, owners = defaultdict(list), Counter(), {}
    for ident, events in groups.items():
        for e in events:
            if e.kind in ('submit_queued', 'submit_driver_begin', 'submit_driver_end'):
                if not e.a or not e.b:
                    raise ValueError('zero submit semaphore domain/tick')
                by_key[e.a, e.b].append(e)
                owners[e.a, e.b, e.kind] = ident
    result = {}
    for key, events in by_key.items():
        names = Counter(e.kind for e in events)
        if max(names.values()) > 1:
            raise ValueError('duplicate submit stage for semaphore domain/tick')
        row = {e.kind: e for e in events}
        begin, end, queued = (row.get(name) for name in
                             ('submit_driver_begin', 'submit_driver_end', 'submit_queued'))
        if begin and end:
            if (begin.thread != end.thread or begin.c != end.c or
                    owners[key[0], key[1], 'submit_driver_begin'] != owners[key[0], key[1], 'submit_driver_end']):
                raise ValueError('submit driver identity/command count changed')
            ordered(begin, end)
        else:
            partial['driver_pair'] += 1
        if end and end.d != 1:
            raise ValueError('Vulkan submission reported failure')
        if queued and queued.d not in (0, 1):
            raise ValueError('invalid submit queue mode')
        if queued and begin:
            ordered(queued, begin)
        else:
            partial['enqueue_pair'] += 1
        result[key] = row
    return result, dict(partial)


DESCRIPTOR_FIELDS = ('calls hit unknown uncacheable layout shape payload layout_only changed_segments '
                     'comparable_segments changed_infos one few many buffer offset range sampler view '
                     'image_layout texel_view').split()
DESCRIPTOR_PATTERN = re.compile(
    r'Perf R4 descriptors: calls (\d+) hit (\d+) unknown (\d+) uncacheable (\d+) layout (\d+) '
    r'shape (\d+) payload (\d+) layout-only (\d+); changed segments (\d+)/(\d+) infos (\d+) '
    r'one/few/many (\d+)/(\d+)/(\d+); fields buffer (\d+) offset (\d+) range (\d+) sampler (\d+) '
    r'view (\d+) image-layout (\d+) texel-view (\d+)\s*$')
PREPARE_PATTERN = re.compile(
    r'Perf R4 prepare: calls (\d+) ms ([\d.]+); buffers (\d+) ms ([\d.]+); '
    r'textures (\d+) ms ([\d.]+); regular sets (\d+) writes (\d+)\s*$')

A1_FIELDS = 'calls full partial skipped input emitted omitted partial_omitted'.split()
A1_PATTERN = re.compile(
    r'Perf A1 descriptors: calls (\d+) full (\d+) partial (\d+) skipped (\d+); '
    r'write segments input (\d+) emitted (\d+) omitted (\d+) partial-omitted (\d+)\s*$')


def descriptor_update_line(line):
    match = A1_PATTERN.search(line)
    if not match:
        raise ValueError('malformed A1 descriptor counters')
    return dict(zip(A1_FIELDS, map(int, match.groups())))


def descriptor_update_invariants(row):
    return {'calls_minus_modes': row['calls'] - row['full'] - row['partial'] - row['skipped'],
            'input_minus_emitted_and_omitted': row['input'] - row['emitted'] - row['omitted'],
            'partial_omitted_excess': max(0, row['partial_omitted'] - row['omitted'])}


def descriptor_line(line):
    match = DESCRIPTOR_PATTERN.search(line)
    if not match:
        raise ValueError('malformed R4 descriptor counters')
    row = dict(zip(DESCRIPTOR_FIELDS, map(int, match.groups())))
    return row


def descriptor_invariants(row):
    # Perf::OnFlip reads each counter separately while CP continues updating it. An interval
    # need not conserve these related counters. Keep the skew visible; do not call it lost data.
    return {'calls_minus_reasons': row['calls'] - sum(row[k] for k in
                ('hit', 'unknown', 'uncacheable', 'layout', 'shape', 'payload')),
            'layout_only_excess': max(0, row['layout_only'] - row['layout']),
            'changed_segments_excess': max(0, row['changed_segments'] - row['comparable_segments']),
            'changed_infos_deficit': max(0, row['changed_segments'] - row['changed_infos']),
            'distribution_excess': max(0, sum(row[k] for k in ('one', 'few', 'many')) -
                                       row['layout'] - row['payload'])}


def depth_spirv_facts(data):
    """Count final binary operations; this does not replace SPIR-V validation."""
    if len(data) < 20 or len(data) % 4:
        raise ValueError('C1 dump has invalid word length')
    words = struct.unpack('<' + 'I' * (len(data) // 4), data)
    if words[0] != 0x07230203:
        raise ValueError('C1 dump has invalid SPIR-V magic')
    counts, arithmetic, at = Counter(), False, 5
    names = {353: 'subgroup_signed_min', 356: 'subgroup_signed_max', 333: 'subgroup_elect',
             236: 'shared_signed_min_atomic', 238: 'shared_signed_max_atomic', 224: 'control_barrier'}
    while at < len(words):
        count, op = words[at] >> 16, words[at] & 65535
        if not count or count > len(words) - at:
            raise ValueError('C1 dump has truncated instruction')
        if op in names:
            counts[names[op]] += 1
        if op == 17 and count == 2 and words[at + 1] == 63:
            arithmetic = True
        at += count
    observed = arithmetic and all(counts[k] == 1 for k in
        ('subgroup_signed_min', 'subgroup_signed_max', 'subgroup_elect',
         'shared_signed_min_atomic', 'shared_signed_max_atomic'))
    return {'arithmetic_capability': arithmetic, 'operations': dict(counts),
            'candidate_pattern_observed': observed}


def depth_reduction_summary(session):
    startup, compiles, binaries, issues = [], [], [], []
    start = re.compile(r'C1 depth reduction: requested=(true|false) supported=(true|false) active=(true|false)')
    compiled = re.compile(r'C1 depth reduction shader 0xce1675e0 permutation (\d+): applied=(true|false) reason=([a-z0-9-]+)')
    try:
        with (session / 'log' / 'shad_log.txt').open(encoding='utf-8-sig', errors='replace') as stream:
            for line in stream:
                match = start.search(line)
                if match:
                    startup.append(dict(zip(('requested', 'supported', 'active'), (v == 'true' for v in match.groups()))))
                match = compiled.search(line)
                if match:
                    compiles.append({'permutation': int(match[1]), 'applied': match[2] == 'true', 'reason': match[3]})
    except OSError as exc:
        issues.append(str(exc))
    for path in sorted((session / 'user' / 'shader' / 'dumps').glob('cs_0x00000000ce1675e0_*.spv')):
        try:
            binaries.append({'file': path.name, **depth_spirv_facts(path.read_bytes())})
        except (OSError, ValueError) as exc:
            issues.append(path.name + ': ' + str(exc))
    observed = any(row['candidate_pattern_observed'] for row in binaries)
    return {'startup': startup, 'compilations': compiles, 'final_binaries': binaries,
            'mechanism_status': 'observed-final-binary' if observed else
                'reported-compiled' if any(r['applied'] for r in compiles) else
                'fallback' if compiles else 'not-observed',
            'issues': issues,
            'limits': ['Final binary construction is not proof of dispatch, image correctness or FPS benefit.',
                       'IR dumps precede this C1 transform; inspect final SPIR-V for its operations.',
                       'Compilation logs can be absent when a shader is loaded from cache.',
                       'Unsupported/unmatched candidates fall back; this does not corrupt collected performance data.']}


def descriptor_summary(session, require_updates=False):
    # Reports are one-second counter deltas, possibly cutting across the selected trace window.
    # Keep their process-wide scope explicit; never normalize by selected-window frame count.
    log = session / 'log' / 'shad_log.txt'
    totals, prepare, reports, prepare_reports = Counter(), Counter(), 0, 0
    snapshot_skews = []
    updates, update_reports, update_skews = Counter(), 0, []
    with log.open(encoding='utf-8-sig', errors='replace') as stream:
        for line in stream:
            if 'Perf R4 descriptors:' in line:
                row = descriptor_line(line)
                deviations = descriptor_invariants(row)
                if any(deviations.values()):
                    snapshot_skews.append({'report': reports + 1, **deviations})
                totals.update(row)
                reports += 1
            elif 'Perf A1 descriptors:' in line:
                row = descriptor_update_line(line)
                deviations = descriptor_update_invariants(row)
                if any(deviations.values()):
                    update_skews.append({'report': update_reports + 1, **deviations})
                updates.update(row)
                update_reports += 1
            elif 'Perf R4 prepare:' in line:
                match = PREPARE_PATTERN.search(line)
                if not match:
                    raise ValueError('malformed R4 preparation counters')
                values = match.groups()
                for i, key in enumerate(('calls', 'prepare_ms', 'buffers', 'buffers_ms',
                                         'textures', 'textures_ms', 'regular_sets', 'regular_writes')):
                    value = float(values[i]) if key.endswith('_ms') else int(values[i])
                    if value < 0 or not math.isfinite(value):
                        raise ValueError('invalid R4 preparation counter')
                    prepare[key] += value
                prepare_reports += 1
    if not reports or not prepare_reports or not totals['calls']:
        raise ValueError('R4 descriptor/preparation reports absent')
    if require_updates and (not update_reports or not updates['calls']):
        raise ValueError('A1 descriptor update reports absent')
    update_summary = None
    if update_reports:
        residuals = descriptor_update_invariants(updates)
        update_summary = {'reports': update_reports, 'counts': dict(updates),
                          'snapshot_skew_intervals': update_skews,
                          'aggregate_invariants': residuals,
                          'aggregate_invariants_met': not any(residuals.values()),
                          'limits': ['Omitted includes existing whole-call skips; partial_omitted isolates A1 subset omissions.',
                                     'Counts measure write segments, not saved CPU time or normal FPS.',
                                     'Independent atomic snapshots can have residuals; do not infer transport loss.']}
    return {'scope': 'All completed process log reporting intervals; NOT selected trace window.',
            'reports': reports, 'prepare_reports': prepare_reports,
            'snapshot_skew_intervals': snapshot_skews,
            'aggregate_invariants': descriptor_invariants(totals),
            'aggregate_invariants_met': not any(descriptor_invariants(totals).values()),
            'descriptors': dict(totals), 'preparation': dict(prepare), 'updates': update_summary,
            'limits': ['Segments are descriptor write segments, not unique binding numbers.',
                       'Field-change counts can overlap; do not add them as miss counts.',
                       'Prepare contains buffer/texture timers; do not sum nested times.',
                       'Hit means identical tracked snapshot, not necessarily a skipped API call.',
                       'Counters are sampled independently across threads; interval/aggregate residuals are reported, not proof of transport loss.']}


def feedback_summary(groups, valid_reads):
    submissions, partial = submission_paths(groups)
    counts, intervals, shaders = Counter(), defaultdict(list), Counter()
    candidate_sites = defaultdict(list)
    for row in submissions.values():
        begin, end, queued = (row.get(name) for name in
                             ('submit_driver_begin', 'submit_driver_end', 'submit_queued'))
        if begin and end:
            intervals['driver_call_cpu_ns'].append(ordered(begin, end))
        if queued and begin:
            intervals['enqueue_to_driver_cpu_ns'].append(ordered(queued, begin))
    for read in valid_reads:
        events = groups[read['request_id']]
        has_evidence = any(e.kind.startswith('read_producer') for e in events)
        if not has_evidence:
            if read['mode'] == 1 and not (one(events, 'read_request').d & 1):
                raise ValueError('R4 asynchronous read has no producer coverage record')
            counts['reads_without_producer_evidence'] += 1
            continue
        evidence = producer_path(events)
        counts['covered_reads'] += 1
        counts['history_evicted_reads'] += bool(evidence['overwritten'])
        counts['blind_spot_reads'] += evidence['blind_spot']
        counts['reads_without_retained_overlap'] += not evidence['candidates']
        dependency = next((e for e in events if e.kind == 'read_dependencies'), None)
        for p in evidence['candidates']:
            counts['declared_candidates'] += 1
            if dependency:
                effective_tick = (dependency.a, dependency.b)[p['queue']]
                counts['matches_effective_dependency_tick' if p['tick'] == effective_tick else
                       'does_not_match_effective_dependency_tick'] += 1
            else:
                counts['no_new_copy_dependency_snapshot'] += 1
            counts['uncommitted_or_unknown_command'] += not bool(p['recorded_ns'] and p['command'])
            counts['request_range_candidates'] += bool(p['scopes'] & 1)
            counts['window_only_candidates'] += p['scopes'] == 2
            shaders[(p['shader'], p['kind'], p['queue'])] += 1
            candidate_sites[evidence['site'], p['shader'], p['kind'], p['queue'], p['scopes']].append(
                (read['total_ns'], read.get('cpu_wait_ns', 0)))
            counts['unknown_domain_snapshot' if not p['domain'] else
                   'known_done_snapshot' if p['tick'] <= p['known_gpu_tick_snapshot'] else
                   'submitted_snapshot' if p['tick'] <= p['submitted_tick_snapshot'] else
                   'beyond_submitted_snapshot'] += 1
            submit = submissions.get((p['domain'], p['tick']), {})
            queued = submit.get('submit_queued')
            if queued:
                if queued.ns < p['declared_ns']:
                    raise ValueError('producer declaration occurred after its submission enqueue')
                counts['linked_enqueue'] += 1
                if {'submit_driver_begin', 'submit_driver_end'} <= submit.keys():
                    counts['linked_complete_submit'] += 1
                    if p['scopes'] & 1 and p['recorded_ns'] and p['kind'] in (3, 4):
                        counts['request_compute_candidates_with_complete_submit'] += 1
                intervals['declared_to_enqueue_cpu_ns'].append(queued.ns - p['declared_ns'])
                if p['recorded_ns'] and p['recorded_ns'] <= queued.ns:
                    intervals['recorded_to_enqueue_cpu_ns'].append(queued.ns - p['recorded_ns'])
                elif p['recorded_ns']:
                    counts['cpu_command_return_after_enqueue'] += 1
            else:
                counts['enqueue_outside_window_or_unavailable'] += 1
    fences = sum(e.kind == 'feedback_fence' for events in groups.values() for e in events)
    return {'counts': dict(counts),
            'has_request_compute_submit_evidence': bool(counts['request_compute_candidates_with_complete_submit']),
            'safe_to_remove_dependencies_from_this_trace_alone': False,
            'submission_domains': len({key[0] for key in submissions}),
            'submission_ticks': len(submissions), 'partial_submission_chains': partial,
            'feedback_fence_markers': fences,
            'cpu_intervals_not_additive': {key: stats(values) for key, values in intervals.items()},
            'interval_units': {'driver_call_cpu_ns': 'per complete driver pair',
                               'enqueue_to_driver_cpu_ns': 'per linked submit',
                               'declared_to_enqueue_cpu_ns': 'per read candidate (may repeat one submit)',
                               'recorded_to_enqueue_cpu_ns': 'per read candidate (may repeat one command)'},
            'candidate_read_sites': [{'rip': hex(site), 'shader': f'{shader:016x}',
                'command_kind': kind, 'queue': queue, 'scopes': scopes,
                'handler': stats([r[0] for r in rows]), 'cpu_wait': stats([r[1] for r in rows])}
                for (site, shader, kind, queue, scopes), rows in
                sorted(candidate_sites.items(), key=lambda item: len(item[1]), reverse=True)[:16]],
            'candidate_shaders': [{'shader': f'{shader:016x}', 'command_kind': kind, 'queue': queue,
                                   'candidates': count} for (shader, kind, queue), count in shaders.most_common(16)],
            'limits': ['Latest retained DECLARED overlap per queue/range, not actual last shader store.',
                       'History is bounded; evictions/BDA/image aliases leave blind spots. Never remove waits from this alone.',
                       'Graphics hashes identify the vertex program, not necessarily the writing stage.',
                       'CPU command recorded/driver return are not GPU completion. Timeline snapshots are lower bounds.',
                       'Beyond a submitted snapshot is not proof that the driver has not received work; publication may lag.',
                       'Same submit may serve multiple reads; candidate intervals overlap and cannot be summed.',
                       'Missing submit links may lie outside the selected window; no submission delay is inferred.',
                       'Feedback fences mark the active scheduler only; they do not prove a complete guest production phase.']}


def analyze(session, start=0, duration=None):
    launch = json.loads((session / 'launch.json').read_text(encoding='utf-8-sig'))
    profile = launch.get('timing_profile')
    if launch.get('optimization_v3') == 'all' and launch.get('diagnostic') == 'full':
        profile = 'v3-full'
    if profile not in ('baseline', 'critical', 'lighting', 'v3-full'):
        raise ValueError('session is not an explicit timing profile')
    result = {'schema': 1, 'session': session.name, 'profile': profile,
              'runtime_sha256': launch.get('runtime_sha256'), 'game_sha256': launch.get('game_sha256'),
              'window': {'start_s': start, 'duration_s': duration}, 'errors': [],
              'performance_acceptance': False,
              'limits': ['CPU wait includes queueing, GPU work and CPU scheduling; it is not pure GPU time.',
                         'Publication can be nested in CP time. Never add these totals or different threads.',
                         'Read-to-frame associations are temporal overlap only; guest_frame is not a causal ID.',
                         'GPU result timestamps are CPU query-retirement time. GPU durations have a separate clock.',
                         'Host phases can nest (present_flush includes submit); never sum their totals.',
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
    expected_profile, expected_hash = ((2, 0) if profile == 'v3-full' else
                                       (2, 0xce1675e0) if profile == 'lighting' else (0, 0))
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
    host_phases = defaultdict(list)
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
            elif kinds & {'host_phase_begin', 'host_phase_end'}:
                if not {'host_phase_begin', 'host_phase_end'} <= kinds:
                    partial['host_phase'] += 1
                    continue
                name, elapsed = host_phase_path(events)
                host_phases[name].append(elapsed)
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
    result['host_phases_cpu'] = {name: stats(values) for name, values in host_phases.items()}
    if profile == 'v3-full':
        required = set(HOST_PHASE_NAMES) - {'swapchain_recreate'}
        missing = sorted(required - host_phases.keys())
        result['host_phase_coverage'] = {'required': sorted(required), 'missing': missing,
                                         'recreate_optional': True}
        if missing:
            result['errors'].append('V3 host phase coverage missing: ' + ', '.join(missing))
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
    overrides = launch.get('v3_overrides') or {}
    r4 = (launch.get('optimization_v3') == 'all' and
          (launch.get('optimization_revision') == 'v3-r4-evidence' or
           overrides.get('v3_descriptor_evidence') == 1))
    if r4:
        try:
            result['feedback_evidence'] = feedback_summary(groups, valid_reads)
            result['descriptor_evidence'] = descriptor_summary(
                session, require_updates=overrides.get('v3_descriptor_delta') == 1)
            counts = result['feedback_evidence']['counts']
            if (not counts.get('covered_reads') or
                    not result['feedback_evidence']['cpu_intervals_not_additive'].get('driver_call_cpu_ns')):
                result['errors'].append('R4 producer/submit evidence absent in selected window')
        except (ValueError, OSError) as exc:
            result['errors'].append('R4 evidence invalid: ' + str(exc))
    if overrides.get('v3_lighting_reduction') == 1:
        result['lighting_optimization'] = depth_reduction_summary(session)
    result['format_usable'] = not result['errors']
    return result



def resolve_window(session, start=None, duration=None):
    """Use actual collector UTC boundaries by default; never the post-capture teardown tail."""
    if start is not None:
        return start, duration or 60, 'explicit-elapsed-window'
    status_path = session / 'diagnostics' / 'status.json'
    if status_path.is_file():
        status = json.loads(status_path.read_text(encoding='utf-8-sig'))
        begin, end = status.get('window_started'), status.get('window_finished')
        if begin and end:
            quality_path = session / 'diagnostics' / 'data-quality.json'
            if quality_path.is_file():
                quality = json.loads(quality_path.read_text(encoding='utf-8-sig'))
                clock = quality.get('checks', {}).get('frames', {})
                if clock.get('wall_clock_steps_over_5ms', 0):
                    raise ValueError('Cannot align capture UTC after a detected wall-clock step')
            begin_ns, end_ns = (round(datetime.fromisoformat(t.replace('Z', '+00:00')).timestamp() * 1e9)
                                for t in (begin, end))
            if end_ns <= begin_ns:
                raise ValueError('Diagnostic capture window is empty or reversed')
            with (session / 'frames.csv').open(encoding='utf-8-sig', newline='') as stream:
                anchor = next(csv.DictReader(stream))
            anchor_ns = int(anchor['utc_ns']) - int(anchor['elapsed_ns'])
            seconds = (end_ns - begin_ns) / 1e9
            selected = min(duration, seconds) if duration is not None else seconds
            return max(0, (end_ns - anchor_ns) / 1e9 - selected), selected, 'diagnostic-capture-utc'
    with (session / 'frames.csv').open(encoding='utf-8-sig', newline='') as stream:
        last_elapsed = 0
        for row in csv.DictReader(stream):
            last_elapsed = int(row['elapsed_ns'])
    selected = duration or 60
    return max(0, last_elapsed / 1e9 - selected), selected, 'last-elapsed-window'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--session', type=Path, required=True)
    parser.add_argument('--start', type=float, help='Seconds from first flip; default: actual diagnostic capture window, otherwise last 60 seconds')
    parser.add_argument('--duration', type=float)
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    if (args.start is not None and (not math.isfinite(args.start) or args.start < 0)) or (
            args.duration is not None and (not math.isfinite(args.duration) or args.duration <= 0)):
        parser.error('start must be finite/nonnegative and duration finite/positive')
    try:
        args.start, args.duration, source = resolve_window(args.session, args.start, args.duration)
        result = analyze(args.session, args.start, args.duration)
        result['window']['source'] = source
    except (ValueError, OSError, KeyError, TypeError, StopIteration, csv.Error) as exc:
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
