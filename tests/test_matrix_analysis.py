# SPDX-License-Identifier: GPL-2.0-or-later
import copy
import io
import sys
from pathlib import Path
import unittest
from unittest.mock import patch
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'scripts'))
from analyze_v3_matrix import (analyze, comparison, expected_overrides, selections,
                               save_signature, validate_launch, mechanism_evidence, NAMES)


def fixture():
    saves = [{'path': 'save.dat', 'bytes': 1024, 'sha256': '1' * 64}]
    plan = {'schema': 1, 'kind': 'v3-ordinary-independent-and-combined', 'mode': 'pro4k',
            'audio_muted': True, 'full_collectors': False, 'automatic_performance_acceptance': False,
            'runtime_sha256': 'a' * 64, 'game_sha256': 'b' * 64, 'source_sha256': 'c' * 64,
            'save_source_files': saves, 'launcher_settings': {'show_fps': True}, 'complete': False}
    row = {'index': 0, 'case': 'recording-4', 'role': 'candidate', 'candidates': ['recording-4'],
           'expected_overrides': expected_overrides(['recording-4']), 'status': 'pending',
           'session': 'state/sessions/fixture'}
    overrides = row['expected_overrides'] | dict.fromkeys(('gpu_profile', 'perf_log', 'profile_stacks',
                    'path_trace', 'consumer_evidence_limit', 'auxiliary_evidence_limit'), 0)
    launch = {key: plan[key] for key in ('runtime_sha256', 'game_sha256', 'mode', 'save_source_files', 'launcher_settings')}
    launch.update(session=row['session'], game_launched=True, complete=True, exit_code=0,
                  audio_muted=True, diagnostic=None, timing_profile=None, optimization_v3='custom',
                  v3_candidates=['recording-4'], v3_overrides=overrides,
                  resolution={'width': 3840, 'height': 2160})
    return plan, row, launch


class MatrixAnalysisTests(unittest.TestCase):
    def scan_logs(self, contents, candidates):
        session=Path(__file__).resolve().parents[1]/'state/sessions/analysis-unit-fixture'
        with patch.object(Path,'is_file',lambda path:path.name in contents), \
             patch.object(Path,'open',lambda path,*a,**k:io.StringIO(contents[path.name])):
            return mechanism_evidence(session,candidates)

    def test_native_title_and_boot_logs_without_legacy_double_counting(self):
        result=self.scan_logs({
            'shadps4.log':'[Config] <Warning> (Main) init.cpp:1 Init: boot warning\n',
            'CUSA00309.log':'[Core.Linker] <Error> (Game:Main) module.cpp:288 Load: baseline error\n'
                '[Render] <Info> (Worker) scheduler.cpp:1 Record: V3 complete-session recorder active: workers=2 commands=0\n'
                '[Render] <Info> (Worker) scheduler.cpp:2 Record: V3 complete-session recorder fallback: boundary\n'
                '[Render] <Info> (CP) scheduler.h:1 Copy: V3 copy regions coalesced: 2 -> 1 within one command\n',
            'shad_log.txt':'[Old] <Critical> (Main) old.cpp:1 Old: must not be counted\n',
            'CUSA99999.log':'[Other] <Critical> (Main) other.cpp:1 Other: must not be counted\n',
        },['recording-2','copy-coalesce'])
        self.assertTrue(result['log_available'])
        self.assertEqual(result['runtime_log_counts'],{'warning':1,'error':1,'critical':0})
        self.assertEqual(result['runtime_issue_signatures']['error'],{'Core.Linker: Load: baseline error':1})
        self.assertEqual({Path(p).name for p in result['log_paths']},{'shadps4.log','CUSA00309.log'})
        recording=result['candidates']['recording-2']
        self.assertTrue(recording['activation_log_observed'])
        self.assertTrue(recording['fallback_observed'])
        self.assertFalse(recording['nonempty_session_observed'])
        self.assertFalse(recording['measurement_window_activation_confirmed'])
        self.assertTrue(result['candidates']['copy-coalesce']['activation_log_observed'])

    def test_missing_log_is_unknown_and_legacy_fallback_is_supported(self):
        missing=self.scan_logs({},['completion-elision'])
        self.assertFalse(missing['log_available'])
        self.assertIsNone(missing['candidates']['completion-elision']['activation_log_observed'])
        legacy=self.scan_logs({'shad_log.txt':'[Render] <Info> (Game) cache.cpp:1 Read: V3 readback completion elided: exact\n'},['completion-elision'])
        self.assertTrue(legacy['log_available'])
        self.assertTrue(legacy['candidates']['completion-elision']['activation_log_observed'])
        boot=self.scan_logs({'shadps4.log':'startup only\n'},[])
        self.assertFalse(boot['log_available'])

    def test_all_selections_and_combinations_are_independent(self):
        self.assertEqual(len(NAMES), 19)
        for name in NAMES:
            values = expected_overrides(selections(name))
            self.assertEqual(sum(bool(v) for v in values.values()), 1)
        for workers in (2, 4):
            chosen = selections('combined-' + str(workers))
            values = expected_overrides(chosen)
            self.assertEqual(len(chosen), 16)
            self.assertEqual(values['v3_recording_workers'], workers)
            self.assertEqual(values['v3_disable_params_lookup'], 0)
            self.assertEqual(values['v3_disable_flatbuf_reuse'], 0)

    def test_frozen_launch_valid_and_mutations_rejected(self):
        plan, row, launch = fixture()
        validate_launch(plan, row, launch)
        for field, value in (('runtime_sha256', 'wrong'), ('audio_muted', False), ('exit_code', 1),
                             ('diagnostic', 'full'), ('resolution', {'width': 1920, 'height': 1080}),
                             ('v3_candidates', []), ('save_source_files', []), ('launcher_settings', {})):
            invalid = copy.deepcopy(launch); invalid[field] = value
            with self.assertRaises(ValueError, msg=field): validate_launch(plan, row, invalid)
        for key in ('path_trace', 'auxiliary_evidence_limit', 'v3_copy_coalesce'):
            invalid = copy.deepcopy(launch); invalid['v3_overrides'][key] = 1
            with self.assertRaises(ValueError, msg=key): validate_launch(plan, row, invalid)

    def test_two_controls_bound_gain_and_tail_regression_remains_visible(self):
        def frame(fps, tail): return dict(fps=fps, p95_ms=tail, p99_ms=tail, max_ms=tail)
        result = comparison(frame(30, 34), frame(32, 50), frame(33, 35))
        self.assertTrue(result['control_drift_over_3_percent'])
        self.assertLess(result['candidate_fps_change_percent_vs_controls'][0], 0)
        self.assertGreater(result['candidate_fps_change_percent_vs_controls'][1], 0)
        self.assertEqual(result['tail_change_ms_vs_controls']['p99_ms'], [16, 15])
        self.assertFalse(result['performance_acceptance'])

    def test_plan_only_cannot_claim_collection(self):
        plan, row, _ = fixture()
        rows = []
        for index, role in enumerate(('control-before', 'candidate', 'control-after')):
            rows.append(dict(row, index=index, role=role))
        plan['rows'] = rows
        result = analyze(plan)
        self.assertEqual(result['collected_runs'], 0)
        self.assertFalse(result['collection_complete'])
        plan['complete'] = True
        with self.assertRaises(ValueError): analyze(plan)

    def test_invalid_selection_and_duplicate_save_rejected(self):
        for names in (['recording-2', 'recording-4'], ['bogus'], ['copy-coalesce'] * 2):
            with self.assertRaises(ValueError): expected_overrides(names)
        with self.assertRaises(ValueError): save_signature([{'path': 'x', 'bytes': 0, 'sha256': 'a'}] * 2)


if __name__ == '__main__': unittest.main()
