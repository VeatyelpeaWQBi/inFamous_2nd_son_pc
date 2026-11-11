"""Pure qualification regressions; no game, profiler or files generated."""
import copy
from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "scripts"))
from analyze_feedback_pair import qualify, baseline_order


class FeedbackPairChecks(unittest.TestCase):
    def setUp(self):
        def session(name, on):
            tuning = dict(command_buffer_sharps=0, compute_barrier_scope=0,
                          readback_feedback_checkpoint=on, readback_feedback_writers=0)
            return dict(session=name, runtime_sha256="same", game_sha256="same", mode="pro4k", complete=True,
                        exit_code=0, saves=[dict(path="save", bytes=1, sha256="same")], trials=[
                dict(kind="measurement", status="collected", warm=True, meter_consumed=True,
                     tuning_verified=True, requested_tuning=tuning, compile_delta={"shader": 0},
                     mechanism_delta=dict(feedback_checkpoint_submits=on),
                     profile="lightweight", scene_before=dict(draws_per_frame=6000, dispatches_per_frame=400),
                     windows={k: dict(n=10, fps=40, p99_ms=50) for k in
                              ("baseline", "early", "peak", "late", "recovery", "tail")})])
        self.a, self.b = session("A", 0), session("B", 1)

    def test_access_union_requires_actual_startup_use_and_isolation(self):
        for on, session in enumerate((self.a, self.b)):
            session['access_union_receipt'] = dict(requested=bool(on), enabled=bool(on), used=bool(on))
            session['trials'][0]['requested_tuning'].update(
                readback_feedback_checkpoint=0, readback_word_load=0, asc_queue=0, gds_counter_scope=0,
                compute_threaded_recording=0, written_sync_fastpath=0, image_sync_once=0,
                readback_word_prefetch=0, readback_word_prefetch_bulk=0, readback_word_page_cluster=0)
        result = qualify(self.a, self.b, 'access-union', 'lightweight')
        self.assertTrue(result['data_checks_passed'])
        self.assertTrue(result['mechanism']['coverage_observed'])
        self.assertFalse(result['performance_acceptance'])
        self.b['access_union_receipt']['used'] = False
        self.assertFalse(qualify(self.a, self.b, 'access-union', 'lightweight')['data_checks_passed'])

    def test_combined_buffer_candidate_requires_both_paths(self):
        for on, session in enumerate((self.a, self.b)):
            session["access_union_receipt"] = dict(requested=bool(on), enabled=bool(on), used=bool(on))
            trial = session["trials"][0]
            trial["requested_tuning"].update(
                readback_feedback_checkpoint=0, readback_word_load=0, asc_queue=0, gds_counter_scope=0,
                compute_threaded_recording=0, written_sync_fastpath=on, image_sync_once=0,
                readback_word_prefetch=0, readback_word_prefetch_bulk=0, readback_word_page_cluster=0)
            trial["mechanism_delta"] = dict(written_sync_skips=on * 100)
        result = qualify(self.a, self.b, "buffer-prep-union", "lightweight")
        self.assertTrue(result["data_checks_passed"])
        self.assertTrue(result["mechanism"]["coverage_observed"])
        self.assertFalse(result["performance_acceptance"])
        self.b["trials"][0]["mechanism_delta"]["written_sync_skips"] = 0
        self.assertFalse(qualify(self.a, self.b, "buffer-prep-union", "lightweight")["data_checks_passed"])
        self.b["trials"][0]["mechanism_delta"]["written_sync_skips"] = 100
        self.b["access_union_receipt"]["used"] = False
        self.assertFalse(qualify(self.a, self.b, "buffer-prep-union", "lightweight")["data_checks_passed"])

    def check(self):
        return qualify(self.a, self.b, "feedback-checkpoint", "lightweight")

    def test_requested_environment_observation_cannot_be_omitted_or_busy(self):
        def check():
            return qualify(self.a, self.b, 'feedback-checkpoint', 'lightweight', require_environment=True)
        self.assertFalse(check()['data_checks_passed'])
        for session in (self.a, self.b):
            session['trials'][0]['environment'] = dict(qualified=True)
        self.assertTrue(check()['data_checks_passed'])
        self.b['trials'][0]['environment']['qualified'] = False
        self.assertFalse(check()['data_checks_passed'])

    def prepare_feedback_pool(self):
        for on, session in enumerate((self.a, self.b)):
            session['trials'][0]['requested_tuning'].update(
                readback_feedback_checkpoint=0, readback_word_load=0, asc_queue=0, gds_counter_scope=0,
                compute_threaded_recording=0, written_sync_fastpath=0, image_sync_once=0,
                readback_word_prefetch=0, readback_word_prefetch_bulk=0,
                readback_word_page_cluster=0, readback_window_kb=512)
            session['feedback_arena_pool_receipt'] = dict(requested=bool(on), compact_layout_verified=True,
                before=dict(moves=[100*on, 100*on], used=[200*on, 200*on],
                            busy=[0, 0], overflows=[0, 0], resets=[10*on, 10*on]),
                after=dict(moves=[200*on, 200*on], used=[300*on, 300*on],
                           busy=[0, 0], overflows=[0, 0], resets=[20*on, 20*on]))

    def test_runtime_context_requires_independent_proof_and_active_pool_on_both_sides(self):
        self.prepare_feedback_pool()
        self.a['feedback_arena_pool_receipt'] = copy.deepcopy(self.b['feedback_arena_pool_receipt'])
        for on, session in enumerate((self.a, self.b)):
            session['trials'][0]['requested_tuning'].update(
                compute_barrier_scope=1, runtime_context_memo=on)
            session['runtime_context_receipt'] = dict(enabled=bool(on), same_switch=True,
                before=[10000*on, 64*on, 0], after=[20000*on, 67*on, 0])
        def check():
            return qualify(self.a, self.b, 'runtime-context', 'lightweight', normal_rendering=True)
        self.assertTrue(check()['data_checks_passed'])
        self.assertFalse(check()['performance_acceptance'])
        original = copy.deepcopy(self.b)
        for field, value in (('enabled', False), ('same_switch', False),
                             ('after', [20000, 67, 1]), ('before', [0, 0, 0])):
            self.b = copy.deepcopy(original)
            self.b['runtime_context_receipt'][field] = value
            self.assertFalse(check()['data_checks_passed'])
        self.b = original
        self.a['feedback_arena_pool_receipt']['requested'] = False
        self.assertFalse(check()['data_checks_passed'])

    def test_feedback_pool_requires_capacity_and_both_sides_without_claiming_gain(self):
        self.prepare_feedback_pool()
        def check():
            return qualify(self.a, self.b, 'feedback-arena-pool', 'lightweight')
        result = check()
        self.assertTrue(result['data_checks_passed'])
        self.assertTrue(result['mechanism']['coverage_observed'])
        self.assertEqual(result['mechanism']['busy_fallbacks_by_side'], [0, 0])
        self.assertEqual(result['mechanism']['original_side_initializations'], [10, 10])
        self.assertFalse(result['performance_acceptance'])
        original = copy.deepcopy(self.b)
        for key, value in (('overflows', [0, 1]), ('used', [2049, 300]),
                           ('moves', [100, 200]), ('busy', []), ('moves', [None, 200]),
                           ('resets', [10, 20])):
            self.b = copy.deepcopy(original)
            self.b['feedback_arena_pool_receipt']['after'][key] = value
            self.assertFalse(check()['data_checks_passed'])

    def test_tiled_pool_reference_must_also_be_active(self):
        self.prepare_feedback_pool()
        self.a['feedback_arena_pool_receipt'] = copy.deepcopy(self.b['feedback_arena_pool_receipt'])
        self.assertTrue(qualify(self.a, self.b, 'feedback-arena-tiled', 'lightweight')['data_checks_passed'])
        self.a['feedback_arena_pool_receipt']['requested'] = False
        self.assertFalse(qualify(self.a, self.b, 'feedback-arena-tiled', 'lightweight')['data_checks_passed'])

    def test_normal_renderer_combination_is_recorded_on_both_sides(self):
        self.prepare_feedback_pool()
        for session in (self.a, self.b):
            session['trials'][0]['requested_tuning']['compute_barrier_scope'] = 1
        self.assertTrue(qualify(self.a, self.b, 'feedback-arena-pool', 'lightweight',
                                normal_rendering=True)['data_checks_passed'])
        self.assertFalse(qualify(self.a, self.b, 'feedback-arena-pool', 'lightweight')['data_checks_passed'])
        self.a['trials'][0]['requested_tuning']['compute_barrier_scope'] = 0
        self.assertFalse(qualify(self.a, self.b, 'feedback-arena-pool', 'lightweight',
                                 normal_rendering=True)['data_checks_passed'])

    def test_valid_is_not_performance_acceptance(self):
        r = self.check()
        self.assertTrue(r["data_checks_passed"])
        self.assertTrue(r["scene_review_required"])
        self.assertFalse(r["performance_acceptance"])

    def test_extra_word_coverage_is_not_a_frame_or_value_acceptance(self):
        for on, session in enumerate((self.a, self.b)):
            trial = session['trials'][0]
            trial['requested_tuning'].update(readback_feedback_checkpoint=0,
                readback_word_load=on, readback_word_prefetch=on, readback_word_prefetch_bulk=0,
                readback_word_page_cluster=on)
            trial['mechanism_delta'] = dict(feedback_word_cluster_served=on * 20)
        result = qualify(self.a, self.b, 'feedback-page-words', 'lightweight')
        self.assertTrue(result['data_checks_passed'])
        self.assertTrue(result['mechanism']['coverage_observed'])
        self.assertFalse(result['performance_acceptance'])

    def test_written_sync_coverage_does_not_claim_frame_gain_or_new_dependencies(self):
        for on, session in enumerate((self.a, self.b)):
            trial = session['trials'][0]
            trial['requested_tuning'].update(readback_feedback_checkpoint=0,
                                               written_sync_fastpath=on)
            trial['mechanism_delta'] = dict(written_sync_skips=on * 100)
        result = qualify(self.a, self.b, 'written-sync', 'lightweight')
        self.assertTrue(result['data_checks_passed'])
        self.assertTrue(result['mechanism']['coverage_observed'])
        self.assertIsNone(result['mechanism']['scheduling_or_dependency_change_observed'])
        self.assertFalse(result['performance_acceptance'])

    def test_duplicate_image_query_is_not_automatically_a_saved_gpu_copy(self):
        for on, session in enumerate((self.a, self.b)):
            trial = session['trials'][0]
            trial['requested_tuning'].update(readback_feedback_checkpoint=0,
                                             written_sync_fastpath=0, image_sync_once=on)
            trial['mechanism_delta'] = dict(image_sync_queries_skipped=on * 100,
                                            image_sync_copies_skipped=0)
        result = qualify(self.a, self.b, 'image-sync-once', 'lightweight')
        self.assertTrue(result['data_checks_passed'])
        self.assertTrue(result['mechanism']['coverage_observed'])
        self.assertFalse(result['mechanism']['gpu_work_avoided_observed'])
        self.b['trials'][0]['mechanism_delta']['image_sync_copies_skipped'] = 20
        result = qualify(self.a, self.b, 'image-sync-once', 'lightweight')
        self.assertTrue(result['mechanism']['gpu_work_avoided_observed'])
        self.assertFalse(result['performance_acceptance'])

    def test_reverse_capture_keeps_baseline_and_candidate_identity(self):
        a, b = dict(baseline=True, session='A'), dict(baseline=False, session='B')
        self.assertEqual(baseline_order(dict(sessions=[a, b])), [a, b])
        self.assertEqual(baseline_order(dict(capture_order='BA', sessions=[b, a])), [a, b])
        for index in (dict(sessions=[b, a]), dict(capture_order='BA', sessions=[a, b]),
                      dict(capture_order='BA', sessions=[b, b]), dict(capture_order='AB', sessions=[dict(baseline=1), b])):
            with self.assertRaises(ValueError):
                baseline_order(index)

    def test_zero_coverage_is_not_effective_optimization(self):
        self.b["trials"][0]["mechanism_delta"]["feedback_checkpoint_submits"] = 0
        r = self.check()
        self.assertTrue(r["data_checks_passed"])
        self.assertFalse(r["mechanism"]["coverage_observed"])
        self.assertFalse(r["mechanism"]["scheduling_or_dependency_change_observed"])

    def test_finer_lookup_without_reduction_is_not_changed_dependency(self):
        for on, session in enumerate((self.a, self.b)):
            t = session["trials"][0]
            t["requested_tuning"].update(readback_feedback_checkpoint=0,
                                         readback_feedback_writers=on)
            t["mechanism_delta"] = dict(feedback_writer_hits=7 * on,
                                         feedback_writer_reductions=0)
        r = qualify(self.a, self.b, "feedback-writers", "lightweight")
        self.assertTrue(r["data_checks_passed"])
        self.assertTrue(r["mechanism"]["coverage_observed"])
        self.assertFalse(r["mechanism"]["scheduling_or_dependency_change_observed"])

    def test_rejects_drift_cold_tuning_save_build_and_same_session(self):
        original = copy.deepcopy(self.b)
        mutations = [
            lambda: self.b.update(session="A"),
            lambda: self.b.update(runtime_sha256="other"),
            lambda: self.b.update(saves=[]),
            lambda: self.b.update(complete=False),
            lambda: self.b["trials"][0].update(warm=False),
            lambda: self.b["trials"][0].update(tuning_verified=False),
            lambda: self.b["trials"][0]["requested_tuning"].update(compute_barrier_scope=1),
            lambda: self.b["trials"][0]["compile_delta"].update(shader=1),
            lambda: self.b["trials"][0]["scene_before"].update(draws_per_frame=4700),
            lambda: self.b["trials"][0]["windows"].pop("late"),
        ]
        for mutation in mutations:
            self.b = copy.deepcopy(original)
            mutation()
            self.assertFalse(self.check()["data_checks_passed"])

    def test_compute_recorder_needs_actual_worker_submissions(self):
        for on, session in enumerate((self.a, self.b)):
            t = session['trials'][0]
            t['requested_tuning'].update(readback_feedback_checkpoint=0, readback_word_load=0,
                gds_counter_scope=0, threaded_recording=1, asc_queue=on,
                compute_threaded_recording=on)
            t['mechanism_delta'] = dict(native_compute=100*on, compute_recorder_submits=0)
            t['snapshots'] = [dict(seconds=5, thread_cpu={role: dict(available=True,
                cpu_percent=75, sample_age_ms=10, window_seconds=1) for role in
                ('command_processor', 'graphics_recorder', 'compute_recorder')})]
        r = qualify(self.a, self.b, 'compute-recorder', 'lightweight')
        self.assertTrue(r['data_checks_passed'])
        self.assertTrue(r['mechanism']['coverage_observed'])
        self.assertFalse(r['mechanism']['scheduling_or_dependency_change_observed'])
        self.b['trials'][0]['mechanism_delta']['compute_recorder_submits'] = 10
        r = qualify(self.a, self.b, 'compute-recorder', 'lightweight')
        self.assertTrue(r['mechanism']['scheduling_or_dependency_change_observed'])
        self.assertFalse(r['performance_acceptance'])
        self.b['trials'][0]['requested_tuning']['compute_threaded_recording'] = 0
        self.assertFalse(qualify(self.a, self.b, 'compute-recorder', 'lightweight')['data_checks_passed'])
        self.b['trials'][0]['requested_tuning']['compute_threaded_recording'] = 1
        self.b['trials'][0]['snapshots'][0]['thread_cpu']['compute_recorder']['sample_age_ms'] = 6000
        self.assertFalse(qualify(self.a, self.b, 'compute-recorder', 'lightweight')['data_checks_passed'])

    def test_gds_matches_without_saved_barriers_are_not_an_effect(self):
        for on, session in enumerate((self.a, self.b)):
            t = session['trials'][0]
            t['requested_tuning'].update(readback_feedback_checkpoint=0,
                readback_word_load=0, asc_queue=0, gds_counter_scope=on)
            t['mechanism_delta'] = dict(gds_scope_bindings=100*on,
                gds_scope_conflicts_reduced=80*on, gds_scope_barriers_avoided=0)
        r = qualify(self.a,self.b,'gds-counter','lightweight')
        self.assertTrue(r['data_checks_passed'])
        self.assertTrue(r['mechanism']['coverage_observed'])
        self.assertFalse(r['mechanism']['scheduling_or_dependency_change_observed'])

    def test_cpu_affinity_requires_hex_receipts_and_successful_thread_application(self):
        for on, session in enumerate((self.a, self.b)):
            t = session['trials'][0]
            t['requested_tuning'].update(readback_feedback_checkpoint=0, readback_word_load=0,
                asc_queue=0, gds_counter_scope=0, gpu_threads_affinity=65535 if on else 'auto',
                cpu_affinity=0, other_threads_affinity=0)
            t['requested_ini_tuning'] = dict(t['requested_tuning'],
                gpu_threads_affinity='ffff' if on else 'auto', cpu_affinity='0', other_threads_affinity='0')
        def check():
            return qualify(self.a, self.b, 'cpu-placement', 'lightweight', 65535)
        self.assertFalse(check()['data_checks_passed'])
        self.b['renderer_thread_affinities'] = {name: 65535 for name in
            ('shadPS4:GpuCommandProcessor', 'shadPS4:GpuCommandRecorder', 'shadPS4:GpuUploadWorker')}
        self.assertTrue(check()['data_checks_passed'])
        self.assertFalse(check()['performance_acceptance'])
        self.b['trials'][0]['requested_ini_tuning']['gpu_threads_affinity'] = '65535'
        self.assertFalse(check()['data_checks_passed'])
        self.b['trials'][0]['requested_ini_tuning']['gpu_threads_affinity'] = 'ffff'
        self.b['renderer_thread_affinities']['shadPS4:GpuCommandRecorder'] = 0xffffffff
        self.assertFalse(check()['data_checks_passed'])


if __name__ == "__main__":
    unittest.main()
