# SPDX-License-Identifier: GPL-2.0-or-later
"""Join precise feedback requests to calibrated GPU copy/submission bounds, read-only."""
import argparse
import bisect
import csv
import json
import re
from collections import Counter, defaultdict
from pathlib import Path


def summary(values):
    v = sorted(values)
    if not v:
        return {"n": 0}
    return {"n": len(v), "mean_ms": sum(v) / len(v) / 1e6,
            "p50_ms": v[len(v) // 2] / 1e6,
            "p95_ms": v[min(len(v)-1, int(len(v)*.95))] / 1e6,
            "max_ms": v[-1] / 1e6, "sum_ms_not_wall": sum(v) / 1e6}


def analyze(session, writer_join=None):
    meta = json.loads((session / 'path-trace.json').read_text())
    if not meta['complete'] or any(meta[k] for k in ('dropped', 'io_failed', 'cutoff_publishers')):
        raise ValueError('Trace is incomplete or lost events')
    writers = defaultdict(list)
    block_writers = defaultdict(list)
    log = session / 'log' / 'CUSA00309.log'
    if log.is_file():
        pattern = re.compile(
            r'PTRACE target-block-writer request (\d+) address ([\da-f]+) range '
            r'([\da-f]+)\+(\d+) shader ([\da-f]+) queue (\d+) tick (\d+) sequence (\d+)')
        for m in pattern.finditer(log.read_text(encoding='utf-8', errors='replace')):
            ident, address, start, size, shader, queue, tick, sequence = m.groups()
            block_writers[int(ident)].append(
                (int(address, 16), int(start, 16), int(size), shader,
                 int(queue), int(tick), int(sequence)))
    writer_addresses = set()
    writer_joins = [writer_join] if isinstance(writer_join, dict) else writer_join or []
    for join in writer_joins:
        if join['session'] != session.name:
            raise ValueError('Writer declaration data belongs to another session')
        address = int(join['address'], 16)
        if address in writer_addresses:
            raise ValueError(f'Duplicate writer declaration data for {address:#x}')
        writer_addresses.add(address)
        for row in join['joined']:
            writers[address, int(row['id'])].append(row)
    groups, copies, sessions, submissions, samples = defaultdict(list), {}, {}, {}, {}
    driver_begins, driver_ends = {}, defaultdict(list)
    workloads = {}
    dispatch_orders = defaultdict(dict)
    dispatch_hashes, dispatch_bounds = {}, {}
    read_threads, flip_threads = {}, Counter()
    calibrations, count = [], Counter()
    anchor = meta['start']
    cpu_anchor = (anchor['ns_before'] + anchor['ns_after']) / 2
    frequency = anchor['frequency']
    with (session / 'path-trace.csv').open() as f:
        for row in csv.DictReader(f):
            kind = row['event']
            ns, ident, a, b, c, d = (int(row[k]) for k in ('ns','id','a','b','c','d'))
            count[kind] += 1
            if kind.startswith('read_'):
                groups[ident].append((ns, kind, a, b, c, d))
                if kind == 'read_request':
                    read_threads[ident] = int(row['thread'])
            elif kind == 'flip_eop_register':
                flip_threads[int(row['thread'])] += 1
            elif kind == 'gpu_copy_bounds':
                copies[ident] = (a, b, c, d)
            elif kind == 'gpu_session_bounds':
                sessions[d, a] = (b, c)
            elif kind == 'gpu_session_workload':
                workloads[d, a] = (b, c)
            elif kind == 'gpu_session_dispatch':
                if b in dispatch_orders[d, a]:
                    raise ValueError('Duplicate dispatch ordinal in one submission')
                dispatch_orders[d, a][b] = c
            elif kind == 'submit_driver_begin':
                driver_begins[ident] = (ns, a, b)
            elif kind == 'submit_driver_end':
                driver_ends[a, b].append((ident, ns))
            elif kind == 'gpu_clock_calibration':
                cpu_ns = cpu_anchor + (b-anchor['qpc'])*1e9/frequency
                calibrations.append((a, cpu_ns-a, c))
            elif kind == 'frame_sample':
                samples[a] = b
            elif kind == 'gpu_dispatch_result':
                dispatch_hashes[ident] = a
            elif kind == 'gpu_dispatch_bounds':
                dispatch_bounds[ident] = (a, b, c, d)
    if sum(count.values()) != meta['events'] or len(calibrations) < 2:
        raise ValueError('Event count mismatch or missing calibrated timestamps')
    for key, receipts in driver_ends.items():
        if len(receipts) == 1:
            submissions[key] = receipts[0][1]
    for key, order in dispatch_orders.items():
        expected = workloads.get(key)
        if expected is None or set(order) != set(range(expected[1])):
            raise ValueError('Dispatch order does not match submission workload')
    calibrations.sort()
    stamps = [r[0] for r in calibrations]

    def cpu(gpu):
        i = bisect.bisect_left(stamps, gpu)
        left, right = calibrations[max(0,i-1)], calibrations[min(i,len(stamps)-1)]
        fraction = (gpu-left[0])/(right[0]-left[0]) if right[0] != left[0] else 0
        return gpu + left[1] + fraction*(right[1]-left[1])

    rounds = json.loads((session / 'super-rounds.json').read_text(encoding='utf-8-sig'))
    frames = list(csv.DictReader((session / 'frames.csv').open(encoding='utf-8-sig')))
    release_utc = next(c['release']['applied_unix_ns'] for c in rounds if c.get('release'))
    near = min((f for f in frames if int(f['frame']) in samples),
               key=lambda f: abs(int(f['utc_ns'])-release_utc))
    release = samples[int(near['frame'])] + release_utc-int(near['utc_ns'])
    submit_samples = []
    submit_exclusions = Counter()
    for key, (gpu_begin, gpu_end) in sessions.items():
        receipts = driver_ends.get(key, ())
        if len(receipts) != 1:
            submit_exclusions['ambiguous_tick' if receipts else 'no_driver_receipt'] += 1
            continue
        ident, driver_end = receipts[0]
        driver_begin = driver_begins.get(ident)
        if driver_begin is None or driver_begin[1:] != key or driver_end < driver_begin[0]:
            submit_exclusions['no_valid_driver_begin'] += 1
            continue
        if key not in workloads:
            continue
        draws, dispatch_count = workloads[key]
        category = ('draw-heavy' if draws > 100 else 'compute-heavy' if dispatch_count >= 80 else
                    'small' if draws or dispatch_count else 'other')
        submit_samples.append(((driver_begin[0] - release) / 1e9, category,
                               driver_end - driver_begin[0], gpu_end - gpu_begin))
    records, exclusions = [], Counter()
    dispatches = defaultdict(list)
    for ident, (begin, end, tick, queue) in dispatch_bounds.items():
        if ident not in dispatch_hashes or end < begin:
            exclusions['unmatched_or_invalid_dispatch_bounds'] += 1
            continue
        dispatches[queue, tick].append((begin, end, dispatch_hashes[ident]))
    dispatch_timing = defaultdict(list)
    for entries in dispatches.values():
        for begin, end, shader in entries:
            dispatch_timing[f'{shader:016x}'].append(end - begin)
    dispatch_start = min((r[0] for r in dispatch_bounds.values()), default=None)
    dispatch_end = max((r[1] for r in dispatch_bounds.values()), default=None)
    by_queue = defaultdict(list)
    commands_by_queue = defaultdict(list)
    for (queue, tick), entries in dispatches.items():
        by_queue[queue].extend((cpu(begin), cpu(end), tick) for begin, end, _ in entries)
        commands_by_queue[queue].extend((cpu(begin), cpu(end), shader)
                                        for begin, end, shader in entries)
    for entries in by_queue.values():
        entries.sort()
    begin_times = {queue: [begin for begin, _, _ in entries]
                   for queue, entries in by_queue.items()}
    envelopes = defaultdict(list)
    for (queue, tick), (begin, end) in sessions.items():
        envelopes[queue].append((cpu(begin), cpu(end), tick))
    for entries in envelopes.values():
        entries.sort()
    envelope_starts = {queue: [begin for begin, _, _ in entries]
                       for queue, entries in envelopes.items()}
    envelope_max_duration = {queue: max(end - begin for begin, end, _ in entries)
                             for queue, entries in envelopes.items()}

    def union_ns(intervals):
        covered = 0
        last = None
        for left, right in sorted(intervals):
            covered += max(0, right - max(left, last if last is not None else left))
            last = max(right, last if last is not None else right)
        return round(covered)

    def registration_read_overlap():
        commands = sorted(commands_by_queue[0])
        if not commands or not flip_threads:
            return {}
        start = commands[0][0]
        end = max(hi for _, hi, _ in commands)
        starts = [lo for lo, _, _ in commands]
        longest = max(hi - lo for lo, hi, _ in commands)
        by_shader = Counter()
        handler_durations = []
        per_request = []
        for ident, events in groups.items():
            if read_threads.get(ident) not in flip_threads:
                continue
            req = next((e for e in events if e[1] == 'read_request'), None)
            ret = next((e for e in events if e[1] == 'read_return'), None)
            if not req or not ret or not start <= req[0] <= ret[0] <= end:
                continue
            if req[2] not in (0x273530404, 0x275330404):
                continue
            first = bisect.bisect_left(starts, req[0] - longest)
            stop = bisect.bisect_left(starts, ret[0])
            overlaps = defaultdict(list)
            for lo, hi, shader in commands[first:stop]:
                left, right = max(lo, req[0]), min(hi, ret[0])
                if left < right:
                    overlaps[shader].append((left, right))
            by_shader.update({f'{shader:016x}': union_ns(spans)
                              for shader, spans in overlaps.items()})
            per_request.append(union_ns(span for spans in overlaps.values() for span in spans))
            handler_durations.append(ret[0] - req[0])
        return {
            'sampling_window_s': [(start - release) / 1e9, (end - release) / 1e9],
            'read_durations': summary(handler_durations),
            'observed_dispatch_overlap': summary(per_request),
            'top_hash_overlap_ms': {k: v / 1e6 for k, v in by_shader.most_common(32)},
            'limit': 'Temporal overlap with sampled graphics-queue compute intervals. '
                     'These are not dependencies or complete GPU coverage.',
        }

    def classify_envelopes(queue, left, right, tick):
        starts = envelope_starts[queue]
        first = bisect.bisect_left(starts, left - envelope_max_duration[queue])
        stop = bisect.bisect_left(starts, right)
        edges = []
        longest_mixed = (0, None)
        longest_sampled = (0, None)
        for begin, end, earlier_tick in envelopes[queue][first:stop]:
            begin, end = max(begin, left), min(end, right)
            if earlier_tick >= tick or end <= begin:
                continue
            counts = workloads.get((queue, earlier_tick))
            category = ('unclassified' if counts is None else
                        ('mixed' if counts[1] else 'draw') if counts[0] else
                        'compute' if counts[1] else 'other')
            if category == 'mixed' and end - begin > longest_mixed[0]:
                longest_mixed = (end - begin, counts)
            if (queue, earlier_tick) in dispatch_orders and end - begin > longest_sampled[0]:
                longest_sampled = (end - begin, earlier_tick)
            edges.extend(((begin, category, 1), (end, category, -1)))
        active, duration = Counter(), Counter()
        previous = left
        for stamp, category, delta in sorted(edges):
            if stamp > previous:
                categories = [k for k, n in active.items() if n > 0]
                if categories:
                    kind = categories[0] if len(categories) == 1 else 'overlap'
                    duration[kind] += stamp - previous
            active[category] += delta
            previous = stamp
        return duration, longest_mixed[1], longest_sampled[1]

    for ident, events in groups.items():
        events.sort()
        req = next((e for e in events if e[1] == 'read_request'), None)
        ret = next((e for e in events if e[1] == 'read_return'), None)
        if not req or not ret:
            continue
        for cp in (e for e in events if e[1] == 'read_copy'):
            bounds = copies.get(cp[5])
            if not bounds:
                exclusions['unprofiled_copy'] += 1
                continue
            start, end, tick, queue = bounds
            sb = sessions.get((queue,tick))
            submit = submissions.get((queue,tick))
            wait = next((e for e in events if e[1] == 'read_wait_end' and e[4] == tick
                         and e[3] == cp[3]), None)
            if sb is None or submit is None or wait is None:
                exclusions['missing_join'] += 1
                continue
            ss, se = sb
            if not ss <= start <= end <= se:
                exclusions['copy_outside_submission_bounds'] += 1
                continue
            deps = next((e for e in reversed(events)
                         if e[1] == 'read_dependencies' and e[0] <= cp[0]), None)
            producer = ('done' if deps[5] & 1 else 'submitted' if deps[5] & 2
                        else 'current' if deps[5] & 4 else 'queued') if deps else 'pending-copy-reuse'
            # Only timed dispatches in this exact queue submission can explain its
            # pre-copy bounds. Never use the later CPU query-retirement timestamp.
            kernels = defaultdict(lambda: {'count': 0, 'clipped_ns': 0})
            intervals = []
            for dispatch_begin, dispatch_end, shader in dispatches.get((queue, tick), []):
                left, right = max(ss, dispatch_begin), min(start, dispatch_end)
                if right > left:
                    entry = kernels[f'{shader:016x}']
                    entry['count'] += 1
                    entry['clipped_ns'] += right-left
                    intervals.append((left, right))
            covered = union_ns(intervals)
            # Also cover older queue submissions during the request, up to the point
            # where the copy submission's TOP timestamp occurs. Intervals from a
            # later tick never explain the request's earlier wait.
            prior_end = min(cpu(ss), cpu(start))
            entries = by_queue.get(queue, ())
            stop = bisect.bisect_left(begin_times.get(queue, ()), prior_end)
            prior = union_ns((max(req[0], begin), min(prior_end, end))
                             for begin, end, earlier_tick in entries[:stop]
                             if earlier_tick < tick and min(prior_end, end) > max(req[0], begin))
            prev = sessions.get((queue, tick - 1))
            # A prior GPU session may overlap this session's TOP timestamp. Clip
            # its measured execution envelope to the driver's queue-delay window.
            prior_session_overlap = (
                max(0, min(cpu(ss), cpu(prev[1])) - max(submit, cpu(prev[0])))
                if prev else None
            )
            starts = envelope_starts[queue]
            first = bisect.bisect_left(starts, submit - envelope_max_duration[queue])
            stop = bisect.bisect_left(starts, cpu(ss))
            queue_overlap = union_ns((max(submit, begin), min(cpu(ss), end))
                                     for begin, end, earlier_tick in envelopes[queue][first:stop]
                                     if earlier_tick < tick and min(cpu(ss), end) > max(submit, begin))
            writer = None
            writer_metrics = {}
            if req[2] in writer_addresses and deps:
                candidates = [w for w in writers.get((req[2], ident), ())
                              if w['dependency_sequence'] == deps[4] and w['queue'] == queue
                              and w['dependency_tick'] == deps[2 + queue]]
                if candidates:
                    writer = max(candidates, key=lambda w: w['sequence'])
                    writer_tick = writer['writer_tick']
                    writer_session = sessions.get((queue, writer_tick))
                    if writer_session and writer_tick <= tick:
                        ws, we = writer_session
                        # The producer submission's BOTTOM bounds the declared dispatch.
                        # The interval after it is a lower bound on the observed gap
                        # after that dispatch, conditional on this declaration being it.
                        writer_metrics = {
                            'writer_tick_gap': tick - writer_tick,
                            'writer_end_to_copy_start_ns': start - we if writer_tick < tick else None,
                            'writer_end_to_copy_submit_top_ns': ss - we if writer_tick < tick else None,
                            'writer_already_complete_at_request': cpu(we) <= req[0],
                            'writer_session_duration_ns': we - ws,
                        }
                        # Exclude time before the CPU asked for the result. A producer
                        # that was already complete has zero residual producer wait.
                        if writer_tick < tick and we <= start and req[0] <= cpu(start):
                            writer_complete = max(req[0], cpu(we))
                            writer_metrics.update({
                                'request_to_declared_writer_complete_ns':
                                    writer_complete - req[0],
                                'request_after_declared_writer_to_copy_ns':
                                    cpu(start) - writer_complete,
                            })
                        if workloads and writer_tick < tick:
                            for label, left, right in (
                                ('before_writer', submit, min(cpu(we), cpu(ss))),
                                ('after_writer', max(submit, cpu(we)), cpu(ss)),
                            ):
                                durations, longest_mixed, sampled = classify_envelopes(
                                    queue, left, right, tick)
                                for category in ('draw', 'compute', 'mixed', 'other',
                                                 'overlap', 'unclassified'):
                                    writer_metrics[f'{label}_{category}_envelope_ns'] = (
                                        durations[category])
                                if longest_mixed:
                                    writer_metrics[f'{label}_largest_mixed_draws'] = longest_mixed[0]
                                    writer_metrics[f'{label}_largest_mixed_dispatches'] = longest_mixed[1]
                                if sampled is not None:
                                    writer_metrics[f'{label}_sampled_tick'] = sampled
                        same_hash_dispatches = [
                            (begin, end) for begin, end, shader in dispatches[queue, writer_tick]
                            if shader == int(writer['shader'], 16)
                        ]
                        if len(same_hash_dispatches) == 1:
                            writer_begin, writer_end = same_hash_dispatches[0]
                            writer_metrics.update({
                                'single_observed_writer_dispatch_ns': writer_end - writer_begin,
                                'single_observed_writer_dispatch_end_to_copy_ns': start - writer_end,
                                'single_observed_writer_dispatch_end_to_writer_signal_ns': we - writer_end,
                            })
            row = {'id': ident, 'copy_id': cp[5], 'address': hex(req[2]), 'rip': hex(req[4]),
                   'release_s': (req[0]-release)/1e9, 'queue': queue, 'tick': tick,
                   'total_ns': ret[0]-req[0], 'request_to_submit_ns': submit-req[0],
                   'driver_to_gpu_start_ns': cpu(ss)-submit,
                   'gpu_before_copy_ns': start-ss, 'gpu_copy_ns': end-start,
                   'gpu_after_copy_ns': se-end,
                   'gpu_end_to_cpu_wake_ns': wait[0]-cpu(se),
                   'wake_to_return_ns': ret[0]-wait[0],
                   'writer_state_at_copy_scheduling': producer,
                   'writer_ticks': [deps[2], deps[3]] if deps else None,
                   'profiled_dispatch_before_copy_ns': covered,
                   'profiled_older_ticks_during_request_ns': prior,
                   'previous_tick_overlap_with_queue_delay_ns': prior_session_overlap,
                   'older_tick_union_with_queue_delay_ns': queue_overlap,
                   'flip_registration_thread': read_threads[ident] in flip_threads,
                   'within_dispatch_sampling_bounds': (
                       dispatch_start is not None and cpu(dispatch_start) <= req[0]
                       and cpu(end) <= cpu(dispatch_end)
                   ),
                   'profiled_dispatches_before_copy': dict(kernels)}
            if writer:
                row['writer_declaration'] = writer
                row.update(writer_metrics)
            target_samples = [e for e in events if e[1] == 'read_target_ticks']
            if len(target_samples) == 1 and deps and queue == 0 and cp[3] == 0:
                target = target_samples[0]
                if target[4] == deps[4] and target[5] == req[2]:
                    local_tick, selected_tick = target[2], deps[2]
                    row['target_tracking_tick'] = local_tick
                    row['selected_copy_dependency_tick'] = selected_tick
                    if local_tick and selected_tick:
                        row['selected_minus_target_tick'] = selected_tick - local_tick
                    if writer and local_tick:
                        row['target_minus_declared_writer_tick'] = local_tick - writer['writer_tick']
                    sources = block_writers.get(ident, ())
                    if len(sources) == 1:
                        address, base, size, shader, source_queue, source_tick, seq = sources[0]
                        if address == req[2] and source_queue == queue and source_tick == local_tick \
                                and local_tick and seq <= target[4]:
                            covers_word = base < address + 4 and address < base + size
                            row['target_block_writer_shader'] = shader
                            row['target_block_writer_covers_word'] = covers_word
            records.append(row)
    def phase(rows):
        kernel_totals = defaultdict(lambda: {'instances_across_requests': 0, 'clipped_ms_not_wall': 0})
        for row in rows:
            for shader, data in row['profiled_dispatches_before_copy'].items():
                kernel_totals[shader]['instances_across_requests'] += data['count']
                kernel_totals[shader]['clipped_ms_not_wall'] += data['clipped_ns']/1e6
        before = sum(r['gpu_before_copy_ns'] for r in rows)
        return {'n':len(rows), 'metrics':{
            k:summary([r[k] for r in rows if r.get(k) is not None])
            for k in sorted({key for r in rows for key in r if key.endswith('_ns')})},
            'addresses':dict(Counter(r['address'] for r in rows)),
            'writer_states': dict(Counter(r['writer_state_at_copy_scheduling'] for r in rows)),
            'target_tick_gaps': {
                key: dict(Counter(r[key] for r in rows if key in r))
                for key in ('selected_minus_target_tick', 'target_minus_declared_writer_tick')
                if any(key in r for r in rows)
            },
            'target_block_writer_bindings': dict(Counter(
                r['target_block_writer_shader'] + (
                    ':overlaps_word' if r['target_block_writer_covers_word'] else ':disjoint_word')
                for r in rows if 'target_block_writer_shader' in r)),
            'timed_dispatches_before_copy': dict(kernel_totals),
            'timed_dispatch_union_fraction_of_before_copy':
                sum(r['profiled_dispatch_before_copy_ns'] for r in rows)/before if before > 0 else None}
    windows = [('baseline',-10,0),('early',0,3),('peak',3,8),('late',8,13),('recover',13,23),
               ('late_first_second',8,9)]
    submit_phases = {}
    dispatch_phases = {}
    for name, lo, hi in windows:
        submit_phases[name] = {}
        for category in ('draw-heavy', 'compute-heavy', 'small', 'other'):
            rows = [row for row in submit_samples if lo <= row[0] < hi and row[1] == category]
            if rows:
                submit_phases[name][category] = {
                    'driver_call': summary([row[2] for row in rows]),
                    'gpu_envelope': summary([row[3] for row in rows]),
                }
        per_shader = defaultdict(list)
        for entries in dispatches.values():
            for begin, end, shader in entries:
                if lo <= (cpu(begin) - release) / 1e9 < hi:
                    per_shader[f'{shader:016x}'].append(end - begin)
        dispatch_phases[name] = {shader: summary(values) for shader, values in per_shader.items()}
    phases = {name: phase([r for r in records if lo <= r['release_s'] < hi])
              for name, lo, hi in windows}
    registration_thread = {
        name: phase([r for r in records if lo <= r['release_s'] < hi
                     and r['flip_registration_thread']])
        for name, lo, hi in windows
    }
    registration_main_words = {}
    for address in ('0x273530404', '0x275330404'):
        address_rows = [r for r in records
                        if r['flip_registration_thread'] and r['address'] == address]
        registration_main_words[address] = {}
        for name, lo, hi in windows:
            rows = [r for r in address_rows if lo <= r['release_s'] < hi]
            if rows:
                registration_main_words[address][name] = phase(rows)
    declared_writers = {}
    declared_writers_by_address = {}
    if writer_addresses:
        for address in sorted(writer_addresses):
            rows = [r for r in records
                    if r['address'] == hex(address) and 'writer_declaration' in r]
            declared_writers_by_address[hex(address)] = {
                name: phase([r for r in rows if lo <= r['release_s'] < hi])
                for name, lo, hi in windows
            }
        for name, lo, hi in windows:
            rows = [r for r in records if lo <= r['release_s'] < hi and 'writer_declaration' in r]
            declared_writers[name] = {
                **phase(rows),
                'shaders': dict(Counter(r['writer_declaration']['shader'] for r in rows)),
                'writer_tick_gaps': dict(Counter(r['writer_tick_gap'] for r in rows
                                                if 'writer_tick_gap' in r)),
                'writer_already_complete_at_request': sum(
                    r.get('writer_already_complete_at_request', False) for r in rows),
                'workload_counts': {
                    k: dict(Counter(r[k] for r in rows if k in r))
                    for k in ('before_writer_largest_mixed_draws', 'before_writer_largest_mixed_dispatches',
                              'after_writer_largest_mixed_draws', 'after_writer_largest_mixed_dispatches')
                },
            }
            for label in ('before_writer', 'after_writer'):
                keys = {(r['queue'], r[f'{label}_sampled_tick']) for r in rows
                        if f'{label}_sampled_tick' in r}
                hashes, pairs, repeated_patterns = Counter(), Counter(), Counter()
                for key in keys:
                    order = dispatch_orders[key]
                    seq = tuple(f'{order[i]:08x}' for i in range(len(order)))
                    hashes.update(seq)
                    pairs.update(f'{a} {b}' for a, b in zip(seq, seq[1:]))
                    for period in range(1, min(len(seq) // 2, 16) + 1):
                        if all(h == seq[i % period] for i, h in enumerate(seq)):
                            repeated_patterns[' '.join(seq[:period])] += 1
                            break
                declared_writers[name][f'{label}_sampled_order'] = {
                    'unique_submissions': len(keys), 'requests': sum(
                        f'{label}_sampled_tick' in r for r in rows),
                    'hashes': dict(hashes.most_common(24)),
                    'transitions': dict(pairs.most_common(24)),
                    'whole_submission_patterns': dict(repeated_patterns),
                }
    within_sampling = [
        r for r in records if r['within_dispatch_sampling_bounds'] and r['flip_registration_thread']
    ]
    return {'session':str(session), 'trace_metadata':meta, 'event_counts':dict(count),
            'clock':{'samples':len(calibrations),'max_deviation_ns':max(r[2] for r in calibrations),
                     'offset_span_ns':max(r[1] for r in calibrations)-min(r[1] for r in calibrations),
                     'first_gpu_ns':stamps[0],'last_gpu_ns':stamps[-1]},
            'limits':['Calibrated device/QPC clocks are interpolated; deviation is recorded.',
                      'Request sums may overlap; these are not frame-time additive budgets.',
                      'Session start/end are top/bottom-of-pipe bounds, not summed kernel time.',
                      'Only profiled graphics demand copies qualify; excluded copies are counted.',
                      'Timed-dispatch coverage is the union of recorded bottom-of-pipe timestamp intervals inside this exact submission before its copy. It is not total shader coverage or a pure execution budget; unmeasured regions include draws, untimed compute, dependencies and timestamp effects.',
                      'Older-tick coverage is a temporal overlap on the same queue before the copy submission TOP timestamp. It does not prove a resource dependency.',
                      'Older-tick and previous-tick submission envelopes include internal waits. Their overlap with queue delay is a subset, not an additional stage.',
                      'Workload categories count recorded draw/dispatch commands, including indirect commands once. Other may include copies, clears, rendering helpers or synchronization. Envelope coverage is not exclusive GPU execution time.',
                      'Writer state is a window-level scheduling snapshot, not the final producer shader identity.'],
            'exclusions':dict(exclusions),'phases':phases,
            'registration_threads': dict(flip_threads),
            'registration_thread_phases': registration_thread,
            'registration_main_word_phases': registration_main_words,
            'declared_writer_phases': declared_writers,
            'declared_writer_phases_by_address': declared_writers_by_address,
            'sampled_dispatch_order_submissions': len(dispatch_orders),
            'timed_dispatch_durations': {shader: summary(values)
                                         for shader, values in dispatch_timing.items()},
            'timed_dispatch_phases': dispatch_phases,
            'registration_main_word_overlap': registration_read_overlap(),
            'submit_driver_phases': submit_phases,
            'submit_driver_exclusions': dict(submit_exclusions),
            'registration_thread_within_dispatch_sampling_bounds': phase(within_sampling),
            'qualified_copy_records': len(records),
            'qualified_unique_requests': len({r['id'] for r in records}),
            'timed_dispatch_hash_counts': dict(Counter(f'{v:016x}' for v in dispatch_hashes.values())),
            'timed_dispatch_start_range_s': (
                [(cpu(t) - release) / 1e9 for t in (
                    min(r[0] for r in dispatch_bounds.values()),
                    max(r[0] for r in dispatch_bounds.values()),
                )] if dispatch_bounds else []
            ),
            'largest_requests':sorted(records,key=lambda r:-r['total_ns'])[:60]}


if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('session',type=Path)
    p.add_argument('--output',type=Path,required=True)
    p.add_argument('--writer-join', type=Path, action='append', default=[],
                   help='Writer declaration JSON for one address. Repeat for additional addresses.')
    args = p.parse_args()
    result = analyze(args.session, [json.loads(path.read_text(encoding='utf-8-sig'))
                                    for path in args.writer_join])
    args.output.write_text(json.dumps(result,indent=2)+'\n')
    print(json.dumps({'clock':result['clock'],'phases':result['phases']},indent=2))
