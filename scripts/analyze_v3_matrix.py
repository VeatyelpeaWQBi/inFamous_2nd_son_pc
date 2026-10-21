# SPDX-License-Identifier: GPL-2.0-or-later
"""Validate frozen V3 ordinary A/B/A evidence; never certify an FPS gain automatically."""
import argparse
from collections import defaultdict
import json
import re
from pathlib import Path
from analyze_ordinary import observation_summary, read_json
from metrics import summarize
from launcher_settings import resolution_config
from project_paths import ROOT, project_child, relative_path, write_json

NAMES = ('descriptor-delta', 'layout-sharing', 'lighting-reduction', 'lighting-cse',
         'lighting-vector', 'lighting-schedule', 'particle-schedule', 'particle-async',
         'demand-dependencies', 'page-batches', 'window-prefetch', 'sort-write-bounds',
         'submit-prepare', 'recording-2', 'recording-4', 'copy-coalesce',
         'completion-elision', 'params-lookup-off', 'flatbuf-reuse-off')
KEYS = ('v3_descriptor_delta', 'v3_layout_sharing', 'v3_lighting_reduction', 'v3_lighting_cse',
        'v3_lighting_vector', 'v3_lighting_schedule', 'v3_particle_schedule', 'v3_particle_async',
        'v3_demand_readback', 'v3_prefetch_batches', 'v3_window_prefetch', 'v3_sort_write_bounds',
        'v3_submit_prepare', 'v3_recording_workers', 'v3_recording_workers', 'v3_copy_coalesce',
        'v3_completion_elision', 'v3_disable_params_lookup', 'v3_disable_flatbuf_reuse')
CATALOG = dict(zip(NAMES, KEYS))
BASELINE = dict.fromkeys(KEYS, 0)


def selections(case):
    if case == 'baseline-pilot': return []
    if case in ('combined-2', 'combined-4'):
        return [name for name in NAMES if not name.endswith('-off') and not name.startswith('recording-')] + ['recording-' + case[-1]]
    if case not in CATALOG: raise ValueError('Unknown matrix case')
    return [case]


def expected_overrides(candidates):
    result = BASELINE.copy()
    if len(candidates) != len(set(candidates)) or {'recording-2', 'recording-4'} <= set(candidates):
        raise ValueError('Repeated or conflicting candidates')
    for name in candidates:
        if name not in CATALOG: raise ValueError('Unknown candidate')
        result[CATALOG[name]] = int(name[-1]) if name.startswith('recording-') else 1
    return result


def save_signature(files):
    if not files: raise ValueError('Missing frozen save identity')
    signature = sorted((r['path'], r['bytes'], r['sha256']) for r in files)
    if len({r[0] for r in signature}) != len(signature): raise ValueError('Repeated save path')
    return signature


def validate_launch(plan, row, launch):
    candidates = selections(row['case']) if row['role'] == 'candidate' else []
    expected = expected_overrides(candidates)
    if row['expected_overrides'] != expected or row['candidates'] != candidates:
        raise ValueError('Plan selection and expected flags disagree')
    for field in ('runtime_sha256', 'game_sha256', 'mode'):
        if launch.get(field) != plan[field]: raise ValueError('Changed identity: ' + field)
    if (launch.get('session') != row['session'] or not launch.get('game_launched') or
            not launch.get('complete') or launch.get('exit_code') != 0 or
            not launch.get('audio_muted') or launch.get('diagnostic') or launch.get('timing_profile') or
            launch.get('optimization_v3') != ('custom' if candidates else 'control') or
            launch.get('v3_candidates') != candidates):
        raise ValueError('Incomplete or inconsistent ordinary muted session')
    overrides = launch['v3_overrides']
    if any(overrides.get(key) != value for key, value in expected.items()):
        raise ValueError('Actual candidate flags disagree')
    for key in ('gpu_profile', 'perf_log', 'profile_stacks', 'path_trace',
                'consumer_evidence_limit', 'auxiliary_evidence_limit'):
        if overrides.get(key) != 0: raise ValueError('Heavy instrumentation enabled: ' + key)
    if save_signature(launch.get('save_source_files')) != save_signature(plan['save_source_files']):
        raise ValueError('Fixed source save changed')
    if launch.get('launcher_settings') != plan['launcher_settings']:
        raise ValueError('Launcher/display settings changed')
    width, height, _, _ = resolution_config(plan['mode'])
    if launch.get('resolution') != {'width': width, 'height': height}:
        raise ValueError('Resolution changed')


def comparison(before, candidate, after):
    """Two controls bound drift; this reports measurements, not causal certification."""
    low, high = sorted((before['fps'], after['fps']))
    return {'fps_control_range': [low, high],
            'control_fps_spread_percent': 100 * (high / low - 1),
            'control_drift_over_3_percent': high / low > 1.03,
            'candidate_fps': candidate['fps'],
            'candidate_fps_change_percent_vs_controls':
                [100 * (candidate['fps'] / high - 1), 100 * (candidate['fps'] / low - 1)],
            'tail_change_ms_vs_controls': {
                key: [candidate[key] - before[key], candidate[key] - after[key]]
                for key in ('p95_ms', 'p99_ms', 'max_ms')},
            'performance_acceptance': False, 'visual_acceptance': False}


def mechanism_evidence(session, candidates):
    # The isolated native logger switches from shadps4.log to the title log.
    # Use the legacy filename only when no title log exists; never scan other titles.
    directory = project_child(relative_path(session / 'log'))
    game_log = project_child(relative_path(directory / 'CUSA00309.log'))
    legacy_log = project_child(relative_path(directory / 'shad_log.txt'))
    boot_log = project_child(relative_path(directory / 'shadps4.log'))
    paths = [p for p in (boot_log, game_log if game_log.is_file() else legacy_log) if p.is_file()]
    game_log_available = game_log.is_file() or legacy_log.is_file()
    markers = {'recording': 'V3 complete-session recorder active:',
               'recording_fallback': 'V3 complete-session recorder fallback:',
               'copy-coalesce': 'V3 copy regions coalesced:',
               'completion-elision': 'V3 readback completion elided:'}
    observed = dict.fromkeys(markers, False)
    issues = {'warning': 0, 'error': 0, 'critical': 0}
    signatures = {'error': defaultdict(int), 'critical': defaultdict(int)}
    recording_nonempty = False
    diagnostic = re.compile(r'^\[([^\]]+)\] <(Error|Critical)> \([^)]*\) (?:\S+:\d+ )?(.*)$')
    for path in paths:
        project_child(relative_path(path))
        with path.open(encoding='utf-8-sig', errors='replace') as stream:
            for line in stream:
                for level in issues:
                    if '<' + level.capitalize() + '>' in line: issues[level] += 1
                for key, marker in markers.items():
                    observed[key] |= marker in line
                match = diagnostic.match(line.rstrip('\r\n'))
                if match:
                    signatures[match[2].lower()][match[1] + ': ' + match[3]] += 1
                if markers['recording'] in line:
                    count = re.search(r'\bcommands=(\d+)\b', line)
                    recording_nonempty |= bool(count and int(count[1]) > 0)
    relevant = {}
    for name in candidates:
        key = 'recording' if name.startswith('recording-') else name
        relevant[name] = {'activation_log_observed': observed[key] if key in observed and game_log_available else None,
                          'measurement_window_activation_confirmed': False,
                          'evidence_scope': 'process-first-hit-only'}
        if key == 'recording':
            relevant[name]['fallback_observed'] = observed['recording_fallback'] if game_log_available else None
            relevant[name]['nonempty_session_observed'] = recording_nonempty if game_log_available else None
    return {'candidates': relevant, 'runtime_log_counts': issues,
            'runtime_issue_signatures': {k: dict(v) for k, v in signatures.items()},
            'log_paths': [relative_path(p) for p in paths],
            'log_available': game_log_available, 'runtime_issues_accepted': False}


def analyze(plan):
    if (plan.get('schema') not in (1, 2) or plan.get('kind') != 'v3-ordinary-independent-and-combined' or
            not plan.get('audio_muted') or plan.get('full_collectors') or
            plan.get('automatic_performance_acceptance')):
        raise ValueError('Unsupported matrix policy')
    rows, cases = [], defaultdict(dict)
    seen_sessions = set()
    for index, row in enumerate(plan['rows']):
        if row['index'] != index or row['role'] not in ('control-before', 'candidate', 'control-after'):
            raise ValueError('Invalid matrix index/role')
        if row['role'] in cases[row['case']]: raise ValueError('Repeated case/role')
        cases[row['case']][row['role']] = None
        if row['status'] not in ('collected','validated'): continue
        if not row['session'] or row['session'] in seen_sessions: raise ValueError('Missing/reused session')
        seen_sessions.add(row['session'])
        session = project_child(row['session'])
        if not session.is_relative_to(project_child('state/sessions')): raise ValueError('Session outside evidence root')
        launch = read_json(session / 'launch.json')
        validate_launch(plan, row, launch)
        sensors = observation_summary(session)
        start, duration = sensors['window']['start_s'], sensors['window']['duration_s']
        if duration != plan['measure_seconds']: raise ValueError('Observation duration changed')
        if plan['schema'] >= 2:
            observation=read_json(session / 'ordinary-observer/observation.json')
            if observation.get('observer_revision',0)<3 or observation.get('scene_key')!=plan['scene_key'] or observation['warmup_seconds']!=plan['warmup_seconds']:
                raise ValueError('Scene gate or warmup policy changed')
            minimum=max(2,duration/2)
            if sensors['cpu']['samples']<minimum or not sensors['gpu'] or any(g['samples']<minimum for g in sensors['gpu']):
                raise ValueError('Insufficient usable ordinary sensor coverage')
        frames = summarize(session / 'frames.csv', start, start + duration, plan['mode'], {})
        if not sensors['complete']: raise ValueError('Incomplete ordinary observer')
        result = {'index': index, 'case': row['case'], 'role': row['role'], 'session': row['session'],
                  'frames': frames, 'conditions': sensors,
                  'mechanisms': mechanism_evidence(session, row['candidates'])}
        rows.append(result)
        cases[row['case']][row['role']] = frames
    groups = []
    for name, roles in cases.items():
        if set(roles) != {'control-before', 'candidate', 'control-after'}: raise ValueError('Missing A/B/A role')
        group = {'case': name, 'complete': all(value is not None for value in roles.values())}
        if group['complete']:
            group['comparison'] = comparison(roles['control-before'], roles['candidate'], roles['control-after'])
            if name == 'baseline-pilot':
                low=min(value['fps'] for value in roles.values()); high=max(value['fps'] for value in roles.values())
                group['comparison']['fps_control_range']=[low,high]
                group['comparison']['control_fps_spread_percent']=100*(high/low-1)
                group['comparison']['control_drift_over_3_percent']=high/low>1.03
        groups.append(group)
    collected_all = len(rows) == len(plan['rows']) and bool(rows)
    if plan['complete'] and not collected_all: raise ValueError('Matrix completion claim lacks all rows')
    return {'schema': 1, 'analysis_revision': 2, 'runtime_sha256': plan['runtime_sha256'], 'mode': plan['mode'],
            'collection_complete': bool(plan['complete'] and collected_all),
            'collected_runs': len(rows), 'planned_runs': len(plan['rows']), 'failure': plan.get('failure'),
            'rows': rows, 'groups': groups, 'performance_acceptance': False,
            'limits': ['Ordinary guest flips and low-rate sensors; no full collector overhead in FPS comparisons.',
                       'Images, scene/camera/input, brightness, particles and native resolution require review.',
                       'Missing activation logs mean unconfirmed execution; combined features can override or bypass one another.',
                       'A/B/A controls show drift, not causality. A single group does not prove a stable whole-game gain.']}


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--plan', type=Path, required=True)
    args = parser.parse_args()
    source = project_child(relative_path(args.plan) if args.plan.is_absolute() else args.plan)
    if not source.is_relative_to(project_child('state/runtime')): parser.error('Matrix plan must be under state/runtime')
    result = analyze(read_json(source))
    write_json(source.with_name(source.stem + '-summary.json'), result)
    print(json.dumps({'planned_runs': result['planned_runs'], 'collected_runs': result['collected_runs'],
                      'collection_complete': result['collection_complete'], 'performance_acceptance': False}))
