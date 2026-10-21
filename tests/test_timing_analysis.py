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
                            descriptor_update_invariants, descriptor_summary, depth_spirv_facts,
                            lighting_cse_summary, feedback_submit_opportunities,
                            summarize_submit_opportunities, compute_sample_summary)


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
    def test_completion_elision_requires_exact_published_range_and_finished_wait(self):
        events = chain()[:5] + [Event(45, 1, 'read_completion_published', 4096, 8),
                               Event(52, 0, 'read_completion_elided', 0, 4096, 8),
                               Event(80, 0, 'read_return', 1)]
        row = read_path(events)
        self.assertEqual(row['completion_elided'], 1)
        self.assertNotIn('completion_cp_ns', row)
        for replacement in (Event(45, 1, 'read_completion_published', 4097, 8),
                            Event(53, 1, 'read_completion_published', 4096, 8)):
            bad = [replacement if e.kind == 'read_completion_published' else e for e in events]
            with self.assertRaises(ValueError):
                read_path(bad)
        with self.assertRaises(ValueError):
            read_path([e for e in events if e.kind != 'read_wait_end'])
        with self.assertRaises(ValueError):
            read_path([e for e in events if e.kind != 'read_completion_published'])

    def test_completion_elision_rejects_writes_wrong_thread_and_duplicate_completion(self):
        events = chain()[:5] + [Event(45, 1, 'read_completion_published', 4096, 8),
                               Event(52, 0, 'read_completion_elided', 0, 4096, 8),
                               Event(80, 0, 'read_return', 1)]
        for bad in ([Event(0, 0, 'read_request', 4096, 8, 123, 1)] + events[1:],
                    [Event(52, 3, 'read_completion_elided', 0, 4096, 8)
                     if e.kind == 'read_completion_elided' else e for e in events],
                    events + [Event(53, 0, 'read_completion_request', 0)]):
            with self.assertRaises(ValueError):
                read_path(bad)

    def test_exact_compute_reports_use_counts_not_rounded_rates(self):
        log = ('Perf GPU compute samples 00000000ce1675e0: count=2 total_ns=1000000\n'
               'Perf GPU compute samples 00000000ce1675e0: count=1 total_ns=8000000\n'
               'GPU compute profile sample budget exhausted: shader=00000000ce1675e0 issued=3\n')
        with patch.object(Path, 'open', return_value=io.StringIO(log)):
            row = compute_sample_summary(Path('session'), single_profiler=True)
        self.assertEqual(row['shaders'][0]['mean_ms'], 3)
        self.assertEqual(row['shaders'][0]['reports'], 2)
        self.assertTrue(row['all_issued_samples_reported'])
        self.assertFalse(row['performance_acceptance'])

    def test_compute_reports_do_not_conflate_multiple_profiler_budgets(self):
        log = ('Perf GPU compute samples 00000000ce1675e0: count=2 total_ns=1000000\n'
               'GPU compute profile sample budget exhausted: shader=00000000ce1675e0 issued=2\n')
        with patch.object(Path, 'open', return_value=io.StringIO(log)):
            row = compute_sample_summary(Path('session'))
        self.assertIsNone(row['resolved_issued_budget'])
        self.assertIsNone(row['all_issued_samples_reported'])
        with patch.object(Path, 'open', return_value=io.StringIO(log * 2)):
            row = compute_sample_summary(Path('session'), single_profiler=True)
        self.assertEqual(row['reported_samples'], 4)
        self.assertIsNone(row['resolved_issued_budget'])
        self.assertIsNone(row['all_issued_samples_reported'])

    def test_compute_reports_reject_malformed_or_over_budget_counts(self):
        for sample in ('count=0 total_ns=0', 'count=broken total_ns=1', 'count=3 total_ns=1'):
            log = ('Perf GPU compute samples 00000000ce1675e0: ' + sample + '\n'
                   'GPU compute profile sample budget exhausted: shader=00000000ce1675e0 issued=2\n')
            with patch.object(Path, 'open', return_value=io.StringIO(log)):
                row = compute_sample_summary(Path('session'), single_profiler=True)
            self.assertTrue(row['issues'])
            self.assertFalse(row['all_issued_samples_reported'])

    def test_lighting_cse_is_not_binary_or_performance_acceptance(self):
        log = ('C1 lighting CSE: requested=true (startup policy)\n'
               'C1 lighting CSE shader 0xce1675e0: pinned=true removed=8 '
               'cross_block=5 floating=2 budget_reached=false\n')
        with patch.object(Path, 'open', return_value=io.StringIO(log)), \
                patch.object(Path, 'glob', return_value=[]):
            summary = lighting_cse_summary(Path('session'))
        self.assertEqual(summary['mechanism_status'], 'reported-ir-reuse')
        self.assertEqual(summary['transformations'][0]['removed'], 8)
        self.assertFalse(summary['performance_acceptance'])
        self.assertEqual(summary['final_binaries'], [])
        self.assertEqual(summary['issues'], [])

    def test_lighting_cse_rejects_inconsistent_or_malformed_evidence(self):
        for row in ('pinned=false removed=1 cross_block=0 floating=0 budget_reached=false',
                    'pinned=true removed=1 cross_block=2 floating=0 budget_reached=false',
                    'pinned=true removed=1 cross_block=0 floating=2 budget_reached=false',
                    'pinned=true removed=broken'):
            with patch.object(Path, 'open', return_value=io.StringIO(
                    'C1 lighting CSE shader 0xce1675e0: ' + row)), \
                    patch.object(Path, 'glob', return_value=[]):
                summary = lighting_cse_summary(Path('session'))
                self.assertTrue(summary['issues'])
                self.assertEqual(summary['mechanism_status'], 'invalid-evidence')

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

    def test_graphics_wait_partition_clips_driver_intervals(self):
        for begin, end, expected in ((25, 30, (4, 5, 20)), (10, 20, (0, 0, 29)),
                                      (10, 60, (0, 29, 0)), (60, 70, (29, 0, 0))):
            events = self.producer_events()
            for event in events:
                if event.kind in ('read_wait_begin', 'read_wait_end'):
                    event.b, event.c = 0, 7
            groups = {9: events, 2: [Event(begin, 2, 'submit_driver_begin', 111, 7, 1),
                                      Event(end, 2, 'submit_driver_end', 111, 7, 1, 1)]}
            summary = feedback_summary(groups, [{'request_id': 9, **read_path(events)}])
            parts = summary['graphics_wait_partition']
            self.assertEqual(summary['counts']['partitioned_graphics_waits'], 1)
            for name, ns in zip(('before_driver_ns', 'during_driver_ns', 'after_driver_ns'), expected):
                self.assertEqual(parts[name]['mean_ms'], ns / 1e6)
            self.assertEqual(sum(expected), 29)

    def test_readback_queue_wait_is_not_joined_by_tick_alone(self):
        groups = {9: self.producer_events(),
                  2: [Event(25, 2, 'submit_driver_begin', 111, 99, 1),
                      Event(30, 2, 'submit_driver_end', 111, 99, 1, 1)]}
        summary = feedback_summary(groups, [{'request_id': 9, **read_path(groups[9])}])
        self.assertEqual(summary['graphics_wait_partition'], {})
        self.assertEqual(summary['counts']['waits_without_resolved_graphics_domain'], 1)

    def opportunity_events(self):
        events = self.producer_events() + [Event(15, 1, 'read_dependencies', 7, 0)]
        for event in events:
            if event.kind in ('read_wait_begin', 'read_wait_end'):
                event.b, event.c = 0, 7
        return events

    def opportunities(self, events, domain=111, tick=7):
        submissions, _ = submission_paths({1: [Event(20, 1, 'submit_queued', domain, tick, 0, 1)]})
        return feedback_submit_opportunities(events, producer_path(events), submissions)

    def test_submit_opportunity_preserves_request_and_window_scope(self):
        for scope in (2, 3):
            events = self.opportunity_events()
            next(e for e in events if e.kind == 'read_producer').d = (3 << 8) | (scope << 16)
            rows = self.opportunities(events)
            self.assertEqual(len(rows), 1)
            self.assertEqual(rows[0]['scopes'], scope)
            self.assertEqual(rows[0]['recorded_to_enqueue_cpu_ns'], 18)
            self.assertEqual(rows[0]['request_to_enqueue_cpu_ns'], 20)
            self.assertEqual(rows[0]['request_before_recorded_cpu_ns'], 2)
            self.assertEqual(rows[0]['cpu_wait_ns'], 29)
            self.assertTrue(rows[0]['beyond_submitted_snapshot'])
        next(e for e in events if e.kind == 'read_request').ns = 3
        row = self.opportunities(events)[0]
        self.assertEqual(row['recorded_before_request_cpu_ns'], 1)
        self.assertNotIn('request_before_recorded_cpu_ns', row)

    def test_submit_opportunity_requires_exact_queue_domain_and_dependency_tick(self):
        self.assertEqual(self.opportunities(self.opportunity_events(), domain=222), [])
        self.assertEqual(self.opportunities(self.opportunity_events(), tick=8), [])
        for field, value in (('read_dependencies', 8), ('read_wait_begin', 8)):
            events = self.opportunity_events()
            event = next(e for e in events if e.kind == field)
            if field == 'read_dependencies': event.a = value
            else: event.c = value
            self.assertEqual(self.opportunities(events), [])
        events = self.opportunity_events()
        for event in events:
            if event.kind in ('read_wait_begin', 'read_wait_end'): event.b = 1
        self.assertEqual(self.opportunities(events), [])

    def test_submit_opportunity_excludes_reused_and_ambiguous_retry_copies(self):
        events = [e for e in self.opportunity_events() if e.kind != 'read_dependencies']
        self.assertEqual(self.opportunities(events), [])
        events += [Event(15, 1, 'read_dependencies', 7, 0),
                   Event(16, 1, 'read_dependencies', 7, 0)]
        self.assertEqual(self.opportunities(events), [])

    def test_submit_opportunity_does_not_clamp_recorder_race_into_headroom(self):
        events = self.opportunity_events()
        # Producer Commit's CPU timestamp can follow enqueue; it is not GPU completion.
        next(e for e in events if e.kind == 'read_producer_time').c = 11
        submissions, _ = submission_paths({1: [Event(5, 1, 'submit_queued', 111, 7, 0, 1)]})
        rows = feedback_submit_opportunities(events, producer_path(events), submissions)
        self.assertTrue(rows[0]['recorded_after_enqueue'])
        self.assertNotIn('recorded_to_enqueue_cpu_ns', rows[0])
        summary = summarize_submit_opportunities(rows * 2)[0]
        self.assertEqual(summary['matched_read_candidates'], 2)
        self.assertEqual(summary['distinct_declared_commands'], 1)
        self.assertEqual(summary['intervals_not_additive']['recorded_to_enqueue_cpu_ns']['count'], 0)

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
