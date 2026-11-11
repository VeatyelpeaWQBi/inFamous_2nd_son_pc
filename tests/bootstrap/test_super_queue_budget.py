"""Check attribution against small independent timelines, including a nonqualifying write."""
import csv
import json
from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'scripts'))
from analyze_super_queue_budget import analyze


class QueueBudgetTests(unittest.TestCase):
    def setUp(self):
        self.area = tempfile.TemporaryDirectory()
        self.addCleanup(self.area.cleanup)
        self.root = Path(self.area.name)
        self.rows = []
        self.meta = dict(complete=True, dropped=0, io_failed=False, cutoff_publishers=0,
                         start=dict(ns_before=10**9, ns_after=10**9, qpc=0, frequency=10**7))
        (self.root / 'super-rounds.json').write_text('[{"release":{"qpc_after":0}}]')
        self.add('particle_label_write', 1, 4_999_000_000, 100, 0, 0, 0)
        self.add('flip_caller_sample', 20, 5_000_000_000, (2 << 32) | 7, 0, 0, 100)
        self.add('flip_caller_sample', 21, 5_000_100_000, (2 << 32) | 7, 0, 0, 100)
        self.add('particle_label_write', 2, 5_001_000_000, 100, 1, 0, 0)
        self.add('particle_label_write', 3, 5_003_000_000, 100, 2, 0, 0)
        self.add('particle_label_schedule', 3, 5_003_000_100,
                 4_999_900_000, 5_002_000_000, 5_002_900_000, 0)
        self.add('cp_queue_slice', 30, 5_001_500_000, 5_000_500_000, 5_001_500_000, 1, 1)
        self.add('cp_asc_slice_work', 30, 5_001_500_100, 600_000, 20_000, 10, 0)
        self.add('cp_asc_dispatch_phases', 30, 5_001_500_200, 100_000, 200_000, 200_000, 50_000)
        self.add('cp_asc_binding_phases', 30, 5_001_500_300, 100_000, 50_000, 10, 0)
        self.add('cp_queue_slice', 31, 5_000_400_000, 5_000_000_000, 5_000_400_000, 0, 1)

    def add(self, kind, ident, stamp, a, b, c, d):
        self.rows.append(dict(event=kind, id=ident, ns=stamp, a=a, b=b, c=c, d=d))

    def save(self):
        self.meta['events'] = len(self.rows)
        (self.root / 'path-trace.json').write_text(json.dumps(self.meta))
        with (self.root / 'path-trace.csv').open('w', newline='') as f:
            writer = csv.DictWriter(f, fieldnames=self.rows[0].keys())
            writer.writeheader()
            writer.writerows(self.rows)

    def test_threshold_queue_mapping_and_nested_budget(self):
        self.save()
        result = analyze(self.root)
        peak = result['sampled_wait_windows']['3:8']
        self.assertEqual(peak['pending_sampled_conditions'], 1)
        self.assertEqual(peak['mean_ms']['remaining_queue_wait'], 2)
        self.assertEqual(peak['mean_ms']['full_slice'], 1)
        self.assertEqual(peak['mean_ms']['resources'], .2)
        self.assertEqual(peak['mean_ms']['buffers'], .1)
        self.assertEqual(result['queue_work_windows']['3:8']['raster_dispatch_calls'], 10)

    def test_invalid_nesting_is_rejected(self):
        next(r for r in self.rows if r['event'] == 'cp_asc_binding_phases')['a'] = 300_000
        self.save()
        with self.assertRaisesRegex(ValueError, 'nesting'):
            analyze(self.root)

    def test_missing_phase_does_not_become_zero(self):
        self.rows = [r for r in self.rows if r['event'] != 'cp_asc_dispatch_phases']
        self.save()
        with self.assertRaisesRegex(ValueError, 'Missing joined'):
            analyze(self.root)


if __name__ == '__main__':
    unittest.main()
