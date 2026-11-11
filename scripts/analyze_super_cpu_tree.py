# SPDX-License-Identifier: GPL-2.0-or-later
"""Read existing VTune CPU exports; no native libraries, target control or collection."""
import argparse
import collections
import csv
import datetime
import hashlib
import json
import math
from pathlib import Path


def number(value):
    result = float(value or 0)
    if not math.isfinite(result) or result < 0:
        raise ValueError(f"Invalid nonnegative CPU duration: {value}")
    return result


def tree_rows(text):
    # VTune puts hierarchy spaces BEFORE a CSV-quoted first field. Preserve the
    # depth separately so template names containing commas remain valid CSV.
    lines = iter(text.splitlines())
    header = next(csv.reader([next(lines)]))
    if 'CPU Time:Self' not in header or 'Function Stack' not in header:
        raise ValueError('Not a VTune CPU top-down export')
    rows = []
    ancestors = []
    for line in lines:
        if not line.strip():
            continue
        depth = len(line) - len(line.lstrip(' '))
        fields = next(csv.reader([line[depth:]]))
        if len(fields) != len(header):
            raise ValueError('Malformed top-down CSV record')
        row = dict(zip(header, fields))
        if not row['Function Stack']:
            continue
        while ancestors and ancestors[-1]['depth'] >= depth:
            ancestors.pop()
        if depth != (ancestors[-1]['depth'] + 1 if ancestors else 0):
            raise ValueError('Discontinuous top-down hierarchy')
        row['depth'] = depth
        row['self_seconds'] = number(row['CPU Time:Self'])
        row['path'] = tuple(parent['Function Stack'] for parent in ancestors) + (row['Function Stack'],)
        rows.append(row)
        ancestors.append(row)
    if not rows or rows[0]['Function Stack'] != 'Total':
        raise ValueError('Missing unique top-down root')
    return rows


def summarize(rows, expected_cpu):
    total = sum(row['self_seconds'] for row in rows)
    if not math.isclose(total, expected_cpu, abs_tol=0.0002):
        raise ValueError(f'Tree/explicit PID+TID self totals disagree: {total} vs {expected_cpu}')
    inclusive = collections.Counter()
    self_time = collections.Counter()
    modules = collections.Counter()
    direct_contexts = collections.Counter()
    for row in rows:
        value = row['self_seconds']
        self_time[(row['Function Stack'], row.get('Module', ''))] += value
        modules[row.get('Module', '')] += value
        # Inclusive functions overlap. Recursion/identical inline names in a path
        # receive this sample once, never once per repeated frame.
        for name in set(row['path']):
            inclusive[name] += value
        context = 'other command processing / waits'
        for name in row['path']:
            if name.startswith('AmdGpu::Liverpool::ProcessCompute'):
                context = 'guest compute queue'; break
            if name == 'Vulkan::Rasterizer::Draw':
                context = 'graphics direct draw'; break
            if name in ('Vulkan::Rasterizer::DrawIndirect', 'Vulkan::Rasterizer::DrawIndirectCount'):
                context = 'graphics indirect draw'; break
            if name in ('Vulkan::Rasterizer::DispatchDirect', 'Vulkan::Rasterizer::DispatchIndirect'):
                context = 'graphics-queue compute dispatch'; break
            if name == 'Vulkan::Rasterizer::CopyBuffer':
                context = 'graphics-queue buffer copy'; break
        direct_contexts[context] += value
    def metric(name, value):
        return {'name': name, 'cpu_seconds': value, 'percent_cp_self': value * 100 / total}
    return {
        'tree_rows': len(rows), 'cp_self_seconds': total,
        'exclusive_contexts': [metric(name, value) for name, value in direct_contexts.most_common()],
        'modules': [metric(name, value) for name, value in modules.most_common()],
        'self_top': [dict(metric(name, value), module=module) for (name, module), value in self_time.most_common(30)],
        'inclusive_functions': [metric(name, value) for name, value in inclusive.most_common()],
        'limits': 'Inclusive rows overlap and cannot be summed. CPU sample time is not removable frame time; this diagnostic is not a performance A/B.'
    }


def analyze(directory):
    state = json.loads((directory / 'status.json').read_text(encoding='utf-8-sig'))
    if state['status'] != 'recovered' or not state['raw_unchanged']:
        raise ValueError('A verified recovered CPU result is required')
    output = {'session': state['session'], 'runtime_sha256': state['runtime_sha256'],
              'sampling_mode': state['sampling_mode'], 'target_pid': state['target_pid'], 'phases': {}}
    session = directory.parents[2]
    with (directory.parent / 'summary.csv').open(encoding='utf-8-sig', newline='') as handle:
        summary = {row['Metric Name']: row['Metric Value'] for row in csv.DictReader(handle)}
    start = datetime.datetime.strptime(summary['Collection start time'], '%H:%M:%S %d/%m/%Y UTC').replace(tzinfo=datetime.timezone.utc)
    start_ns = int(start.timestamp()) * 1_000_000_000
    with (session / 'frames.csv').open(encoding='utf-8-sig', newline='') as handle:
        frames = [(int(row['utc_ns']), int(row['interval_ns'])) for row in csv.DictReader(handle)]
    for phase in state['phases']:
        path = directory / (phase['name'] + '-cp-top-down.csv')
        if not path.is_file():
            continue
        rows = tree_rows(path.read_text(encoding='utf-8-sig'))
        result = summarize(rows, phase['cp_cpu_seconds'])
        result['cp_tid'] = phase['cp_tid']
        result['tree_sha256'] = hashlib.sha256(path.read_bytes()).hexdigest()
        result['window_seconds'] = phase['end_seconds'] - phase['begin_seconds']
        result['cp_cpu_per_wall_second'] = result['cp_self_seconds'] / result['window_seconds']
        lo = start_ns + round(phase['begin_seconds'] * 1_000_000_000)
        hi = start_ns + round(phase['end_seconds'] * 1_000_000_000)
        count = sum(1 for end, interval in frames if interval > 0 and lo <= end - interval and end <= hi)
        if count <= 0:
            raise ValueError('No complete frame intervals in the CPU analysis window')
        result['complete_intervals'] = count
        result['approx_cp_ms_per_interval'] = result['cp_self_seconds'] * 1000 / count
        result['normalization_limits'] = 'Approximate CPU sample budget divided by completed frame intervals, not a per-frame critical-path trace. VTune summary start has up to 1 s timestamp uncertainty; this is an instrumented CPU diagnostic, not lightweight FPS acceptance.'
        for metric_list in ('exclusive_contexts', 'inclusive_functions'):
            for row in result[metric_list]:
                row['approx_cpu_ms_per_interval'] = row['cpu_seconds'] * 1000 / count
        output['phases'][phase['name']] = result
    if not output['phases']:
        raise ValueError('No existing top-down report')
    return output


def self_test():
    source = 'Function Stack,CPU Time:Total,CPU Time:Self,Module\nTotal,100,0,root\n "A<int,int>",100,1,engine\n  Recursive,75,1,engine\n   Recursive,50,2,engine\n'
    result = summarize(tree_rows(source), 4)
    inclusive = {row['name']: row['cpu_seconds'] for row in result['inclusive_functions']}
    assert inclusive['A<int,int>'] == 4 and inclusive['Recursive'] == 3
    assert sum(row['cpu_seconds'] for row in result['exclusive_contexts']) == 4
    for invalid in (source.replace('  Recursive', '    Recursive', 1), source.replace(',1,engine', ',nan,engine', 1)):
        try:
            tree_rows(invalid)
        except ValueError:
            pass
        else:
            raise AssertionError('Malformed hierarchy/nonfinite CPU value accepted')
    try:
        summarize(tree_rows(source), 5)
    except ValueError:
        pass
    else:
        raise AssertionError('Mismatched independent self total accepted')
    print('CPU tree checks passed: VTune quoting, recursive inclusive totals, exclusive budget, malformed hierarchy/value, independent total.')


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('directory', nargs='?', type=Path)
    parser.add_argument('--output', type=Path)
    parser.add_argument('--self-test', action='store_true')
    args = parser.parse_args()
    if args.self_test:
        self_test()
    else:
        result = analyze(args.directory)
        text = json.dumps(result, ensure_ascii=False, indent=2)
        if args.output:
            args.output.write_text(text + '\n', encoding='utf-8')
        else:
            print(text)
