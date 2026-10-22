# SPDX-License-Identifier: GPL-2.0-or-later
"""Pure in-memory regression; no game, disk fixtures or third-party dependencies."""
import sys
from pathlib import Path
import unittest
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'scripts'))
from analyze_timing import Event, flip_path, read_path, stats


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

    def test_percentiles_are_nearest_rank(self):
        values = list(range(1, 101))
        result = stats([value * 1_000_000 for value in values])
        self.assertEqual(result['p95_ms'], 95)
        self.assertEqual(result['p99_ms'], 99)
        self.assertEqual(result['mean_ms'], 50.5)


if __name__ == '__main__':
    unittest.main()
