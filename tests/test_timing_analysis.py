# SPDX-License-Identifier: GPL-2.0-or-later
"""Pure in-memory regression; no game, disk fixtures or third-party dependencies."""
import sys
import io
import struct
from unittest.mock import patch
from pathlib import Path
import unittest
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'scripts'))
from analyze_timing import (Event, flip_path, read_path, stats, host_phase_path,
                            producer_path, submission_paths, feedback_summary, descriptor_line,
                            descriptor_invariants, frame_summary, descriptor_update_line,
                            descriptor_update_invariants, descriptor_summary, depth_spirv_facts)


def chain():
    return [Event(0, 0, 'read_request', 4096, 8, 123),
            Event(10, 1, 'read_cp_begin'), Event(20, 1, 'read_cp_end'),
            Event(21, 0, 'read_wait_begin', 0, 1, 99),
            Event(50, 0, 'read_wait_end', 0, 1, 99),
            Event(52, 0, 'read_completion_request', 0),
            Event(62, 1, 'read_cp_begin', 1, 0),
            Event(72, 1, 'read_cp_end', 1, 0),
            Event(80, 0, 'read_return', 1)]


class TimingAnalysisTests(unittest.TestCase):
    def test_host_phase_ordering_and_identity(self):
        begin = Event(10, 2, 'host_phase_begin', 2)
        end = Event(35, 2, 'host_phase_end', 2)
        self.assertEqual(host_phase_path([end, begin]), ('present_mutex', 25))
        with self.assertRaises(ValueError):
            host_phase_path([begin, Event(35, 3, 'host_phase_end', 2)])
        with self.assertRaises(ValueError):
            host_phase_path([begin, Event(35, 2, 'host_phase_end', 3)])
        with self.assertRaises(ValueError):
            host_phase_path([begin, Event(5, 2, 'host_phase_end', 2)])
        with self.assertRaises(ValueError):
            host_phase_path([begin])

    def test_disjoint_stages_and_residual(self):
        row = read_path(list(reversed(chain())))  # CSV is intentionally NOT globally sorted.
        self.assertEqual(row['total_ns'], 80)
        self.assertEqual(row['cpu_wait_ns'], 29)
        self.assertEqual(row['completion_queue_ns'], 10)
        self.assertEqual(row['handoff_and_unattributed_ns'], 11)
        self.assertEqual(sum(value for key, value in row.items() if key.endswith('_ns') and key != 'total_ns'), 80)

    def test_nested_fallback_never_added_twice(self):
        events = chain() + [Event(64, 1, 'read_fallback_begin'), Event(70, 1, 'read_fallback_end')]
        row = read_path(events)
        self.assertEqual(row['nested_fallback'], 6)
        self.assertEqual(row['completion_cp_ns'], 10)
        self.assertEqual(row['total_ns'], 80)

    def test_wrong_semaphore_identity_rejected(self):
        events = chain()
        events[4].c = 100
        with self.assertRaises(ValueError): read_path(events)

    def test_missing_completion_request_rejected(self):
        with self.assertRaises(ValueError): read_path([e for e in chain() if e.kind != 'read_completion_request'])

    def test_cpu_return_is_not_allowed_before_cp_completion(self):
        events = chain()
        events[-1].ns = 70
        with self.assertRaises(ValueError): read_path(events)

    def test_wrong_mode_and_repeated_initial_phase_rejected(self):
        events = chain()
        events[-1].a = 2
        with self.assertRaises(ValueError): read_path(events)
        with self.assertRaises(ValueError): read_path(chain() + [Event(5, 1, 'read_cp_begin')])

    def test_present_queue_separated_from_present_call(self):
        names = ('flip_request', 'flip_prepare', 'flip_ready', 'flip_consume', 'flip_present_return', 'flip_done')
        row = flip_path([Event(ns, 0, name) for ns, name in zip((0, 10, 40, 100, 130, 150), names)])
        self.assertEqual(row['ready_queue_ns'], 60)
        self.assertEqual(row['present_call_cpu_ns'], 30)
        self.assertEqual(row['total_ns'], 150)

    def producer_events(self):
        return chain() + [Event(11,1,'read_producer_window',4096,64,4096,8),
            Event(11,1,'read_producer_coverage',1,5,0,4),
            Event(12,1,'read_producer',5,99,0xabc,(3<<8)|(3<<16)),
            Event(12,1,'read_producer_range',5,4096,8,7),
            Event(12,1,'read_producer_time',5,1,2,99),
            Event(12,1,'read_producer_domain',5,111,6,5)]

    def test_declared_producer_identity_and_missing_parts(self):
        events = self.producer_events()
        row = producer_path(events)
        self.assertEqual(row['candidates'][0]['command'],99)
        self.assertEqual(row['candidates'][0]['scopes'],3)
        with self.assertRaises(ValueError):
            producer_path([e for e in events if e.kind!='read_producer_domain'])
        events[-2].d=100
        with self.assertRaises(ValueError): producer_path(events)

    def test_producer_metadata_must_belong_to_initial_cp_phase(self):
        events=self.producer_events()
        events[-1].thread=3
        with self.assertRaises(ValueError): producer_path(events)
        events=self.producer_events()
        events[-1].ns=22
        with self.assertRaises(ValueError): producer_path(events)

    def test_history_eviction_and_no_retained_overlap_are_evidence_limits(self):
        events = [e for e in self.producer_events() if e.kind not in
                  ('read_producer','read_producer_range','read_producer_time','read_producer_domain')]
        next(e for e in events if e.kind=='read_producer_coverage').c=999
        next(e for e in events if e.kind=='read_producer_coverage').d=5
        row=producer_path(events)
        self.assertTrue(row['blind_spot'])
        self.assertEqual(row['candidates'],[])

    def test_nonoverlap_or_duplicate_queue_range_cannot_claim_a_producer(self):
        events=self.producer_events()
        next(e for e in events if e.kind=='read_producer_range').b=8192
        with self.assertRaises(ValueError): producer_path(events)
        events=self.producer_events()
        duplicates=[Event(e.ns,e.thread,e.kind,4,e.b,e.c,e.d) for e in events
                    if e.kind in ('read_producer','read_producer_range','read_producer_time','read_producer_domain')]
        with self.assertRaises(ValueError): producer_path(events+duplicates)

    def test_submit_domains_are_distinct_and_return_is_not_gpu_completion(self):
        groups={1:[Event(20,1,'submit_queued',111,7,0,1)],
                2:[Event(25,2,'submit_driver_begin',111,7,1),
                   Event(30,2,'submit_driver_end',111,7,1,1)],
                3:[Event(40,3,'submit_driver_end',222,7,1,1)]}
        rows,partial=submission_paths(groups)
        self.assertEqual(len(rows),2)
        self.assertEqual(partial['driver_pair'],1)
        groups[9]=self.producer_events()
        row=feedback_summary(groups,[{'request_id':9,'mode':1,'total_ns':80,'cpu_wait_ns':29}])
        self.assertEqual(row['counts']['linked_enqueue'],1)
        self.assertEqual(row['counts']['beyond_submitted_snapshot'],1)
        self.assertEqual(row['cpu_intervals_not_additive']['driver_call_cpu_ns']['mean_ms'],5/1e6)
        groups[2][-1].d=0
        with self.assertRaises(ValueError): submission_paths(groups)

    def test_empty_timeline_signal_submission_is_legal(self):
        groups={1:[Event(20,1,'submit_driver_begin',111,7,0),
                   Event(25,1,'submit_driver_end',111,7,0,1)]}
        rows,_=submission_paths(groups)
        self.assertEqual(len(rows),1)

    def test_submit_pair_must_share_request_identity(self):
        groups={1:[Event(20,1,'submit_driver_begin',111,7,1)],
                2:[Event(25,1,'submit_driver_end',111,7,1,1)]}
        with self.assertRaises(ValueError): submission_paths(groups)

    def test_descriptor_counter_invariants(self):
        line=('Perf R4 descriptors: calls 10 hit 2 unknown 1 uncacheable 0 layout 3 '
              'shape 1 payload 3 layout-only 2; changed segments 4/12 infos 6 '
              'one/few/many 2/1/0; fields buffer 1 offset 4 range 2 sampler 0 '
              'view 0 image-layout 0 texel-view 0')
        row=descriptor_line(line)
        self.assertEqual(row['changed_segments'],4)
        self.assertFalse(any(descriptor_invariants(row).values()))
        higher=descriptor_line(line.replace('calls 10','calls 11'))
        lower=descriptor_line(line.replace('calls 10','calls 9'))
        self.assertEqual(descriptor_invariants(higher)['calls_minus_reasons'],1)
        self.assertEqual(descriptor_invariants(lower)['calls_minus_reasons'],-1)
        self.assertEqual(higher['calls']+lower['calls'],2*row['calls'])
        with self.assertRaises(ValueError): descriptor_line(line.replace('calls 10','calls broken'))

    def test_c1_final_binary_evidence_does_not_infer_fps(self):
        words = [0x07230203, 0x00010500, 0, 100, 0,
                 (2 << 16) | 17, 63,
                 (6 << 16) | 353, 3, 20, 7, 0, 8,
                 (6 << 16) | 356, 3, 21, 7, 0, 8,
                 (4 << 16) | 333, 2, 22, 7,
                 (7 << 16) | 236, 3, 23, 9, 7, 7, 8,
                 (7 << 16) | 238, 3, 24, 9, 7, 7, 8,
                 (4 << 16) | 224, 7, 7, 7]
        facts = depth_spirv_facts(struct.pack('<' + 'I' * len(words), *words))
        self.assertTrue(facts['candidate_pattern_observed'])
        self.assertEqual(facts['operations']['control_barrier'], 1)
        self.assertNotIn('fps', facts)
        words[5] = 0
        with self.assertRaises(ValueError):
            depth_spirv_facts(struct.pack('<' + 'I' * len(words), *words))
        with self.assertRaises(ValueError):
            depth_spirv_facts(b'broken')

    def test_descriptor_update_reports_preserve_snapshot_residuals(self):
        line = ('Perf A1 descriptors: calls 5 full 1 partial 3 skipped 1; '
                'write segments input 20 emitted 8 omitted 12 partial-omitted 8')
        row = descriptor_update_line(line)
        self.assertEqual(row['partial'], 3)
        self.assertFalse(any(descriptor_update_invariants(row).values()))
        skew = descriptor_update_line(line.replace('calls 5', 'calls 6'))
        self.assertEqual(descriptor_update_invariants(skew)['calls_minus_modes'], 1)
        with self.assertRaises(ValueError):
            descriptor_update_line(line.replace('emitted 8', 'emitted broken'))

    def test_a1_reports_required_only_for_a1_sessions(self):
        evidence = ('Perf R4 descriptors: calls 1 hit 0 unknown 1 uncacheable 0 layout 0 '
                    'shape 0 payload 0 layout-only 0; changed segments 0/0 infos 0 '
                    'one/few/many 0/0/0; fields buffer 0 offset 0 range 0 sampler 0 '
                    'view 0 image-layout 0 texel-view 0\n'
                    'Perf R4 prepare: calls 1 ms 1.000; buffers 1 ms 0.500; '
                    'textures 1 ms 0.250; regular sets 0 writes 0\n')
        with patch.object(Path, 'open', return_value=io.StringIO(evidence)):
            self.assertIsNone(descriptor_summary(Path('unused'))['updates'])
        with patch.object(Path, 'open', return_value=io.StringIO(evidence)):
            with self.assertRaisesRegex(ValueError, 'A1 descriptor update reports absent'):
                descriptor_summary(Path('unused'), require_updates=True)
        updates = ('Perf A1 descriptors: calls 3 full 1 partial 1 skipped 0; '
                   'write segments input 8 emitted 5 omitted 3 partial-omitted 3\n'
                   'Perf A1 descriptors: calls 1 full 0 partial 1 skipped 1; '
                   'write segments input 8 emitted 1 omitted 7 partial-omitted 3\n')
        with patch.object(Path, 'open', return_value=io.StringIO(evidence + updates)):
            result = descriptor_summary(Path('unused'), require_updates=True)['updates']
        self.assertTrue(result['aggregate_invariants_met'])
        self.assertEqual(len(result['snapshot_skew_intervals']), 2)
        self.assertEqual(result['counts']['partial'], 2)

    def test_frame_crossing_start_must_not_inflate_selected_duration(self):
        text=('frame,elapsed_ns,interval_ns\n0,0,0\n1,11000000000,11000000000\n'
              '2,11500000000,500000000\n3,11900000000,400000000\n')
        with patch.object(Path,'open',return_value=io.StringIO(text)):
            summary,rows=frame_summary(Path('unused'),10,2)
        self.assertEqual(set(rows),{2,3})
        self.assertEqual(summary['intervals_crossing_start_excluded'],1)
        self.assertAlmostEqual(summary['elapsed_s'],0.9)

    def test_percentiles_are_nearest_rank(self):
        values = list(range(1, 101))
        result = stats([value * 1_000_000 for value in values])
        self.assertEqual(result['p95_ms'], 95)
        self.assertEqual(result['p99_ms'], 99)
        self.assertEqual(result['mean_ms'], 50.5)


if __name__ == '__main__':
    unittest.main()
