# SPDX-License-Identifier: GPL-2.0-or-later
import sys
from pathlib import Path
import unittest
import csv
import io
import json
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'scripts'))
from analyze_auxiliary_trace import analyze, decode_context, HIGH
from analyze_timing import Event, FIELDS


def event(ns, kind, a=0, b=0, c=0, d=0, thread=1):
    return Event(ns, thread, kind, a, b, c, d)


def packet(ns, kind, ordinal, address, words, buffered=0):
    values = words + [0] * (7 - len(words))
    return [event(ns, 'aux_packet', kind, ordinal, address, buffered),
            event(ns + 1, 'aux_words', values[0] | values[1] << 32,
                  values[2] | values[3] << 32, values[4] | values[5] << 32,
                  values[6] | len(words) << 32)]


def context(packets, count=1, omitted=0, finished=1, flags=1):
    return [event(1, 'aux_begin', 0, 0x1000, 256, 0), *packets,
            event(100, 'aux_end', count, omitted, finished, flags)]


class AuxiliaryAnalysisTests(unittest.TestCase):
    def test_unsigned_dma_and_candidate_are_not_generation_proof(self):
        data = packet(10, 1, 1, 0x1000, [0xc0055000, 0x04004000, 0x1234, 0x12, 0x5678, 0x34, 0x160])
        data += packet(20, 1, 2, 0x101c, [0xc0055000, 0x84004000, 0x9000, 0, 0xa000, 0, 0xa0])
        result = decode_context(HIGH + 1, context(data, 2), 64)
        self.assertEqual(result['packets'][0]['source'], '0x1200001234')
        self.assertEqual(result['packets'][0]['destination'], '0x3400005678')
        self.assertFalse(result['packets'][0]['cp_sync'])
        self.assertTrue(result['packets'][1]['cp_sync'])
        self.assertEqual(result['candidate_dma_pairs'], [{'ordinals': [1, 2], 'count': 2, 'semantic_identity_proven': False}])
        self.assertTrue(result['closed_metadata_context'])

    def test_release_and_ib_decode(self):
        data = packet(10, 2, 1, 0x1000, [0xc0054902, 0x62f, 0x23000000, 0x100, 0x12, 42, 0])
        data += packet(20, 3, 2, 0x101c, [0xc0023f00, 0x2000, 0, 16])
        result = decode_context(HIGH + 1, context(data, 2), 64)
        self.assertTrue(result['packets'][0]['matches_r14_release_template'])
        self.assertEqual(result['packets'][1]['target_bytes'], 64)

    def test_incomplete_contexts_are_retained_without_closed_claim(self):
        data = packet(10, 3, 1, 0x9000, [0xc0023f00, 0x2000, 0, 16], buffered=1)
        for kwargs in ({'omitted': 1}, {'finished': 0}, {'flags': 0}, {}):
            result = decode_context(HIGH + 1, context(data, **kwargs), 64)
            self.assertFalse(result['closed_metadata_context'])

    def test_reject_wrong_header_width_address_and_ordinal(self):
        for data in (packet(10, 3, 1, 0x1000, [0xc0033f00, 0x2000, 0, 16]),
                     packet(10, 3, 1, 0x2000, [0xc0023f00, 0x2000, 0, 16]),
                     packet(10, 3, 2, 0x1000, [0xc0023f00, 0x2000, 0, 16]),
                     packet(10, 3, 1, 0x1000, [0xc0023f00, 0x2000, 0, 16])[:1]):
            with self.assertRaises(ValueError): decode_context(HIGH + 1, context(data), 64)

    def test_reject_identity_thread_and_budget_mismatch(self):
        data = context(packet(10, 3, 1, 0x1000, [0xc0023f00, 0x2000, 0, 16]))
        for ident, limit in ((1, 64), (HIGH + 1, 0)):
            with self.assertRaises(ValueError): decode_context(ident, data, limit)
        data[1] = event(10, 'aux_packet', 3, 1, 0x1000, thread=2)
        with self.assertRaises(ValueError): decode_context(HIGH + 1, data, 64)


class MemorySession:
    def __init__(self, files, name=''): self.files, self.name = files, name
    def __truediv__(self, name): return MemorySession(self.files, name)
    def read_text(self, **kwargs): return self.files[self.name]
    def open(self, **kwargs): return io.StringIO(self.files[self.name])


def transport():
    parent, child = HIGH + 1, HIGH + 2
    events = [(parent, event(1, 'aux_begin', 0, 0x1000, 256)),
              *[(parent, e) for e in packet(2, 3, 1, 0x1000, [0xc0023f00, 0x2000, 0, 16])],
              (child, event(10, 'aux_begin', 0, 0x2000, 64, parent)),
              (child, event(50, 'aux_end', 0, 0, 1, 3)),
              (parent, event(100, 'aux_end', 1, 0, 1, 1))]
    output = io.StringIO(); writer = csv.DictWriter(output, FIELDS); writer.writeheader()
    for seq, (ident, e) in enumerate(events):
        writer.writerow(dict(ns=e.ns, thread=e.thread, seq=seq, guest_frame=0, event=e.kind, id=ident,
                             a=e.a, b=e.b, c=e.c, d=e.d))
    launch = {'session': 'memory', 'runtime_sha256': 'a' * 64, 'timing_profile': 'auxiliary',
              'audio_muted': True, 'game_sha256': '2d1ca79630d7bbe6fa29575f59aa7996d43071d74ac8e4f7d40137967d9039ab'}
    meta = {'complete': True, 'consumer_only': True, 'registered_threads': 2, 'events': 6,
            'auxiliary_evidence': {'enabled': True, 'context_limit': 16, 'contexts': 2,
                                   'packets_per_context': 64, 'omitted_packets': 0}}
    return {'launch.json': json.dumps(launch), 'path-trace.json': json.dumps(meta),
            'path-trace.csv': output.getvalue()}


class AuxiliaryTransportTests(unittest.TestCase):
    def test_actual_parent_ib_link_without_generation_claim(self):
        result = analyze(MemorySession(transport()))
        self.assertTrue(result['transport_complete'])
        self.assertTrue(result['contexts'][1]['parent_observed'])
        self.assertFalse(result['generation_proven'])
        self.assertFalse(result['safe_to_remove_dependencies'])

    def test_wrong_child_binding_rejected(self):
        files = transport()
        files['path-trace.csv'] = files['path-trace.csv'].replace(',8192,64,', ',12288,64,')
        with self.assertRaises(ValueError): analyze(MemorySession(files))

    def test_dropped_transport_and_sequence_gap_rejected(self):
        for field in ('dropped', 'cutoff_publishers', 'io_failed'):
            files = transport(); meta = json.loads(files['path-trace.json']); meta[field] = 1
            files['path-trace.json'] = json.dumps(meta)
            with self.assertRaises(ValueError): analyze(MemorySession(files))
        files = transport()
        files['path-trace.csv'] = files['path-trace.csv'].replace('1,2,3,0,aux_words,', '1,3,3,0,aux_words,')
        with self.assertRaises(ValueError): analyze(MemorySession(files))

    def test_empty_budget_observation_is_not_a_positive_result(self):
        files = transport(); meta = json.loads(files['path-trace.json'])
        meta['events'] = meta['auxiliary_evidence']['contexts'] = 0
        files['path-trace.json'] = json.dumps(meta)
        files['path-trace.csv'] = files['path-trace.csv'].splitlines()[0] + '\n'
        result = analyze(MemorySession(files))
        self.assertFalse(result['observed'])
        self.assertFalse(result['performance_acceptance'])


if __name__ == '__main__': unittest.main()
