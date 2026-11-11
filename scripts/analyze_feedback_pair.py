"""Offline qualification of two fresh-process feedback trials. Never launches anything."""
import argparse
import json
from pathlib import Path

from analyze_super_batch import analyze, load

RECENT_CANDIDATES = {
    "early-complete": ("readback_early_complete", "feedback_early_complete_submits",
                       "feedback_early_complete_fallbacks"),
    "writer-footprint": ("readback_writer_footprint", "writer_footprint_narrowed",
                         "writer_footprint_proven", "writer_footprint_fallbacks"),
}


def baseline_order(index):
    records = index.get("sessions", [])
    flags = [r.get("baseline") for r in records]
    order = index.get("capture_order", "AB")
    expected = {"AB": [True, False], "BA": [False, True]}.get(order)
    if expected is None or flags != expected or any(type(v) is not bool for v in flags):
        raise ValueError("Pair records do not match their explicit AB/BA capture order.")
    return records if order == "AB" else list(reversed(records))


def qualify(a, b, candidate, profile, performance_core_mask=None, normal_rendering=False,
            require_environment=False):
    issues = []
    if type(require_environment) is not bool:
        issues.append('Invalid external observation requirement.')
    if type(normal_rendering) is not bool or (normal_rendering and candidate not in ("feedback-arena-pool", "runtime-context")):
        issues.append("Normal renderer settings are restricted to the compact pool comparison.")
    if a.get("session") == b.get("session"):
        issues.append("Trials must use distinct game sessions/processes.")
    for key in ("runtime_sha256", "game_sha256", "mode"):
        if not a.get(key) or a.get(key) != b.get(key):
            issues.append(f"Different or missing {key}.")
    saves = []
    for session in (a, b):
        saves.append(sorted((r["path"], r["bytes"], r["sha256"])
                            for r in session.get("saves") or []))
    if not saves[0] or saves[0] != saves[1]:
        issues.append("Original save snapshots differ or are missing.")
    trials = []
    for ordinal, session in enumerate((a, b)):
        if not session.get("complete") or session.get("exit_code") != 0:
            issues.append(f"Trial {ordinal}: launcher did not finalize successfully.")
        measured = [t for t in session.get("trials", []) if t.get("kind") == "measurement"]
        if len(measured) != 1:
            issues.append(f"Trial {ordinal}: expected exactly one measured release.")
            continue
        t = measured[0]
        trials.append(t)
        if require_environment and not t.get('environment', {}).get('qualified'):
            issues.append(f'Trial {ordinal}: external load observation did not qualify.')
        expected = dict(command_buffer_sharps=0, compute_barrier_scope=int(bool(normal_rendering)),
                        readback_feedback_checkpoint=0, readback_feedback_writers=0)
        for tuning_key, *_ in RECENT_CANDIDATES.values():
            if tuning_key in t.get("requested_tuning", {}):
                expected[tuning_key] = 0
        # Preserve old four-flag historical captures, while explicitly isolating the new flag
        # whenever it exists in the request or this is its own experiment.
        if candidate == "feedback-word" or "readback_word_load" in t.get("requested_tuning", {}):
            expected["readback_word_load"] = 0
        if candidate == "feedback-async" or "asc_queue" in t.get("requested_tuning", {}):
            expected["asc_queue"] = 0
        if candidate == "gds-counter" or "gds_counter_scope" in t.get("requested_tuning", {}):
            expected["gds_counter_scope"] = 0
        if candidate == "compute-recorder" or "compute_threaded_recording" in t.get("requested_tuning", {}):
            expected["compute_threaded_recording"] = 0
        key = {"feedback-checkpoint": "readback_feedback_checkpoint",
               "feedback-writers": "readback_feedback_writers", "feedback-word": "readback_word_load",
               "feedback-async": "asc_queue", "gds-counter": "gds_counter_scope"}.get(candidate)
        if candidate in RECENT_CANDIDATES:
            expected[RECENT_CANDIDATES[candidate][0]] = ordinal
        elif candidate == "cpu-placement":
            if not performance_core_mask or not 0 < performance_core_mask < 2**64-1:
                issues.append("Missing valid enumerated performance-core mask.")
            expected.update(gpu_threads_affinity="auto" if ordinal == 0 else performance_core_mask,
                            cpu_affinity=0, other_threads_affinity=0)
            ini = t.get("requested_ini_tuning") or {}
            wanted_ini = dict(expected)
            wanted_ini.update(gpu_threads_affinity="auto" if ordinal == 0 else f"{performance_core_mask or 0:x}",
                              cpu_affinity="0", other_threads_affinity="0")
            if ini != wanted_ini:
                issues.append(f"Trial {ordinal}: hexadecimal INI/receipt contract differs.")
        elif candidate in ("feedback-word-prefetch", "feedback-word-bulk", "feedback-page-words"):
            expected.update(readback_word_load=ordinal, readback_word_prefetch=ordinal)
            if candidate == "feedback-word-bulk":
                expected["readback_word_prefetch_bulk"] = ordinal
            elif candidate == "feedback-page-words":
                expected.update(readback_word_prefetch_bulk=0, readback_word_page_cluster=ordinal)
        elif candidate in ("access-union", "readback-copy-stage", "image-read-transition",
                           "buffer-prep-union"):
            combined = candidate == "buffer-prep-union"
            expected.update(written_sync_fastpath=ordinal if combined else 0, image_sync_once=0,
                            readback_word_prefetch=0, readback_word_prefetch_bulk=0,
                            readback_word_page_cluster=0)
            if candidate != "readback-copy-stage":
                prefix = "access_union" if combined else candidate.replace("-", "_")
                receipt = session.get(prefix + "_receipt", {})
                if any(receipt.get(k) is not bool(ordinal) for k in ('requested', 'enabled', 'used')):
                    issues.append(f'Trial {ordinal}: {candidate} startup / actual-use receipt differs.')
                if combined:
                    count = t.get("mechanism_delta", {}).get("written_sync_skips")
                    if type(count) is not int or (count <= 0 if ordinal else count != 0):
                        issues.append(f"Trial {ordinal}: written preparation coverage differs.")
            else:
                receipt = session.get("readback_copy_stage_receipt", {})
                delta = t.get("mechanism_delta", {}).get("readback_copy_stage_narrowed")
                if (receipt.get("requested") is not bool(ordinal) or
                        receipt.get("enabled") is not bool(ordinal) or
                        type(delta) is not int or (delta <= 0 if ordinal else delta != 0)):
                    issues.append(f"Trial {ordinal}: readback copy scope startup/use receipt differs.")
        elif candidate in ("vertex-input-key", "texture-binding-reuse"):
            expected.update(written_sync_fastpath=0, image_sync_once=0,
                            readback_word_prefetch=0, readback_word_prefetch_bulk=0,
                            readback_word_page_cluster=0)
            receipt = session.get(candidate.replace('-', '_') + "_receipt", {})
            before, after = (receipt.get(k) for k in ("before", "after"))
            if (receipt.get("requested") is not bool(ordinal) or
                    type(before) is not int or type(after) is not int or
                    (not 0 < before < after if ordinal else before != 0 or after != 0)):
                issues.append(f"Trial {ordinal}: {candidate} startup/use receipt differs.")
        elif candidate in ("feedback-arena-pool", "feedback-arena-tiled", "runtime-context"):
            expected.update(written_sync_fastpath=0, image_sync_once=0,
                            readback_word_prefetch=0, readback_word_prefetch_bulk=0,
                            readback_word_page_cluster=0, readback_window_kb=512)
            if candidate == "runtime-context":
                expected['runtime_context_memo'] = ordinal
                rc = session.get('runtime_context_receipt', {})
                counts = [rc.get(phase) for phase in ('before', 'after')]
                valid_counts = all(isinstance(r, list) and len(r) == 3 and
                                   all(type(n) is int and n >= 0 for n in r) for r in counts)
                if (not normal_rendering or rc.get('enabled') is not bool(ordinal) or
                        rc.get('same_switch') is not True or
                        not valid_counts or
                        (valid_counts and (any(r[2] for r in counts) or
                         (not ordinal and any(n for r in counts for n in r)) or
                         (ordinal and (not 0 < counts[0][0] < counts[1][0] or
                                       not 0 < counts[0][1] <= counts[1][1]))))):
                    issues.append(f'Trial {ordinal}: runtime context proof/use differs.')
            receipt = session.get("feedback_arena_pool_receipt", {})
            before, after = (receipt.get(k, {}) for k in ("before", "after"))
            valid = all(isinstance(sample.get(key), list) and len(sample[key]) == 2 and
                        all(type(value) is int and value >= 0 for value in sample[key])
                        for sample in (before, after)
                        for key in ("moves", "overflows", "busy", "used", "resets"))
            active = bool(ordinal) or candidate in ("feedback-arena-tiled", "runtime-context")
            if (receipt.get("requested") is not active or not valid or
                    not receipt.get("compact_layout_verified") or
                    (not active and any(value for sample in (before, after)
                                          for values in sample.values() for value in values)) or
                    (active and any(
                        sample["overflows"][side] != 0 or
                        not 0 < sample["used"][side] <= 2048 or
                        sample["moves"][side] <= 0 or sample["resets"][side] <= 0
                        for sample in (before, after) for side in range(2))) or
                    (active and any(after["moves"][side] <= before["moves"][side] or
                                          after["busy"][side] < before["busy"][side] or
                                          after["resets"][side] <= before["resets"][side] or
                                          after["used"][side] < before["used"][side]
                                          for side in range(2)))):
                issues.append(f"Trial {ordinal}: feedback pool startup/use/capacity receipt differs.")
        elif candidate == "feedback-arena-group":
            expected.update(written_sync_fastpath=0, image_sync_once=0,
                            readback_word_prefetch=0, readback_word_prefetch_bulk=0,
                            readback_word_page_cluster=0, readback_window_kb=512)
            receipt = session.get("feedback_arena_group_receipt", {})
            before, after = (receipt.get(k) for k in ("before", "after"))
            valid = all(isinstance(values, list) and len(values) == 32 and
                        all(type(value) is int and value >= 0 for value in values)
                        for values in (before, after))
            if (receipt.get("requested") is not bool(ordinal) or not valid or
                    (ordinal == 0 and any(before + after)) or
                    (ordinal == 1 and any(not 0 < before[i] < after[i]
                                          for i in (4, 6, 7, 20, 22, 23)))):
                issues.append(f"Trial {ordinal}: feedback group startup/use receipt differs.")
        elif candidate == "feedback-arena":
            expected.update(written_sync_fastpath=0, image_sync_once=0,
                            readback_word_prefetch=0, readback_word_prefetch_bulk=0,
                            readback_word_page_cluster=0, readback_window_kb=512)
            receipt = session.get("feedback_arena_receipt", {})
            moves = receipt.get("moved_during_release")
            starts = receipt.get("moves_before_release")
            if (receipt.get("requested") is not bool(ordinal) or not receipt.get("window_verified")
                    or moves is None or starts is None
                    or (not all(v > 0 for v in starts + moves) if ordinal else any(starts + moves))):
                issues.append(f"Trial {ordinal}: feedback arena startup/use receipt differs.")
        elif candidate == "image-sync-once":
            expected.update(written_sync_fastpath=0, image_sync_once=ordinal)
        elif candidate == "written-sync":
            expected["written_sync_fastpath"] = ordinal
        elif candidate == "reg-dump-fast":
            expected["reg_dump_fast_empty"] = ordinal
        elif candidate == "particle-poll-pause":
            if session.get("particle_poll_pause_loaded") is not bool(ordinal):
                issues.append(f"Trial {ordinal}: particle poll patch receipt differs.")
        elif candidate == "c73-scan":
            if session.get("c73_scan_loaded") is not bool(ordinal):
                issues.append(f"Trial {ordinal}: C73 preloaded module receipt differs.")
        elif candidate == "recorder-spin":
            expected.update(threaded_recording=1, recorder_spin_us=200 if ordinal == 0 else 0)
        elif candidate == "compute-recorder":
            expected.update(threaded_recording=1, asc_queue=ordinal,
                            compute_threaded_recording=ordinal)
        else:
            expected[key] = ordinal
        if t.get("requested_tuning") != expected or not t.get("tuning_verified"):
            issues.append(f"Trial {ordinal}: requested/observed tuning mismatch.")
        compile_delta = t.get("compile_delta", {})
        if any(compile_delta.values()):
            issues.append(f"Trial {ordinal}: shader/pipeline compilation occurred: {compile_delta}.")
        elif not t.get("warm") or t.get("status") == "captured-new-compilation":
            issues.append(f"Trial {ordinal}: warm status was not verified.")
        if t.get("status") not in ("collected", "captured-new-compilation"):
            issues.append(f"Trial {ordinal}: capture status is {t.get('status')!r}.")
        if not t.get("meter_consumed"):
            issues.append(f"Trial {ordinal}: super-meter consumption was not observed.")
        if t.get("profile") != profile:
            issues.append(f"Trial {ordinal}: profile differs from pair index.")
        if candidate == "compute-recorder":
            roles = ("command_processor", "graphics_recorder") + (("compute_recorder",) if ordinal else ())
            for role in roles:
                observed = []
                for sample in t.get("snapshots", []):
                    cpu = (sample.get("thread_cpu") or {}).get(role, {})
                    if (0 <= sample.get("seconds", -1) <= 12 and cpu.get("available") is True and
                            isinstance(cpu.get("cpu_percent"), (float, int)) and
                            0 <= cpu["cpu_percent"] <= 200 and
                            0 <= cpu.get("sample_age_ms", 1e9) <= 3000 and
                            0 < cpu.get("window_seconds", 0) < 5):
                        observed.append(cpu)
                if not observed:
                    issues.append(f"Trial {ordinal}: fresh native thread CPU sample missing for {role}.")
        if not all(t.get("windows", {}).get(k, {}).get("n", 0) > 0
                   for k in ("baseline", "early", "peak", "late", "recovery")):
            issues.append(f"Trial {ordinal}: required frame windows missing.")
        if candidate in RECENT_CANDIDATES and not t.get("windows", {}).get("super_0_12", {}).get("n"):
            issues.append(f"Trial {ordinal}: combined super frame window missing.")
    drift = {}
    windows = {}
    mechanism = {}
    if len(trials) == 2:
        delta = trials[1].get("mechanism_delta", {})
        coverage, effect = {"feedback-checkpoint": ("feedback_checkpoint_submits", "feedback_checkpoint_submits"),
                            "feedback-writers": ("feedback_writer_hits", "feedback_writer_reductions"),
                            "feedback-word": ("feedback_word_served", "feedback_word_served"),
                            "feedback-word-prefetch": ("feedback_word_served", "feedback_word_served"),
                            "feedback-word-bulk": ("feedback_word_bulk_queued", "feedback_word_bulk_queued"),
                            "feedback-page-words": ("feedback_word_cluster_served", "feedback_word_cluster_served"),
                            "written-sync": ("written_sync_skips", "written_sync_skips"),
                            "buffer-prep-union": (None, None),
                            "image-sync-once": ("image_sync_queries_skipped", "image_sync_copies_skipped"),
                            "feedback-async": ("native_compute", "native_compute"),
                            "gds-counter": ("gds_scope_bindings", "gds_scope_barriers_avoided"),
                            "cpu-placement": (None, None),
                            "compute-recorder": ("native_compute", "compute_recorder_submits"),
                            "recorder-spin": (None, None),
                            "reg-dump-fast": (None, None),
                            "particle-poll-pause": (None, None), "c73-scan": (None, None),
                            "readback-copy-stage": ("readback_copy_stage_narrowed", "readback_copy_stage_narrowed"),
                            "access-union": (None, None),
                            "image-read-transition": (None, None),
                            "feedback-arena": (None, None),
                            "vertex-input-key": (None, None),
                            "texture-binding-reuse": (None, None),
                            "feedback-arena-group": (None, None),
                            "feedback-arena-pool": (None, None),
                            "feedback-arena-tiled": (None, None), "runtime-context": (None, None),
                            "early-complete": (None, None), "writer-footprint": (None, None)}[candidate]
        if candidate in RECENT_CANDIDATES:
            _, active_counter, *other_counters = RECENT_CANDIDATES[candidate]
            counters = (active_counter, *other_counters)
            baseline = trials[0].get("mechanism_delta", {})
            valid = all(type(baseline.get(k)) is int and baseline[k] == 0 and
                        type(delta.get(k)) is int and delta[k] >= 0 for k in counters)
            if not valid:
                issues.append("Missing, negative or active-in-baseline mechanism counters.")
            mechanism = dict(coverage_observed=valid and delta[active_counter] > 0,
                             baseline_delta={k: baseline.get(k) for k in counters},
                             release_delta={k: delta.get(k) for k in counters})
        elif candidate == "buffer-prep-union":
            used = b.get("access_union_receipt", {}).get("used") is True
            count = delta.get("written_sync_skips")
            mechanism = dict(coverage_observed=used and type(count) is int and count > 0,
                             access_union_used=used, skipped_upload_preparations=count,
                             scheduling_or_dependency_change_observed=False,
                             limitation="Both equivalent paths were used. Timing does not "
                                        "isolate the contribution of each path.")
        elif candidate in ("access-union", "image-read-transition"):
            receipt = b.get(candidate.replace('-', '_') + '_receipt', {})
            mechanism = dict(coverage_observed=receipt.get('used') is True,
                             scheduling_or_dependency_change_observed=False,
                             limitation='The equivalent path was executed. All real accesses and barriers are retained; use frame times to assess performance.')
        elif candidate in ("vertex-input-key", "texture-binding-reuse"):
            receipt = b.get(candidate.replace('-', '_') + "_receipt", {})
            before, after = (receipt.get(k) for k in ("before", "after"))
            hits = after - before if type(before) is int and type(after) is int else None
            mechanism = dict(coverage_observed=hits is not None and hits > 0,
                             scheduling_or_dependency_change_observed=False)
            field = ("vertex_input_preparations_reused" if candidate == "vertex-input-key"
                     else "texture_binding_storage_reused")
            mechanism[field] = hits
        elif candidate in ("feedback-arena-pool", "feedback-arena-tiled", "runtime-context"):
            receipt = b.get("feedback_arena_pool_receipt", {})
            before, after = (receipt.get(k, {}) for k in ("before", "after"))
            def counter_delta(key):
                start, end = (sample.get(key) for sample in (before, after))
                return ([y - x for x, y in zip(start, end)]
                        if isinstance(start, list) and isinstance(end, list) and
                        len(start) == len(end) == 2 and
                        all(type(v) is int for v in start + end) else None)
            moves, busy = (counter_delta(key) for key in ("moves", "busy"))
            mechanism = dict(coverage_observed=moves is not None and all(v > 0 for v in moves),
                             relocated_records_by_side=moves, busy_fallbacks_by_side=busy,
                             original_side_initializations=counter_delta("resets"),
                             peak_record_slots_by_side=after.get("used"),
                             scheduling_or_dependency_change_observed=None,
                             limitation="Private slots retain the original side lifetime and precise readback. "
                                        "Capacity fallbacks use original reservations; counters do not "
                                        "prove wait reduction or faster frames.")
        elif candidate == "feedback-arena-group":
            receipt = b.get("feedback_arena_group_receipt", {})
            before, after = (receipt.get(k) for k in ("before", "after"))
            moves = ([y - x for x, y in zip(before, after)]
                     if isinstance(before, list) and isinstance(after, list) else None)
            mechanism = dict(coverage_observed=moves is not None and len(moves) == 32 and
                             all(moves[i] > 0 for i in (4, 6, 7, 20, 22, 23)),
                             relocated_records_by_slot=moves,
                             scheduling_or_dependency_change_observed=None)
        elif candidate == "feedback-arena":
            moves = b.get("feedback_arena_receipt", {}).get("moved_during_release")
            mechanism = dict(coverage_observed=moves is not None and all(v > 0 for v in moves),
                             relocated_records_by_side=moves,
                             scheduling_or_dependency_change_observed=None,
                             limitation="These are allocations. Use frame time and total precise "
                                        "readback waits to see whether isolation helped.")
        elif candidate == "cpu-placement":
            threads = ("shadPS4:GpuCommandProcessor", "shadPS4:GpuCommandRecorder", "shadPS4:GpuUploadWorker")
            observed = b.get("renderer_thread_affinities", {})
            applied = bool(performance_core_mask) and all(observed.get(t) == performance_core_mask for t in threads)
            mechanism = dict(coverage_observed=applied, scheduling_or_dependency_change_observed=applied,
                             successful_thread_affinity_logs=observed,
                             limitation="Successful SetThreadAffinityMask is an allowed-core constraint, not a residency measurement or proof of faster frames.")
            if not applied:
                issues.append("Required CP/recorder/upload thread affinity application logs missing or different.")
        elif candidate == "feedback-page-words":
            served = delta.get("feedback_word_cluster_served")
            mechanism = dict(coverage_observed=isinstance(served, int) and served > 0,
                             scheduling_or_dependency_change_observed=None,
                             additional_exact_words_served=served,
                             limitation="Additional words passed the existing exact-version "
                                        "checks; reference comparisons and frame/tail times "
                                        "remain separate requirements.")
            if served is None:
                issues.append("Additional exact word coverage counter missing.")
        elif candidate == "image-sync-once":
            queries, copies = (delta.get(k) for k in
                               ("image_sync_queries_skipped", "image_sync_copies_skipped"))
            mechanism = dict(coverage_observed=isinstance(queries, int) and queries > 0,
                             scheduling_or_dependency_change_observed=None,
                             gpu_work_avoided_observed=isinstance(copies, int) and copies > 0,
                             duplicate_queries_skipped=queries, duplicate_conversions_skipped=copies,
                             limitation="The first image synchronization is retained; counters "
                                        "do not establish a frame-time gain.")
            if queries is None or copies is None:
                issues.append("Duplicate image synchronization counters missing.")
        elif candidate == "written-sync":
            count = delta.get("written_sync_skips")
            mechanism = dict(coverage_observed=isinstance(count, int) and count > 0,
                             scheduling_or_dependency_change_observed=None,
                             skipped_upload_preparations=count,
                             limitation="Coverage proves only skipped preparation; dependencies "
                                        "and readback policy remain unchanged. Frame times "
                                        "must independently demonstrate any benefit.")
            if count is None:
                issues.append("Written synchronization preparation counter missing.")
        elif candidate in ("recorder-spin", "reg-dump-fast", "particle-poll-pause", "c73-scan"):
            mechanism = dict(coverage_observed=not issues,
                             scheduling_or_dependency_change_observed=None,
                             limitation="The receipt verifies policy. CPU usage and frame times establish the measured effect.")
        else:
            mechanism = dict(release_delta=delta, coverage_observed=delta.get(coverage, 0) > 0,
                             scheduling_or_dependency_change_observed=delta.get(effect, 0) > 0)
            if coverage not in delta or effect not in delta:
                issues.append("Lightweight mechanism coverage counters missing.")
        for key in ("draws_per_frame", "dispatches_per_frame"):
            av, bv = (t.get("scene_before", {}).get(key) for t in trials)
            if av is None or bv is None or av <= 0:
                issues.append(f"Missing valid baseline {key}.")
            else:
                drift[key] = 100 * (bv / av - 1)
                if abs(drift[key]) > 10:
                    issues.append(f"Baseline {key} drift exceeds 10%; scenes not comparable.")
        for key in ("baseline", "super_0_12", "early", "peak", "late", "recovery", "tail"):
            av, bv = (t.get("windows", {}).get(key, {}) for t in trials)
            windows[key] = dict(A=av, B=bv, raw_fps_change_percent=(
                100 * (bv["fps"] / av["fps"] - 1) if av.get("fps") and bv.get("fps") else None))
    if candidate == 'runtime-context':
        rc = b.get('runtime_context_receipt', {})
        mechanism.update(runtime_context_receipt=rc,
                         limitation='Runtime construction only; current descriptors, selection inputs, '
                                    'pipeline keys, hazards and GPU work are still evaluated. '
                                    'Hits or validation counts do not imply FPS benefit.')
    return dict(data_checks_passed=not issues, issues=issues, baseline_drift_percent=drift,
                environment_observation_required=require_environment,
                mechanism=mechanism,
                windows=windows, scene_review_required=True, performance_acceptance=False,
                limitation="Two releases give no repeatability interval. Workload checks do not prove image/world equivalence. Check mechanism counters, normal rendering and tails before accepting gains.")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("index", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    index = load(args.index)
    if (index.get("status") != "collected" or index.get("candidate") not in
            ("feedback-checkpoint", "feedback-writers", "feedback-word", "feedback-word-prefetch", "feedback-word-bulk", "feedback-page-words", "feedback-async", "gds-counter", "cpu-placement", "compute-recorder", "recorder-spin", "reg-dump-fast", "particle-poll-pause", "c73-scan", "written-sync", "image-sync-once", "access-union", "readback-copy-stage", "image-read-transition", "buffer-prep-union", "feedback-arena", "vertex-input-key", "texture-binding-reuse", "feedback-arena-group", "feedback-arena-pool", "feedback-arena-tiled", "runtime-context", *RECENT_CANDIDATES) or index.get("release_count") != 2 or
            len(index.get("sessions", [])) != 2):
        raise ValueError("Expected one complete independent feedback pair.")
    records = baseline_order(index)
    root = Path(__file__).resolve().parents[1]
    results = [analyze(root / r["session"]) for r in records]
    if index['candidate'] in ('access-union', 'image-read-transition', 'buffer-prep-union'):
        prefix = ('access_union' if index['candidate'] == 'buffer-prep-union'
                  else index['candidate'].replace('-', '_'))
        for result, record in zip(results, records):
            directory = root / record['session']
            cycles = load(directory / 'super-rounds.json')
            samples = []
            for cycle in cycles:
                samples += [cycle.get(k, {}) for k in ('filled', 'before', 'after')]
                samples += [e for e in load(directory / cycle['events_file']) if 'draws_per_frame' in e]
            def consistent(key):
                values = [e.get(key) for e in samples]
                return values[0] if values and all(type(v) is bool and v == values[0] for v in values) else None
            result[prefix + '_receipt'] = dict(
                requested=load(directory / 'launch.json').get(prefix + '_candidate'),
                enabled=consistent(prefix + '_enabled'), used=consistent(prefix + '_used'))
    if index["candidate"] == "readback-copy-stage":
        for result, record in zip(results, records):
            directory = root / record["session"]
            cycle, = load(directory / "super-rounds.json")
            launch = load(directory / "launch.json")
            values = [cycle[k].get("readback_copy_stage_enabled") for k in ("before", "after")]
            result["readback_copy_stage_receipt"] = dict(
                requested=launch.get("readback_copy_stage_candidate"),
                enabled=values[0] if all(type(v) is bool and v == values[0] for v in values) else None)
    if index["candidate"] == "feedback-arena":
        for result, record in zip(results, records):
            directory = root / record["session"]
            cycle, = load(directory / "super-rounds.json")
            launch = load(directory / "launch.json")
            before, after = (cycle[k] for k in ("before", "after"))
            starts, ends = (sample.get("feedback_arena_moves") for sample in (before, after))
            valid = all(isinstance(v, list) and len(v) == 2 and
                        all(type(n) is int and n >= 0 for n in v) for v in (starts, ends))
            result["feedback_arena_receipt"] = dict(
                requested=launch.get("feedback_arena_candidate"),
                window_verified=all(s.get("readback_window_kb") == 512 for s in (before, after)),
                moves_before_release=starts if valid else None,
                moved_during_release=[y - x for x, y in zip(starts, ends)] if valid else None)
    if index["candidate"] in ("feedback-arena-pool", "feedback-arena-tiled", "runtime-context"):
        for result, record in zip(results, records):
            directory = root / record["session"]
            cycle, = load(directory / "super-rounds.json")
            launch = load(directory / "launch.json")
            tiled = index["candidate"] == "feedback-arena-tiled"
            active = tiled or index["candidate"] == "runtime-context" or not record["baseline"]
            tiled_startup = tiled and not record["baseline"]
            group_records = 64 if tiled_startup else 2048
            startup_key = "feedback_arena_tiled_candidate" if tiled_startup else "feedback_arena_pool_candidate"
            opposite_key = "feedback_arena_pool_candidate" if tiled_startup else "feedback_arena_tiled_candidate"
            result["feedback_arena_pool_receipt"] = dict(
                requested=launch.get(startup_key),
                expected_records_per_window=group_records,
                compact_layout_verified=all(cycle[phase].get("feedback_arena_pool_capacity") == 2048 and
                                            cycle[phase].get("feedback_arena_pool_stride") == 256 and
                                            cycle[phase].get("feedback_arena_pool_group_records",
                                                2048 if active else 0) == (group_records if active else 0)
                                            for phase in ("before", "after")) and
                                            launch.get(opposite_key, False) is False,
                **{phase: {key: cycle[phase].get("feedback_arena_pool_" + key)
                           for key in ("moves", "overflows", "busy", "used", "resets")}
                   for phase in ("before", "after")})
    if index["candidate"] == 'runtime-context':
        for result, record in zip(results, records):
            cycle, = load(root / record['session'] / 'super-rounds.json')
            result['runtime_context_receipt'] = dict(
                enabled=cycle['before'].get('runtime_context_memo'),
                same_switch=cycle['before'].get('runtime_context_memo') is
                            cycle['after'].get('runtime_context_memo'),
                **{p: cycle[p].get('runtime_context_counts') for p in ('before', 'after')})
    if index["candidate"] == "feedback-arena-group":
        for result, record in zip(results, records):
            directory = root / record["session"]
            cycle, = load(directory / "super-rounds.json")
            launch = load(directory / "launch.json")
            result["feedback_arena_group_receipt"] = dict(
                requested=launch.get("feedback_arena_group_candidate"),
                before=cycle["before"].get("feedback_arena_group_moves"),
                after=cycle["after"].get("feedback_arena_group_moves"))
    if index["candidate"] in ("vertex-input-key", "texture-binding-reuse"):
        for result, record in zip(results, records):
            directory = root / record["session"]
            cycle, = load(directory / "super-rounds.json")
            launch = load(directory / "launch.json")
            key = index["candidate"].replace('-', '_')
            counter = "vertex_input_key_hits" if key == "vertex_input_key" else "texture_binding_reuses"
            result[key + "_receipt"] = dict(
                requested=launch.get(key + "_candidate"),
                before=cycle["before"].get(counter),
                after=cycle["after"].get(counter))
    if index["candidate"] == "particle-poll-pause":
        for trial, record in zip(results, records):
            log = (root / record["session"] / "log" / "CUSA00309.log").read_text(
                encoding="utf-8", errors="replace")
            trial["particle_poll_pause_loaded"] = (
                "Particle poll PAUSE experiment: verified call replacement" in log)
    if index["candidate"] == "c73-scan":
        for trial, record in zip(results, records):
            log = (root / record["session"] / "log" / "CUSA00309.log").read_text(
                encoding="utf-8-sig", errors="replace")
            trial["c73_scan_loaded"] = "C73 scan experiment: verified SPIR-V replacement" in log
    mask = None
    if index["candidate"] == "cpu-placement":
        cores = index["cpu_topology"]["cores"]
        classes = {c["efficiency_class"] for c in cores}
        if len(classes) < 2 or any(c["group"] != 0 for c in cores):
            raise ValueError("Expected enumerated hybrid topology in one processor group.")
        highest = max(classes)
        all_mask = 0
        mask = 0
        for c in cores:
            part = int(c["mask_hex"], 16)
            if part <= 0 or part >= 2**64 or all_mask & part:
                raise ValueError("Invalid or overlapping core masks.")
            all_mask |= part
            if c["efficiency_class"] == highest:
                mask |= part
        if not 0 < mask < all_mask or mask != int(index["performance_core_mask_hex"], 16):
            raise ValueError("Performance-core mask does not match captured topology.")
    result = qualify(*results, index["candidate"], index["profile"], mask,
                     index.get("normal_rendering", False), index.get('observe_environment', False))
    result.update(candidate=index["candidate"], sessions=results,
                  normal_rendering=index.get("normal_rendering", False),
                  capture_order=index.get("capture_order", "AB"),
                  analysis_order="A baseline, B candidate, independent of capture order")
    args.output.write_text(json.dumps(result, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print("Data checks:", "passed; image/mechanism review pending" if result["data_checks_passed"] else "excluded")
    for issue in result["issues"]:
        print(issue)


if __name__ == "__main__":
    main()
