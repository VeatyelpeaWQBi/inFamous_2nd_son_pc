# SPDX-License-Identifier: GPL-2.0-or-later
"""Bounded command-side evidence. No performance or CPU-generation certification."""
import argparse
from collections import Counter, defaultdict
import csv
import json
from pathlib import Path
from analyze_timing import Event, FIELDS, one
from project_paths import project_child, write_json

HIGH = 1 << 63
MASK = (1 << 64) - 1
KINDS = {'aux_begin', 'aux_packet', 'aux_words', 'aux_end'}


def decode_context(ident, events, limit):
    begin, end = one(events, 'aux_begin'), one(events, 'aux_end')
    if not ident & HIGH or not 0 < begin.c <= MASK - begin.b or begin.c % 4 or begin.a >= 56:
        raise ValueError('Invalid context identity/range/queue')
    if any(e.thread != begin.thread or not begin.ns <= e.ns <= end.ns for e in events):
        raise ValueError('Context changed thread or escaped its lifetime')
    middle = sorted((e for e in events if e.kind not in {'aux_begin', 'aux_end'}), key=lambda e: e.ns)
    if len(middle) % 2 or end.a != len(middle) // 2 or end.a > limit or end.c not in (0, 1) or end.d > 3:
        raise ValueError('Packet count, budget or end flags disagree')
    packets = []
    for i in range(0, len(middle), 2):
        head, packed = middle[i:i + 2]
        if head.kind != 'aux_packet' or packed.kind != 'aux_words' or head.b != i // 2 + 1 or head.d not in (0, 1):
            raise ValueError('Missing/reordered packet words or ordinal')
        count = 4 if head.a == 3 else 7
        if head.a not in (1, 2, 3) or packed.d >> 32 != count:
            raise ValueError('Unknown packet kind/width')
        raw = [packed.a & 0xffffffff, packed.a >> 32, packed.b & 0xffffffff, packed.b >> 32,
               packed.c & 0xffffffff, packed.c >> 32, packed.d & 0xffffffff]
        opcode = {1: 0x50, 2: 0x49, 3: 0x3f}[head.a]
        if raw[0] >> 30 != 3 or (raw[0] >> 8) & 255 != opcode or ((raw[0] >> 16) & 0x3fff) + 2 != count:
            raise ValueError('Packet header does not match its decoder evidence')
        if count == 4 and (packed.c or packed.d & 0xffffffff):
            raise ValueError('IB packet has unexpected trailing words')
        if not head.d and not begin.b <= head.c <= begin.b + begin.c - count * 4:
            raise ValueError('Packet address lies outside the declared command context')
        packet = {'kind': {1: 'dma', 2: 'release', 3: 'ib'}[head.a], 'ordinal': head.b,
                  'ns': head.ns, 'address': hex(head.c), 'buffered_fragment': bool(head.d),
                  'words': [f'{w:08x}' for w in raw[:count]]}
        if head.a == 1:
            packet.update(source=hex(raw[2] | raw[3] << 32), destination=hex(raw[4] | raw[5] << 32),
                          bytes=raw[6] & 0x1fffff, cp_sync=bool(raw[1] >> 31),
                          source_select=(raw[1] >> 29) & 3, destination_select=(raw[1] >> 20) & 3)
        elif head.a == 2:
            packet.update(destination=hex(raw[3] | raw[4] << 32), value_low=raw[5], value_high=raw[6],
                          data_select=raw[2] >> 29, interrupt_select=(raw[2] >> 24) & 7,
                          matches_r14_release_template=raw[:3] == [0xc0054902, 0x62f, 0x23000000] and raw[6] == 0)
        else:
            packet.update(target=hex(raw[1] | (raw[2] & 0xffff) << 32), target_bytes=(raw[3] & 0xfffff) * 4)
        packets.append(packet)
    candidates = []
    for first, second in zip(packets, packets[1:]):
        if (first['kind'] == second['kind'] == 'dma' and first['bytes'] and
                first['bytes'] % 0xb0 == 0 and second['bytes'] % 0x50 == 0 and
                first['bytes'] // 0xb0 == second['bytes'] // 0x50 and
                first['words'][1] == '04004000' and second['words'][1] == '84004000'):
            candidates.append({'ordinals': [first['ordinal'], second['ordinal']],
                               'count': first['bytes'] // 0xb0, 'semantic_identity_proven': False})
    return {'context_id': str(ident), 'parent_context_id': str(begin.d), 'queue': begin.a,
            'base': hex(begin.b), 'bytes': begin.c, 'begin_ns': begin.ns, 'end_ns': end.ns,
            'finished': bool(end.c), 'span_consumed': bool(end.d & 1), 'indirect': bool(end.d & 2),
            'omitted_packets': end.b, 'packets': packets, 'candidate_dma_pairs': candidates,
            'closed_metadata_context': bool(end.c and end.d & 1 and not end.b and
                                            not any(p['buffered_fragment'] for p in packets))}


def analyze(session):
    load = lambda p: json.loads(p.read_text(encoding='utf-8-sig'))
    launch, meta = load(session / 'launch.json'), load(session / 'path-trace.json')
    policy = meta.get('auxiliary_evidence', {})
    if (launch.get('timing_profile') != 'auxiliary' or not launch.get('audio_muted') or
            launch.get('game_sha256') != '2d1ca79630d7bbe6fa29575f59aa7996d43071d74ac8e4f7d40137967d9039ab' or
            not meta.get('complete') or not meta.get('consumer_only') or
            meta.get('dropped') or meta.get('cutoff_publishers') or meta.get('io_failed') or
            not policy.get('enabled') or not 1 <= policy.get('context_limit', 0) <= 1024 or
            policy.get('packets_per_context') != 64):
        raise ValueError('Not complete, pinned, bounded auxiliary-only evidence')
    groups, sequences = defaultdict(list), {}
    count = 0
    with (session / 'path-trace.csv').open(encoding='utf-8-sig', newline='') as stream:
        reader = csv.DictReader(stream)
        if reader.fieldnames != FIELDS:
            raise ValueError('Unexpected trace header')
        for row in reader:
            if None in row or any(row.get(key) is None for key in FIELDS) or row['event'] not in KINDS:
                raise ValueError('Malformed or unrelated trace row')
            v = {key: int(row[key]) for key in FIELDS if key != 'event'}
            if any(not 0 <= value <= MASK for value in v.values()) or not v['id'] & HIGH or v['thread'] >= meta['registered_threads']:
                raise ValueError('Invalid auxiliary transport identity')
            prior_seq, prior_ns = sequences.get(v['thread'], (-1, 0))
            if v['seq'] != prior_seq + 1 or v['ns'] < prior_ns:
                raise ValueError('Auxiliary trace sequence/clock gap')
            sequences[v['thread']] = v['seq'], v['ns']
            groups[v['id']].append(Event(v['ns'], v['thread'], row['event'], *(v[k] for k in ('a', 'b', 'c', 'd'))))
            count += 1
    if count != meta['events'] or len(groups) != policy['contexts'] or len(groups) > policy['context_limit']:
        raise ValueError('Event/context totals disagree with native metadata')
    contexts = [decode_context(ident, events, 64) for ident, events in groups.items()]
    by_id = {c['context_id']: c for c in contexts}
    for context in contexts:
        parent = by_id.get(context['parent_context_id'])
        context['parent_observed'] = parent is not None
        if parent and (context['queue'] != parent['queue'] or not context['indirect'] or
                       not parent['begin_ns'] <= context['begin_ns'] <= context['end_ns'] <= parent['end_ns'] or
                       not any(p['kind'] == 'ib' and p['target'] == context['base'] and
                               p['target_bytes'] == context['bytes'] for p in parent['packets'])):
            raise ValueError('Child context disagrees with its actual parent IB')
    if sum(c['omitted_packets'] for c in contexts) != policy['omitted_packets']:
        raise ValueError('Omitted packet totals disagree')
    return {'schema': 1, 'session': launch['session'], 'runtime_sha256': launch['runtime_sha256'],
            'transport_complete': True, 'contexts': contexts,
            'packet_kinds': dict(Counter(p['kind'] for c in contexts for p in c['packets'])),
            'observed': bool(contexts), 'generation_proven': False, 'safe_to_remove_dependencies': False,
            'performance_acceptance': False,
            'limits': ['Selected compute command metadata only; other packet types and CPU consumption are not captured.',
                       'Packet caps, buffered ring fragments and unobserved parents limit coverage.',
                       'R14 template matches are candidates, not emitter identity or same-generation proofs.',
                       'Context completion is CPU decoder completion, not GPU completion or a rendered frame.']}


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--session', type=Path, required=True)
    args = parser.parse_args()
    session = project_child(args.session) if not args.session.is_absolute() else project_child(args.session.relative_to(Path(__file__).resolve().parents[1]))
    result = analyze(session)
    write_json(session / 'auxiliary-summary.json', result)
    print(json.dumps({'contexts': len(result['contexts']), 'observed': result['observed'],
                      'packet_kinds': result['packet_kinds'], 'generation_proven': False}))
