# SPDX-License-Identifier: GPL-2.0-or-later
"""Qualify an explicit cross-binary compiler experiment; never launch anything."""
import argparse
import csv
import copy
import json
import re
import statistics
from pathlib import Path

from analyze_super_batch import analyze, load

UNITS = {'src/video_core/renderer_vulkan/vk_pipeline_cache.cpp',
         'src/video_core/renderer_vulkan/vk_rasterizer.cpp',
         'src/video_core/buffer_cache/buffer_cache.cpp'}
PGO_UNITS = UNITS | {'src/video_core/amdgpu/liverpool.cpp', 'src/video_core/page_manager.cpp',
                     'src/video_core/renderer_vulkan/vk_runtime.cpp'}


def check_compiler_inputs(a, b, *additional_keys):
    for key in ('configuration', 'source_sha256', 'compiler', 'compiler_sha256',
                'normalized_commands_sha256', 'generated_headers', 'precision_policy',
                *additional_keys):
        if key not in a or key not in b or a[key] != b[key] or (key != 'generated_headers' and not a[key]):
            raise ValueError(f'Uncontrolled build difference: {key}')
    if a['configuration'] != 'RelWithDebInfo' or a['precision_policy'] != 'unchanged':
        raise ValueError('Configuration or numeric policy changed')
    for manifest in (a, b):
        if not manifest.get('runtime_sha256') or not manifest.get('pdb_sha256'):
            raise ValueError('Missing runtime / PDB identity')
    if a['runtime_sha256'] == b['runtime_sha256']:
        raise ValueError('Compiler experiment produced the same runtime binary')


def check_build(a, b):
    if a.get('candidate') != 'renderer-inline' or b.get('candidate') != 'renderer-inline':
        raise ValueError('Not an explicit renderer compiler experiment')
    if a.get('enabled') is not False or b.get('enabled') is not True:
        raise ValueError('Expected baseline / candidate compiler manifests')
    check_compiler_inputs(a, b)
    for manifest in (a, b):
        entries = manifest.get('renderer_commands', [])
        if {r['file'].replace('\\', '/') for r in entries} != UNITS or len(entries) != 3:
            raise ValueError('Unexpected renderer unit set')
    baseline = {r['file'].replace('\\', '/'): r for r in a['renderer_commands']}
    for row in b['renderer_commands']:
        old = baseline[row['file'].replace('\\', '/')]
        if row['source_sha256'] != old['source_sha256']:
            raise ValueError('Renderer source changed between builds')
        if len(re.findall(r' /Ob2(?= )', row['command'])) != 1:
            raise ValueError('Candidate inlining flag not applied exactly once')
        if re.sub(r' /Ob2(?= )', '', row['command']) != old['command']:
            raise ValueError('Compiler arguments changed beyond the scoped inlining flag')


def check_pgo_build(a, b, profile_sha256):
    if (a.get('renderer_pgo'), b.get('renderer_pgo')) != ('Off', 'Use'):
        raise ValueError('Expected normal and profile-guided renderer builds')
    if any(m.get('enabled') is not False for m in (a, b)):
        raise ValueError('The unrelated renderer inlining variant must be off')
    if (not re.fullmatch(r'[0-9a-f]{64}', profile_sha256 or '')
            or b.get('pgo_data_sha256') != profile_sha256 or a.get('pgo_data_sha256')):
        raise ValueError('Profile identity does not match the recorded training data')
    check_compiler_inputs(a, b, 'generated_revision_sha256')
    tables = []
    for manifest in (a, b):
        entries = manifest.get('pgo_commands', [])
        table = {row['file'].replace('\\', '/'): row['command'] for row in entries}
        if len(entries) != len(PGO_UNITS) or table.keys() != PGO_UNITS:
            raise ValueError('Unexpected PGO compilation unit set')
        tables.append(table)
    flag = r' (?:"/clang:-fprofile-use=[^"]+"|/clang:-fprofile-use="[^"]+"|/clang:-fprofile-use=\S+)(?= )'
    for file in sorted(PGO_UNITS):
        command, count = re.subn(flag, '', tables[1][file])
        command, warning_count = re.subn(r' /clang:-Werror=backend-plugin(?= )', '', command)
        if count != 1 or warning_count != 1 or command != tables[0][file] or '-fprofile-' in command:
            raise ValueError(f'Unexpected PGO compiler argument difference in {file}')


def clock_check(session):
    with (session / 'frames.csv').open(encoding='utf-8-sig', newline='') as handle:
        rows = list(csv.DictReader(handle))
    if len(rows) < 600:
        raise ValueError('Insufficient continuous frame evidence')
    slopes = []
    differences = []
    for old, new in zip(rows, rows[1:]):
        interval = int(new['interval_ns'])
        delta = int(new['utc_ns']) - int(old['utc_ns'])
        if int(new['frame']) != int(old['frame']) + 1 or interval <= 0 or delta <= 0:
            raise ValueError('Discontinuous frame stream')
        if int(new['qpc_before']) > int(new['qpc_after']):
            raise ValueError('Invalid frame QPC bracket')
        qpc = int(new['qpc_before']) - int(old['qpc_before'])
        if qpc <= 0 or abs(delta - interval) > 5_000_000:
            raise ValueError('Frame timestamp discontinuity exceeds 5 ms')
        slopes.append(delta / qpc)
        differences.append((qpc, delta))
    slope = statistics.median(slopes)
    if max(abs(qpc * slope - delta) for qpc, delta in differences) > 5_000_000:
        raise ValueError('QPC / UTC discontinuity exceeds 5 ms')
    return {'rows': len(rows), 'qpc_utc_ns_per_tick_observed': slope}


def self_test():
    a = dict(candidate='renderer-inline', enabled=False, configuration='RelWithDebInfo',
             source_sha256='source', compiler='installed-clang', compiler_sha256='compiler',
             normalized_commands_sha256='commands', generated_headers=[], precision_policy='unchanged',
             runtime_sha256='A', pdb_sha256='A-pdb', renderer_commands=[
                 dict(file=name, command=f'clang /O2 /Ob1 -c -- {name}', source_sha256=name) for name in sorted(UNITS)])
    b = copy.deepcopy(a)
    b.update(enabled=True, runtime_sha256='B', pdb_sha256='B-pdb')
    for row in b['renderer_commands']:
        row['command'] = row['command'].replace(' -c -- ', ' /Ob2 -c -- ')
    check_build(a, b)
    for key, value in [('source_sha256', 'changed'), ('compiler_sha256', 'other compiler'),
                       ('generated_headers', [{'file': 'new header'}]), ('normalized_commands_sha256', 'extra flags')]:
        bad = copy.deepcopy(b); bad[key] = value
        try:
            check_build(a, bad)
        except ValueError:
            pass
        else:
            raise AssertionError(f'Uncontrolled {key} admitted')
    bad = copy.deepcopy(b); bad['renderer_commands'][0]['command'] += ' /fp:fast'
    try:
        check_build(a, bad)
    except ValueError:
        pass
    else:
        raise AssertionError('Uncontrolled precision flag admitted')
    print('Compiler comparison checks passed: scoped flags accepted; changed source/compiler/header/other arguments rejected.')


def qualify(index, root):
    candidate = index.get('candidate')
    if index.get('status') != 'collected' or candidate not in ('renderer-inline', 'renderer-pgo'):
        raise ValueError('Expected a complete explicit compiler pair')
    records = index.get('trials', [])
    if [r['label'] for r in records] != list(index.get('order', '')) or set(index.get('order', '')) != {'A', 'B'} or len(records) != 2:
        raise ValueError('Compiler pair order / trial count is invalid')
    by_label = {r['label']: r for r in records}
    manifests = [load(root / by_label[k]['compiler_manifest']) for k in ('A', 'B')]
    if candidate == 'renderer-pgo':
        check_pgo_build(*manifests, index.get('profile_sha256'))
    else:
        check_build(*manifests)
    results = [analyze(root / by_label[k]['session']) for k in ('A', 'B')]
    issues = []
    launches = [load(root / by_label[k]['session'] / 'launch.json') for k in ('A', 'B')]
    for key in ('game_sha256', 'mode', 'resolution', 'timing_profile', 'timing_overrides'):
        if launches[0].get(key) != launches[1].get(key):
            issues.append(f'Different session {key}')
    if not results[0].get('game_sha256') or results[0]['game_sha256'] != results[1].get('game_sha256'):
        issues.append('Game identity differs or is missing')
    saves = [sorted((r['path'], r['bytes'], r['sha256']) for r in result.get('saves') or []) for result in results]
    if not saves[0] or saves[0] != saves[1]:
        issues.append('Original save snapshots differ')
    measured = []
    for label, record, manifest, result, launch in zip(('A', 'B'), (by_label['A'], by_label['B']), manifests, results, launches):
        if result['runtime_sha256'] != manifest['runtime_sha256'] or not result['complete'] or result['exit_code'] != 0:
            issues.append(f'{label}: runtime identity or clean launcher completion missing')
        if launch.get('progress_published') is not False or launch.get('source_game_written') is not False:
            issues.append(f'{label}: original save / game isolation unverified')
        if candidate == 'renderer-pgo' and launch.get('renderer_pgo') != manifest['renderer_pgo']:
            issues.append(f'{label}: launch did not attest to the requested PGO mode')
        cycles = load(root / record['session'] / 'super-rounds.json')
        trials = [t for t in result['trials'] if t['kind'] == 'measurement']
        if len(cycles) != 1 or len(trials) != 1:
            raise ValueError('Each compiler trial must contain exactly one release and no warmup')
        trial = trials[0]
        if trial['status'] != 'collected' or not trial['warm'] or not trial['meter_consumed'] or any(trial['compile_delta'].values()):
            issues.append(f'{label}: incomplete / cold release or new shader / pipeline compilation')
        if trial['profile'] != 'lightweight':
            issues.append(f'{label}: instrumentation differs from the intended lightweight trial')
        for snapshot in (cycles[0]['filled'], cycles[0]['before'], cycles[0]['after']):
            for key in ('asc_queue', 'compute_threaded_recording', 'command_buffer_sharps', 'compute_barrier_scope', 'written_sync_fastpath', 'image_sync_once', 'readback_word_page_cluster', 'gpu_profile', *(
                    ('access_union_enabled', 'access_union_used') if candidate == 'renderer-pgo' else ())):
                if snapshot.get(key) not in (False, 0):
                    issues.append(f'{label}: non-baseline / missing policy {key}')
        result['clock_check'] = clock_check(root / record['session'])
        measured.append(trial)
    if measured[0]['requested_tuning'] != measured[1]['requested_tuning']:
        issues.append('Runtime tuning differs between compiler builds')
    drift = {}
    for key in ('draws_per_frame', 'dispatches_per_frame'):
        a, b = (t['scene_before'].get(key) for t in measured)
        if not a or not b:
            issues.append(f'Missing baseline workload {key}')
        else:
            drift[key] = 100 * (b / a - 1)
            if abs(drift[key]) > 10:
                issues.append(f'Baseline workload drift exceeds 10%: {key}')
    windows = {}
    for key in ('baseline', 'early', 'peak', 'late', 'recovery', 'tail'):
        a, b = (t['windows'].get(key) for t in measured)
        if not a or not b:
            issues.append(f'Missing frame window {key}')
            continue
        windows[key] = {'A': a, 'B': b, 'raw_fps_change_percent': 100 * (b['fps'] / a['fps'] - 1)}
    frames = {label: t['frame_windows'].get('first_480') for label, t in zip(('A', 'B'), measured)}
    if not all(frames.values()):
        issues.append('Insufficient complete post-input frame intervals')
    return {'candidate': candidate, 'order': index['order'], 'data_checks_passed': not issues,
            'issues': issues, 'baseline_workload_drift_percent': drift, 'windows': windows,
            'first_480': frames,
            'sessions': results, 'performance_acceptance': False,
            'limitation': 'Explicit cross-binary compiler comparison, identical source/toolchain/normalized commands and inputs. Two releases do not prove repeatability or image equivalence.'}


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('index', nargs='?', type=Path)
    parser.add_argument('--check-build', nargs=2, type=Path)
    parser.add_argument('--check-pgo-build', nargs=3, metavar=('BASELINE', 'GUIDED', 'PROFILE_SHA256'))
    parser.add_argument('--output', type=Path)
    parser.add_argument('--self-test', action='store_true')
    args = parser.parse_args()
    if args.self_test:
        self_test()
    elif args.check_build:
        check_build(*(load(p) for p in args.check_build))
        print('Compiler manifest checks passed: exact source, toolchain, generated headers and only three /Ob2 additions.')
    elif args.check_pgo_build:
        a, b, digest = args.check_pgo_build
        check_pgo_build(load(Path(a)), load(Path(b)), digest)
        print('Renderer PGO manifest checks passed.')
    else:
        result = qualify(load(args.index), Path(__file__).resolve().parents[1])
        args.output.write_text(json.dumps(result, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
        print('Compiler pair data checks:', 'passed' if result['data_checks_passed'] else 'excluded')
        for issue in result['issues']:
            print(issue)
