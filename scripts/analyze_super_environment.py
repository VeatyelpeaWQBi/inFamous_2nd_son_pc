"""Read-only native external-load qualification; never launches anything."""
import json
import math
from pathlib import Path

POLICY = dict(nongame_cpu_core_equiv=1.0, unrelated_gpu_engine_percent=10.0,
              disk_bytes_per_second=20*1024*1024, disk_average_queue=1.0,
              preflight_idle_seconds=60)


def finite(value):
    return type(value) in (int, float) and math.isfinite(value) and value >= 0


def quiet(row, preflight=False):
    gpu = row.get('gpu', {})
    fields = (row.get('nongame_core_equiv'), gpu.get('unrelated_max_engine_percent'),
              row.get('disk_bytes_per_second'), row.get('disk_average_queue'),
              row.get('read_wall_ms'), row.get('monitor_cpu_ms'))
    available = (row.get('cpu_available') is True and gpu.get('available') is True and gpu.get('classifier_available') is True and
                 row.get('disk_available') is True and all(finite(n) for n in fields))
    if not available:
        return None
    limit = (1.0, 10.0, 20*1024*1024, 1.0, 200, 40)
    result = all(value <= bound for value, bound in zip(fields, limit))
    if preflight:
        idle = row.get('last_input_idle_seconds')
        result = result and finite(idle) and idle >= 60
    return result


def evaluate(rows, meta, pid, release_qpc):
    if (meta.get('schema') != 2 or meta.get('complete') is not True or
            meta.get('sample_count') != len(rows) or not 3 <= len(rows) <= 2048 or
            meta.get('target_pid') != pid or type(pid) is not int or pid <= 0 or
            meta.get('policy') != POLICY or meta.get('qpc_frequency', 0) <= 0):
        raise ValueError('Incomplete, mismatched or unknown environment observation')
    frequency = meta['qpc_frequency']
    prior = None
    target_seen = False
    for seq, row in enumerate(rows):
        clock_fields = ('qpc_before', 'qpc_cpu', 'qpc_after', 'interval_start_qpc')
        if (row.get('schema') != 2 or row.get('sequence') != seq or
                row.get('qpc_frequency') != frequency or
                any(type(row.get(k)) is not int or row[k] < 0 for k in clock_fields) or
                not row['interval_start_qpc'] < row['qpc_cpu'] <= row['qpc_after'] or
                not row['qpc_before'] <= row['qpc_cpu'] or row.get('target_pid') not in (0, pid) or
                (target_seen and row.get('target_pid') != pid)):
            raise ValueError('Discontinuous environment clock, sequence or owned process')
        if prior and (row['qpc_before'] < prior['qpc_after'] or
                      (prior['target_pid'] == row['target_pid'] and
                       row['interval_start_qpc'] != prior['qpc_cpu'])):
            raise ValueError('Environment observation overlapped or skipped an interval')
        actual = (row['qpc_cpu']-row['interval_start_qpc'])/frequency
        if (not finite(row.get('interval_seconds')) or
                not math.isclose(actual, row['interval_seconds'], abs_tol=1e-8)):
            raise ValueError('Environment interval duration mismatch')
        if row.get('cpu_available') is True:
            system, game, external = (row.get(k) for k in
                                      ('system_busy_core_equiv', 'game_core_equiv', 'nongame_core_equiv'))
            if (not all(finite(n) for n in (system, game, external)) or
                    game > system+.1 or
                    not math.isclose(external, max(0, system-game), abs_tol=1e-8) or
                    row.get('processor_groups') != 1 or not .5 <= actual <= 10):
                raise ValueError('CPU availability flag contradicts time accounting')
        target_seen |= row['target_pid'] == pid
        prior = row
    preflight = [r for r in rows if r.get('phase') == 'preflight' and r['target_pid'] == 0]
    preflight_ok = len(preflight) >= 3 and all(quiet(r, True) is True for r in preflight[-3:])
    windows, issues = {}, []
    if not preflight_ok:
        issues.append('No three consecutive quiet, idle desktop samples before launch')
    for name, lo, hi in (('baseline', -10, 0), ('early', 0, 3), ('peak', 3, 8),
                         ('late', 8, 12), ('recovery', 12, 25), ('tail', 25, 30)):
        left, right = release_qpc+lo*frequency, release_qpc+hi*frequency
        selected = [(r, max(0, min(right,r['qpc_cpu'])-max(left,r['interval_start_qpc']))/frequency)
                    for r in rows if r['target_pid'] == pid and r['qpc_cpu'] > left and
                    r['interval_start_qpc'] < right]
        covered = sum(w for r,w in selected)
        unknown = sum(w for r,w in selected if quiet(r) is None or r['interval_seconds'] > 3.5)
        busy = sum(w for r,w in selected if quiet(r) is False)
        def mean(key, nested=False):
            valid = [((r.get('gpu', {}) if nested else r).get(key), w) for r,w in selected]
            valid = [(v,w) for v,w in valid if finite(v)]
            return sum(v*w for v,w in valid)/sum(w for v,w in valid) if valid else None
        windows[name] = dict(covered_seconds=covered, unknown_seconds=unknown, busy_seconds=busy,
                             nongame_cpu_mean_core_equiv=mean('nongame_core_equiv'),
                             unrelated_gpu_mean_max_engine_percent=mean('unrelated_max_engine_percent', True),
                             compositor_gpu_mean_max_engine_percent=mean('compositor_max_engine_percent', True),
                             disk_mean_bytes_per_second=mean('disk_bytes_per_second'))
        if not math.isclose(covered, hi-lo, abs_tol=.001) or unknown > .001 or busy > .001:
            issues.append(name+': load high, unavailable, too sparsely sampled or incomplete')
    return dict(qualified=not issues, issues=issues, preflight_quiet=preflight_ok,
                windows=windows, performance_acceptance=False,
                limitation='Conservative screening at two-second resolution, not proof of zero interference. '
                           'GPU includes all adapters; disk pressure has no process attribution. '
                           'No thermal/power/clock telemetry and no correction of FPS by background load.')


def analyze(directory: Path, launch, release_qpc):
    try:
        meta = json.loads((directory/'environment.json').read_text(encoding='utf-8-sig'))
        path = directory/'environment-samples.jsonl'
        if path.stat().st_size > 2*1024*1024:
            raise ValueError('Environment file exceeds its budget')
        rows = [json.loads(line) for line in path.read_text(encoding='utf-8-sig').splitlines()]
        return evaluate(rows, meta, launch['pid'], release_qpc)
    except (OSError, ValueError, KeyError, TypeError) as e:
        return dict(qualified=False, issues=[str(e)], performance_acceptance=False)
