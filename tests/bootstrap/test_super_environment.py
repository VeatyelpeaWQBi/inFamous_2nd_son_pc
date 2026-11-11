"""External-load evidence integrity and unknown/busy distinction, without launching anything."""
import copy
from pathlib import Path
import sys
import unittest
sys.path.insert(0, str(Path(__file__).resolve().parents[2]/'scripts'))
from analyze_super_environment import POLICY, evaluate, quiet


class EnvironmentChecks(unittest.TestCase):
    def setUp(self):
        self.rows = []
        for i in range(30):
            end = (i+1)*2000
            game = 0 if i < 3 else 2
            self.rows.append(dict(schema=2, sequence=i, qpc_frequency=1000,
                interval_start_qpc=end-2000, qpc_before=end-1, qpc_cpu=end, qpc_after=end+1,
                interval_seconds=2, target_pid=0 if i < 3 else 123,
                phase='preflight' if i < 3 else 'measurement',
                cpu_available=True, system_busy_core_equiv=game+.2,
                game_core_equiv=game, nongame_core_equiv=.2, processor_groups=1,
                gpu=dict(available=True, classifier_available=True, unrelated_max_engine_percent=1),
                disk_available=True, disk_bytes_per_second=1024, disk_average_queue=.01,
                read_wall_ms=1, monitor_cpu_ms=0, last_input_idle_seconds=120))
        self.meta = dict(schema=2, complete=True, sample_count=30, target_pid=123,
                         qpc_frequency=1000, policy=POLICY)

    def check(self):
        return evaluate(self.rows, self.meta, 123, 30000)

    def test_complete_quiet_windows_are_evidence_not_fps_acceptance(self):
        result = self.check()
        self.assertTrue(result['qualified'])
        self.assertFalse(result['performance_acceptance'])
        self.assertEqual(result['windows']['peak']['covered_seconds'], 5)

    def test_busy_cpu_and_gpu_cannot_be_hidden_by_whole_machine_percentage(self):
        for field in ('cpu', 'gpu', 'disk'):
            rows = copy.deepcopy(self.rows)
            if field == 'cpu':
                self.rows[16].update(system_busy_core_equiv=3.2, nongame_core_equiv=1.2)
            elif field == 'gpu':
                self.rows[16]['gpu']['unrelated_max_engine_percent'] = 20
            else:
                self.rows[16]['disk_average_queue'] = 2
            self.assertFalse(self.check()['qualified'])
            self.rows = rows

    def test_unavailable_telemetry_is_unknown_not_zero_load(self):
        self.rows[16]['gpu'] = dict(available=False, unrelated_max_engine_percent=0)
        self.assertIsNone(quiet(self.rows[16]))
        result = self.check()
        self.assertFalse(result['qualified'])
        self.assertGreater(result['windows']['peak']['unknown_seconds'], 0)

    def test_game_composition_is_reported_separately_without_hiding_unrelated_gpu(self):
        self.rows[16]['gpu'].update(nongame_max_engine_percent=70,
                                    compositor_max_engine_percent=69)
        self.assertTrue(self.check()['qualified'])
        self.rows[16]['gpu']['unrelated_max_engine_percent'] = 15
        self.assertFalse(self.check()['qualified'])
        self.rows[16]['gpu']['classifier_available'] = False
        self.assertIsNone(quiet(self.rows[16]))

    def test_clock_holes_owned_pid_mismatch_and_false_time_accounting_rejected(self):
        for field, value in (('sequence', 0), ('interval_start_qpc', 0),
                             ('target_pid', 456), ('nongame_core_equiv', 0)):
            original = copy.deepcopy(self.rows)
            self.rows[16][field] = value
            with self.assertRaises(ValueError):
                self.check()
            self.rows = original

    def test_recent_user_input_prevents_launch_and_incomplete_observation_rejected(self):
        self.rows[2]['last_input_idle_seconds'] = 0
        self.assertFalse(self.check()['qualified'])
        self.meta['complete'] = False
        with self.assertRaises(ValueError):
            self.check()


if __name__ == '__main__':
    unittest.main()
