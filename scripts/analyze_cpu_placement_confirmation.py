"""Read-only AB/BA confirmation audit. Two pairs do not provide a repeatability interval."""
import argparse
import csv
import json
from pathlib import Path

from analyze_super_batch import load, stats
from diagnostic_quality import frame_quality


def audit(path):
    report = load(path)
    if report.get('candidate') != 'cpu-placement' or not report.get('data_checks_passed'):
        raise ValueError('Expected a qualified CPU placement pair, before human performance acceptance.')
    rows = []
    for s in report['sessions']:
        if len(s['trials']) != 1:
            raise ValueError('Expected exactly one release per session.')
        t = s['trials'][0]
        folder = Path(s['session'])
        at = t['release_ns']
        frames = list(csv.DictReader((folder / 'frames.csv').open(encoding='utf-8-sig')))
        whole = stats([int(f['interval_ns'])/1e6 for f in frames if at <= int(f['utc_ns']) < at+12e9])
        clocks = frame_quality(folder, (at-10e9)/1e9, (at+12e9)/1e9)
        launch = load(folder / 'launch.json')
        rows.append(dict(session=s['session'], variant=t['variant'],
                         runtime_sha256=s['runtime_sha256'], game_sha256=s['game_sha256'],
                         mode=s['mode'], saves=s['saves'], complete=s['complete'],
                         exit_code=s['exit_code'], progress_published=launch.get('progress_published'),
                         compile_delta=t['compile_delta'],
                         metrics=dict(whole_0_12_fps=whole['fps'],
                                      first_240_fps=t['frame_windows']['first_240']['fps'],
                                      first_480_fps=t['frame_windows']['first_480']['fps'],
                                      worst_fixed_second_fps=min(v['fps'] for v in t['seconds'] if v.get('fps')),
                                      first_480_p99_ms=t['frame_windows']['first_480']['p99_ms']),
                         baseline=t['windows']['baseline'], whole_0_12=whole,
                         clock_quality=clocks, requested_tuning=t['requested_tuning']))
    if [r['variant'] for r in rows] != ['A-cpu-placement', 'B-cpu-placement']:
        raise ValueError('Analysis must retain A baseline/B candidate identity regardless of capture order.')
    a, b = rows
    return dict(source=str(path), capture_order=report.get('capture_order', 'AB'),
                A=a, B=b, candidate_change_percent={k:100*(b['metrics'][k]/a['metrics'][k]-1) for k in a['metrics']},
                baseline_drift_percent=report['baseline_drift_percent'])


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('ab', type=Path)
    p.add_argument('ba', type=Path)
    p.add_argument('--output', type=Path, required=True)
    args = p.parse_args()
    pairs = [audit(args.ab), audit(args.ba)]
    if [r['capture_order'] for r in pairs] != ['AB', 'BA']:
        raise ValueError('Expected one AB pair followed by one BA pair.')
    sessions = [r[k] for r in pairs for k in ('A', 'B')]
    for key in ('runtime_sha256', 'game_sha256', 'mode'):
        if len({s[key] for s in sessions}) != 1:
            raise ValueError('Cross-pair identity mismatch: '+key)
    saves = [sorted((v['path'], v['bytes'], v['sha256']) for v in s['saves']) for s in sessions]
    if not saves[0] or any(v != saves[0] for v in saves):
        raise ValueError('Cross-pair original saves differ.')
    consistent = {k:all(pair['candidate_change_percent'][k] > 0 for pair in pairs)
                  for k in ('whole_0_12_fps', 'first_240_fps', 'first_480_fps', 'worst_fixed_second_fps')}
    result = dict(schema=1, pairs=pairs, consistently_positive_metrics=consistent,
                  performance_acceptance=False,
                  assessment='Not reproduced: do not enable by default.' if not all(consistent.values())
                             else 'Consistent observations; human workload/image/interference review still required.',
                  limits=['No confidence interval from two pairs; individual frames are not independent repeats.',
                          'Raw chronological changes cannot isolate affinity, foreground activity, thermal/power or cache state.',
                          'No data is deleted, trimmed, warmed up again or normalized by a disturbed baseline.'])
    args.output.write_text(json.dumps(result, ensure_ascii=False, indent=2)+'\n', encoding='utf-8')
    for pair in pairs:
        print(pair['capture_order'], pair['candidate_change_percent'])
    print(result['assessment'])


if __name__ == '__main__':
    main()
