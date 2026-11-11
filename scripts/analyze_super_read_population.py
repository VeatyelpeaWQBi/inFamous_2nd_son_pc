# SPDX-License-Identifier: GPL-2.0-or-later
"""Account for ordinary and private-word reads on the EOP registration thread, read-only."""
import argparse
from collections import Counter, defaultdict
import csv
import json
from pathlib import Path

PRIMARY = (0x273530404, 0x275330404)


def union_ns(spans):
    total = 0
    end = -1
    for lo, hi in sorted(spans):
        total += max(0, hi - max(lo, end))
        end = max(end, hi)
    return total


def analyze(session):
    meta = json.loads((session / 'path-trace.json').read_text(encoding='utf-8-sig'))
    if not meta['complete'] or any(meta[k] for k in ('dropped', 'io_failed', 'cutoff_publishers')):
        raise ValueError('Incomplete trace')
    rounds = json.loads((session / 'super-rounds.json').read_text(encoding='utf-8-sig'))
    release, = (r['release'] for r in rounds if r.get('release'))
    clock = meta['start']
    origin = (clock['ns_before'] + clock['ns_after']) // 2
    origin += (release['qpc_after'] - clock['qpc']) * 10**9 // clock['frequency']
    requests, returns, flips, words = {}, {}, [], []
    checks, copies = [], Counter()
    count = 0
    first_event, last_event = None, None
    with (session / 'path-trace.csv').open(encoding='utf-8-sig', newline='') as stream:
        for row in csv.DictReader(stream):
            count += 1
            event_stamp = int(row['ns'])
            first_event = event_stamp if first_event is None else min(first_event, event_stamp)
            last_event = event_stamp if last_event is None else max(last_event, event_stamp)
            kind = row['event']
            if kind not in ('read_request', 'read_return', 'flip_eop_register',
                            'word_load_result', 'word_value_check', 'word_prefetch_copy'):
                continue
            stamp, ident, thread, a, b, c, d = (int(row[k]) for k in
                                               ('ns', 'id', 'thread', 'a', 'b', 'c', 'd'))
            if kind == 'read_request':
                if ident in requests:
                    raise ValueError('Duplicate read request')
                requests[ident] = (stamp, thread, a, c)
            elif kind == 'read_return':
                if ident in returns:
                    raise ValueError('Duplicate read return')
                returns[ident] = (stamp, thread)
            elif kind == 'flip_eop_register':
                flips.append((stamp, thread))
            elif kind == 'word_load_result':
                if c > stamp:
                    raise ValueError('Invalid private word duration')
                words.append((stamp - c, stamp, thread, a, d))
            elif kind == 'word_value_check':
                if d not in (0, 1) or bool(d) != (b == c):
                    raise ValueError('Invalid reference comparison record')
                checks.append(dict(address=hex(a), private=b, reference=c, equal=bool(d),
                                   release_seconds=(stamp - origin) / 10**9))
            elif kind == 'word_prefetch_copy':
                copies[hex(a)] += 1
    if count != meta['events'] or not flips:
        raise ValueError('Trace count mismatch or missing EOP')
    registration = Counter(thread for _, thread in flips).most_common(1)[0][0]
    spans = []
    unmatched = 0
    for ident, (start, thread, address, site) in requests.items():
        if ident not in returns:
            unmatched += 1
            continue
        end, returned_thread = returns[ident]
        if end < start or thread != returned_thread:
            raise ValueError('Invalid ordinary read interval')
        if thread == registration:
            spans.append((start, end, address, site))
    windows = {}
    for lo, hi in ((-8, -1), (0, 3), (3, 8), (8, 12)):
        left, right = origin + lo * 10**9, origin + hi * 10**9
        n = sum(left <= stamp < right and thread == registration for stamp, thread in flips)
        ordinary = defaultdict(list)
        attempts = defaultdict(list)
        reasons = defaultdict(Counter)
        for start, end, address, site in spans:
            clipped = max(left, start), min(right, end)
            if clipped[0] < clipped[1]:
                ordinary[address, site].append(clipped)
        for start, end, thread, address, reason in words:
            if thread == registration and min(right, end) > max(left, start):
                attempts[address].append((max(left, start), min(right, end)))
                reasons[address][reason] += 1
        categories = defaultdict(list)
        scopes = defaultdict(list)
        pages = {address // 4096 for address in PRIMARY}
        feedback_windows = {address // (512 * 1024) for address in PRIMARY}
        for (address, site), values in ordinary.items():
            category = ('primary' if address in PRIMARY else
                        'same_page_other' if address // 4096 in pages else 'other_page')
            categories[category].extend(values)
            scopes['observed_windows' if address // (512 * 1024) in feedback_windows else
                   'outside_windows'].extend(values)
        for address, values in attempts.items():
            category = ('primary' if address in PRIMARY else
                        'same_page_other' if address // 4096 in pages else 'other_page')
            categories[category].extend(values)
            scopes['observed_windows' if address // (512 * 1024) in feedback_windows else
                   'outside_windows'].extend(values)
        scale = n * 1e6 if n else None
        windows[f'{lo}:{hi}'] = {
            'coverage_complete': first_event <= left and last_event >= right,
            'covered_seconds': max(0, min(right, last_event) - max(left, first_event)) / 10**9,
            'registrations': n,
            'union_ms_per_registration': {
                key: union_ns(categories[key]) / scale if scale else None
                for key in ('primary', 'same_page_other', 'other_page')},
            'all_read_union_ms_per_registration': (
                union_ns([s for values in categories.values() for s in values]) / scale
                if scale else None),
            'scope_union_ms_per_registration': {
                key: union_ns(scopes[key]) / scale if scale else None
                for key in ('observed_windows', 'outside_windows')},
            'top_ordinary_other_reads': sorted([
                dict(address=hex(address), site=hex(site), requests=len(values),
                     ms_per_registration=union_ns(values) / scale if scale else None)
                for (address, site), values in ordinary.items() if address not in PRIMARY
            ], key=lambda row: -(row['ms_per_registration'] or 0))[:16],
            'private_word_attempts': {
                hex(address): dict(attempts=len(values), reasons=dict(reasons[address]),
                                  ms_per_registration=union_ns(values) / scale if scale else None)
                for address, values in attempts.items()},
        }
    return dict(session=session.name, events=count, registration_trace_slot=registration,
                primary_addresses=[hex(a) for a in PRIMARY], unmatched_reads=unmatched,
                value_checks=checks, reference_mismatches=sum(not c['equal'] for c in checks),
                prefetched_word_copies=dict(copies), windows=windows,
                limit='Same-thread read interval unions per EOP registration. Includes private '
                      'attempt failures and nested reference reads without double counting. '
                      'These are diagnostic wall times, not pure GPU work or FPS gains. '
                      'Address roles and two aligned 512 KiB scope bins apply only to the '
                      'explicitly listed fixed-save test population, not arbitrary arenas.')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('session', type=Path)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    result = analyze(args.session)
    args.output.write_text(json.dumps(result, indent=2) + '\n', encoding='utf-8')
    print(json.dumps({k: result[k] for k in
                      ('session', 'events', 'reference_mismatches', 'prefetched_word_copies')}))
