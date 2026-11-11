"""Offline integrity and nested-budget tests; no game, profiler or files generated."""
import copy
from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "scripts"))
from analyze_draw_prep import (decode_sample, summarize, KINDS, KINDS_V2,
                               decode_sample_v2, summarize_v2, sampling_target)


class DrawPrepChecks(unittest.TestCase):
    def setUp(self):
        payloads = [(1000000, 1000036, 0, (256 << 32) | 1), (1, 2, 3, 4), (5, 6, 7, 8),
                    (2, 1, 9, 4), (1, 0, 0, 2), (0xabc, 0xdef, 12 << 32 | 1, 1)]
        self.events = [dict(event=kind, thread=2, id=10, ns=1000100+i,
                            **dict(zip("abcd", values)))
                       for i, (kind, values) in enumerate(zip(KINDS, payloads))]

    def test_phases_partition_total_and_nested_resources_are_separate(self):
        sample = decode_sample(self.events)
        self.assertEqual(sample['total_ns'], 36)
        self.assertEqual(sum(sample['phases'].values()), 36)
        result = summarize([sample], 10)
        self.assertAlmostEqual(result['estimated_direct_draw_cpu_ms_per_eop'], 36*256/10/1e6)
        self.assertAlmostEqual(sum(result['estimated_phase_cpu_ms_per_eop'].values()),
                               result['estimated_direct_draw_cpu_ms_per_eop'])
        self.assertEqual(result['recorded_samples'], 1)

    def test_missing_or_duplicate_event_is_not_a_zero_cost_sample(self):
        for events in (self.events[:-1], self.events + self.events[-1:]):
            with self.assertRaises(ValueError):
                decode_sample(events)

    def test_bad_clock_partition_thread_weight_and_unsigned_fields_are_rejected(self):
        for row, key, value in ((0, 'b', 1000200), (1, 'a', 2), (2, 'thread', 3),
                                (0, 'd', (64 << 32) | 1), (4, 'a', -1), (1, 'id', 0)):
            events = copy.deepcopy(self.events)
            events[row][key] = value
            with self.assertRaises(ValueError):
                decode_sample(events)

    def test_nested_times_cannot_exceed_their_parent(self):
        for row, key, value in ((3, 'a', 5), (4, 'd', 3), (4, 'a', 3)):
            events = copy.deepcopy(self.events)
            events[row][key] = value
            with self.assertRaises(ValueError):
                decode_sample(events)

    def test_empty_sampling_window_is_not_reported_as_zero_work(self):
        with self.assertRaises(ValueError):
            summarize([], 100)


class SinglePhaseChecks(unittest.TestCase):
    @staticmethod
    def events(block, reached=True):
        selected, target = sampling_target(block * 256)
        begin, end = 1000000 + block * 1000, 1000100 + block * 1000
        a, b = (begin, end) if target == 8 else (begin + 20, begin + 30)
        if not reached:
            a = b = end
        payloads = [(begin, end, block * 256 + selected, (256 << 32) | 256 | target),
                    (a, b, target, int(reached)), (0xabc, 0xdef, (12 << 32) | 1, 1)]
        return [dict(event=kind, thread=2, id=block+1, ns=end+10+i,
                     **dict(zip('abcd', values)))
                for i, (kind, values) in enumerate(zip(KINDS_V2, payloads))]

    def test_all_targets_covered_once_per_group_including_wrapping_u64(self):
        for group in (0, 1, 100, 6550):
            rows = [decode_sample_v2(self.events(group * 9 + i)) for i in range(9)]
            self.assertEqual({s['target'] for s in rows}, set(range(9)))
            self.assertEqual(len({s['ordinal'] // 256 for s in rows}), 9)

    def test_estimates_are_independent_of_whole_control(self):
        rows = [decode_sample_v2(self.events(i)) for i in range(9)]
        result = summarize_v2(rows, 10)
        self.assertAlmostEqual(sum(result['estimated_phase_cpu_ms_per_eop'].values()), 80*2304/10/1e6)
        self.assertAlmostEqual(result['estimated_direct_draw_cpu_ms_per_eop'], 100*2304/10/1e6)
        self.assertAlmostEqual(result['phase_sum_to_whole_control_ratio'], .8)
        self.assertEqual(result['balanced_nine_block_groups'], 1)

    def test_unreached_phase_is_zero_but_missing_is_an_error(self):
        sample = decode_sample_v2(self.events(0, reached=False))
        self.assertFalse(sample['reached'])
        self.assertEqual(sample['measured_ns'], 0)
        with self.assertRaises(ValueError):
            decode_sample_v2(self.events(0)[:-1])
        with self.assertRaises(ValueError):
            summarize_v2([sample], 10)

    def test_invalid_schedule_bounds_control_and_unsigned_fields(self):
        base = self.events(0)
        for row, key, value in ((0, 'c', 1), (0, 'd', (256 << 32) | 257),
                                (1, 'b', 1000101), (1, 'd', 0),
                                (2, 'thread', 3), (1, 'a', -1)):
            rows = copy.deepcopy(base)
            rows[row][key] = value
            with self.assertRaises(ValueError):
                decode_sample_v2(rows)
        control = next(self.events(i) for i in range(9) if sampling_target(i*256)[1] == 8)
        control[1]['a'] += 1
        with self.assertRaises(ValueError):
            decode_sample_v2(control)


if __name__ == '__main__':
    unittest.main()
