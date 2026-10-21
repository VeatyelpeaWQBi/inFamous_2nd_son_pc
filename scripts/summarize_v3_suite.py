# SPDX-License-Identifier: GPL-2.0-or-later
"""Local deterministic numerical verification and complete suite logging. No AI/network calls."""
import argparse
import copy
import csv
import json
import math
from collections import defaultdict
from pathlib import Path
from project_paths import project_child, relative_path, read_object, write_json, write_text
from analyze_v3_matrix import analyze


def runtime_issue_comparison(summary):
    """Compare exact diagnostic messages/counts, without approving or suppressing them."""
    rows = {r['role']: r['mechanisms'] for r in summary.get('rows', [])}
    roles = ('control-before', 'candidate', 'control-after')
    result = {'signatures_complete': False, 'all_errors_identical': False,
              'common_errors_accepted': False, 'counts_by_role': {},
              'common_error_signatures': {}, 'candidate_additional_error_signatures': {}}
    if set(rows) != set(roles): return result
    known = True
    signatures = {}
    for role in roles:
        item = rows[role]
        result['counts_by_role'][role] = item['runtime_log_counts']
        signatures[role] = {}
        for level in ('error', 'critical'):
            values = item.get('runtime_issue_signatures', {}).get(level, {})
            if (any(not isinstance(v, int) or v <= 0 for v in values.values()) or
                    sum(values.values()) != item['runtime_log_counts'][level]): known = False
            signatures[role][level] = values
    result['signatures_complete'] = known
    if known:
        before, candidate, after = (signatures[r] for r in roles)
        result['all_errors_identical'] = before == candidate == after
        common = {s: min(n, after['error'].get(s, 0)) for s, n in before['error'].items()}
        result['common_error_signatures'] = {s: n for s, n in common.items() if n}
        result['candidate_additional_error_signatures'] = {
            s: n - common.get(s, 0) for s, n in candidate['error'].items() if n > common.get(s, 0)}
    return result


def classify_group(summary, config):
    groups=summary.get('groups', [])
    if len(groups)!=1 or not groups[0].get('complete'): return 'incomplete'
    compare=groups[0]['comparison']
    if compare['control_fps_spread_percent']>config['maximum_control_spread_percent']: return 'unstable-controls'
    candidate=next((r for r in summary['rows'] if r['role']=='candidate'), None)
    controls=[r for r in summary['rows'] if r['role']!='candidate']
    if not candidate or len(controls)!=2: return 'incomplete'
    for row in summary['rows']:
        if not row['mechanisms']['log_available']: return 'missing-runtime-log'
    diagnostics = runtime_issue_comparison(summary)
    if any(row['mechanisms']['runtime_log_counts']['critical'] for row in summary['rows']): return 'runtime-errors'
    if any(row['mechanisms']['runtime_log_counts']['error'] for row in summary['rows']):
        # Common baseline errors stay blocking. This label is not a whitelist or a pass.
        return 'baseline-runtime-errors' if diagnostics['all_errors_identical'] else 'runtime-errors'
    for key in ('p95_ms','p99_ms'):
        best=min(r['frames'][key] for r in controls)
        if candidate['frames'][key]>best*(1+config['maximum_tail_regression_percent']/100): return 'tail-regression'
    mechanisms=candidate['mechanisms']['candidates']
    if any(v['activation_log_observed'] is False for v in mechanisms.values()): return 'requested-path-not-observed'
    if any(v.get('nonempty_session_observed') is False for v in mechanisms.values()): return 'requested-nonempty-path-unconfirmed'
    gain=compare['candidate_fps_change_percent_vs_controls'][0]
    if not math.isfinite(gain): return 'invalid-number'
    if gain>=config['minimum_fps_gain_percent']: return 'numerical-candidate'
    return 'no-confirmed-gain'


def load_attempt_details(attempt, task):
    """Reanalyze immutable attempts, or recover legacy attempts from plan.history.

    A shared -summary.json may have been overwritten by a later retry. It must
    never supply a failed attempt with the later attempt's successful sessions.
    """
    if task['phase'] in ('full', 'auxiliary'):
        return read_object(project_child(attempt['summary'])) if attempt.get('summary') else None
    plan_name = attempt.get('plan_snapshot') or attempt.get('plan')
    if plan_name:
        plan = read_object(project_child(plan_name))
        sources = [r for h in plan.get('history', []) for r in h.get('rows', [])] + plan['rows']
        number = attempt.get('number', 1)
        owned = set(attempt.get('sessions', []))
        chosen = [r for r in sources if r['case'] == task['case'] and
                  (r.get('attempt') == number if 'attempt' in r else r.get('session') in owned)]
        if not chosen: raise ValueError('Attempt has no matching frozen or historical matrix rows')
        attempt['row_states'] = copy.deepcopy(chosen)
        plan = copy.deepcopy(plan)
        complete = bool(plan.get('complete') and all(r in plan['rows'] for r in chosen))
        plan['rows'] = [dict(r, index=i) for i, r in enumerate(chosen)]
        plan['complete'] = complete  # An earlier failed history cannot borrow a later completion.
        attempt['details_source'] = plan_name
        return analyze(plan)
    if attempt.get('summary'):
        detail = read_object(project_child(attempt['summary']))
        if any(r['session'] not in set(attempt.get('sessions', [])) for r in detail.get('rows', [])):
            raise ValueError('Shared summary contains sessions from another attempt')
        attempt['details_source'] = attempt['summary']
        return detail
    return None


def summarize(suite):
    config=suite['configuration'];rows=[];confirmation=defaultdict(list)
    for task in suite['tasks']:
        entry={k:task.get(k) for k in ('index','phase','mode','case','repetition','status')}
        entry['attempts']=copy.deepcopy(task.get('attempts',[]))
        for attempt in entry['attempts']:
            try:
                detail=load_attempt_details(attempt,task)
                if detail is not None:attempt['details']=detail
            except (OSError,ValueError,KeyError,TypeError) as error:attempt['analysis_error']=str(error)
        selected=next((a for a in reversed(entry['attempts']) if a.get('status')=='collected'),None)
        entry['numerical_status']='not-applicable' if task['phase'] in ('full','auxiliary') else 'incomplete'
        if selected and selected.get('details'):
            try:
                detail=selected['details']
                entry['details']=detail
                if task['phase'] not in ('full','auxiliary'):
                    entry['runtime_issue_comparison']=runtime_issue_comparison(detail)
                    entry['numerical_status']=classify_group(detail,config)
                    if task['phase']=='confirmation':confirmation[(task['mode'],task['case'])].append(entry['numerical_status'])
            except (OSError,ValueError,KeyError,TypeError) as error:
                entry['analysis_error']=str(error)
        rows.append(entry)
    verdicts=[]
    for (mode,case),statuses in sorted(confirmation.items()):
        needed=config['confirmation_groups']
        verdicts.append({'mode':mode,'case':case,'groups':len(statuses),'required_groups':needed,
                         'numerical_verification':'repeatable-improvement' if len(statuses)>=needed and all(s=='numerical-candidate' for s in statuses) else 'not-established',
                         'statuses':statuses,'visual_acceptance':False,'whole_game_acceptance':False})
    planned=[]
    scope=suite.get('planned_scope',{})
    actual={(t['phase'],t['mode'],t['case']) for t in suite['tasks']}
    for mode in scope.get('modes',[]):
        for phase,cases in (('baseline',['baseline-pilot']),('correctness',scope.get('correctness_cases',[])),
                            ('screening',scope.get('screening_cases',[])),('full',scope.get('full_cases',[]))):
            for case in cases:
                if (phase,mode,case) not in actual:planned.append({'phase':phase,'mode':mode,'case':case,'status':'not-started'})
    if scope.get('auxiliary_requested') and ('auxiliary','pro4k','auxiliary') not in actual:
        planned.append({'phase':'auxiliary','mode':'pro4k','case':'auxiliary','status':'not-started'})
    return {'schema':1,'analysis_revision':2,'suite_status':suite['status'],'runtime_sha256':suite.get('runtime_sha256'),
            'not_started':planned,
            'tasks':rows,'numerical_verdicts':verdicts,'whole_game_acceptance':False,
            'limits':['Numerical verification uses complete guest-flip windows, not hardware scanout.',
                      'OCR/pause/render counters and a coarse image signature do not prove brightness, full particles or every rendering effect.',
                      'All incomplete, failed and retried attempts are retained. Full/auxiliary timings are excluded from FPS verification.',
                      'Identical baseline Error messages remain blocking; diagnostics are never silently suppressed.',
                      'First-hit logs prove process-level execution only, not activation during the measurement window.',
                      'GPU-only consumption, grouped generation snapshots and direct guest-memory writes remain research; this script does not generate or rewrite engine code.']}


CSV_FIELDS = ('phase','mode','case','repetition','attempt','attempt_status','row_status','selected_attempt',
              'group_complete','failure','analysis_error','role','session','fps','p95_ms','p99_ms',
              'over50','over100','over50_per_min','over100_per_1000_frames')


def result_rows(report):
    for task in report['tasks']:
        if task['phase'] in ('full','auxiliary'): continue
        selected=next((a for a in reversed(task['attempts']) if a.get('status')=='collected'),None)
        for attempt in task['attempts']:
            detail=attempt.get('details',{})
            measured={r['session']:r for r in detail.get('rows',[])}
            states=attempt.get('row_states') or list(measured.values())
            complete=bool(detail.get('groups')) and all(g['complete'] for g in detail['groups'])
            for state in states:
                if not state.get('session'): continue
                row={k:task[k] for k in ('phase','mode','case','repetition')}
                row.update(attempt=attempt.get('number'),attempt_status=attempt.get('status'),
                           row_status=state.get('status','not-recorded'),selected_attempt=attempt is selected,
                           group_complete=complete,failure=attempt.get('failure'),analysis_error=attempt.get('analysis_error'),
                           role=state['role'],session=state['session'])
                run=measured.get(state['session'])
                if run:
                    f=run['frames'];seconds=f['duration_seconds']
                    row.update(fps=f['fps'],p95_ms=f['p95_ms'],p99_ms=f['p99_ms'],
                               over50=f['frames_over_50ms'],over100=f['frames_over_100ms'],
                               over50_per_min=f['frames_over_50ms']*60/seconds,
                               over100_per_1000_frames=f['frames_over_100ms']*1000/f['frames'])
                yield row


def emit(path,directory=None):
    suite=read_object(path);report=summarize(suite);directory=directory or path.parent
    write_json(directory/'summary.json',report)
    lines=['V3 完整自动测试报告',f"状态: {suite['status']}",f"引擎: {suite.get('runtime_sha256','not-built')}",
           '准备/构建记录: '+json.dumps({k:v for k,v in suite.items() if k!='tasks'},ensure_ascii=False,indent=2),
           '说明：数值候选不等于画质或全游戏验收通过；所有失败和重试均保留。','']
    for task in report['tasks']:
        lines.append(f"[{task['index']}] {task['phase']} {task['mode']} {task['case']} repeat={task['repetition']} status={task['status']} numerical={task['numerical_status']}")
        lines.append(json.dumps(task,ensure_ascii=False,indent=2))
    lines.extend(['','未启动的计划项目',json.dumps(report['not_started'],ensure_ascii=False,indent=2),
                  '','重复验证结论',json.dumps(report['numerical_verdicts'],ensure_ascii=False,indent=2),'',*report['limits']])
    write_text(directory/'report.log','\n'.join(lines)+'\n')
    import io
    output=io.StringIO(newline='')
    writer=csv.DictWriter(output,fieldnames=CSV_FIELDS,lineterminator='\n')
    writer.writeheader();writer.writerows(result_rows(report))
    write_text(directory/'results.csv',output.getvalue())
    print(json.dumps({'tasks':len(report['tasks']),'verdicts':len(report['numerical_verdicts']),'report':relative_path(directory/'report.log')}))


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--suite',required=True)
    parser.add_argument('--output-directory',help='Optional separate directory under state/runtime; preserves original reports.')
    args=parser.parse_args()
    path=project_child(args.suite)
    if not path.is_relative_to(project_child('state/runtime')):parser.error('Suite state must stay under state/runtime')
    directory=project_child(args.output_directory) if args.output_directory else path.parent
    if not directory.is_relative_to(project_child('state/runtime')):parser.error('Reports must stay under state/runtime')
    emit(path,directory)
