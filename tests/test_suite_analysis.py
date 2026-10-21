# SPDX-License-Identifier: GPL-2.0-or-later
# Source only in this implementation turn; executed by the user-started suite.
import sys
import copy
from pathlib import Path
import unittest
from unittest.mock import patch
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'scripts'))
from summarize_v3_suite import (classify_group, runtime_issue_comparison,
                                load_attempt_details, result_rows)


def fixture():
    config={'maximum_control_spread_percent':3,'minimum_fps_gain_percent':3,'maximum_tail_regression_percent':5}
    def row(role, fps):
        return {'role':role,'frames':{'fps':fps,'p95_ms':30,'p99_ms':35},
                'mechanisms':{'log_available':True,'runtime_log_counts':{'error':0,'critical':0,'warning':0},'candidates':{}}}
    summary={'groups':[{'complete':True,'comparison':{'control_fps_spread_percent':1,'candidate_fps_change_percent_vs_controls':[5,6]}}],
             'rows':[row('control-before',30),row('candidate',32),row('control-after',30.3)]}
    return summary,config


class SuiteAnalysisTests(unittest.TestCase):
    def test_identical_baseline_errors_remain_blocking_and_new_errors_stay_visible(self):
        summary,config=fixture()
        for row in summary['rows']:
            row['mechanisms']['runtime_log_counts']['error']=1
            row['mechanisms']['runtime_issue_signatures']={'error':{'Loader: existing':1},'critical':{}}
        self.assertEqual(classify_group(summary,config),'baseline-runtime-errors')
        diagnostics=runtime_issue_comparison(summary)
        self.assertTrue(diagnostics['all_errors_identical'])
        self.assertFalse(diagnostics['common_errors_accepted'])
        summary['rows'][1]['mechanisms']['runtime_issue_signatures']['error']={'Loader: different':1}
        self.assertEqual(classify_group(summary,config),'runtime-errors')
        self.assertEqual(runtime_issue_comparison(summary)['candidate_additional_error_signatures'],{'Loader: different':1})
        for row in summary['rows']:
            row['mechanisms']['runtime_log_counts']['critical']=1
            row['mechanisms']['runtime_issue_signatures']['critical']={'Device: lost':1}
        self.assertEqual(classify_group(summary,config),'runtime-errors')

    def test_empty_recording_activation_does_not_prove_nonempty_execution(self):
        summary,config=fixture()
        summary['rows'][1]['mechanisms']['candidates']={'recording-2':{
            'activation_log_observed':True,'nonempty_session_observed':False}}
        self.assertEqual(classify_group(summary,config),'requested-nonempty-path-unconfirmed')

    def test_shared_retry_plan_recovers_only_the_requested_attempt(self):
        def row(attempt,role,session,status):
            return {'index':0,'case':'copy-coalesce','attempt':attempt,'role':role,'session':session,'status':status}
        old=[row(1,'control-before','old-control','validated'),row(1,'candidate','old-candidate','running'),
             row(1,'control-after',None,'pending')]
        new=[row(2,role,'new-'+role,'validated') for role in ('control-before','candidate','control-after')]
        plan={'history':[{'rows':old}],'rows':new,'complete':True}
        original=copy.deepcopy(plan)
        task={'phase':'correctness','case':'copy-coalesce'}
        with patch('summarize_v3_suite.read_object',return_value=plan), \
             patch('summarize_v3_suite.analyze',side_effect=lambda value:value):
            first=load_attempt_details({'number':1,'plan':'state/runtime/unit-plan.json'},task)
            second=load_attempt_details({'number':2,'plan':'state/runtime/unit-plan.json'},task)
        self.assertEqual([r['session'] for r in first['rows']],['old-control','old-candidate',None])
        self.assertFalse(first['complete'])
        self.assertTrue(second['complete'])
        self.assertEqual([r['index'] for r in second['rows']],[0,1,2])
        self.assertEqual(plan,original)

    def test_csv_retains_failed_partial_rows_without_borrowing_later_metrics(self):
        def measured(role,session):
            return {'role':role,'session':session,'frames':{'fps':30,'p95_ms':34,'p99_ms':50,
                    'duration_seconds':20,'frames':600,'frames_over_50ms':2,'frames_over_100ms':0}}
        old=measured('control-before','old-control')
        new=[measured(role,'new-'+role) for role in ('control-before','candidate','control-after')]
        attempts=[{'number':1,'status':'failed','failure':'status read failed',
                   'details':{'rows':[old],'groups':[{'complete':False}]},
                   'row_states':[dict(old,status='validated'),{'role':'candidate','session':'old-candidate','status':'running'}]},
                  {'number':2,'status':'collected','details':{'rows':new,'groups':[{'complete':True}]},
                   'row_states':[dict(r,status='validated') for r in new]}]
        rows=list(result_rows({'tasks':[{'phase':'correctness','mode':'pro4k','case':'copy-coalesce',
                                        'repetition':1,'attempts':attempts}]}))
        self.assertEqual(len(rows),5)
        self.assertEqual(rows[0]['fps'],30)
        self.assertNotIn('fps',rows[1])
        self.assertFalse(rows[0]['selected_attempt'])
        self.assertFalse(rows[0]['group_complete'])
        self.assertTrue(rows[-1]['selected_attempt'])
        self.assertTrue(rows[-1]['group_complete'])

    def test_numerical_candidate_is_only_a_numerical_candidate(self):
        summary,config=fixture()
        self.assertEqual(classify_group(summary,config),'numerical-candidate')

    def test_instability_and_tail_regression_block_gain(self):
        summary,config=fixture();summary['groups'][0]['comparison']['control_fps_spread_percent']=4
        self.assertEqual(classify_group(summary,config),'unstable-controls')
        summary,config=fixture();summary['rows'][1]['frames']['p99_ms']=50
        self.assertEqual(classify_group(summary,config),'tail-regression')

    def test_errors_missing_logs_and_unobserved_requested_path_block_gain(self):
        summary,config=fixture();summary['rows'][1]['mechanisms']['runtime_log_counts']['error']=1
        self.assertEqual(classify_group(summary,config),'runtime-errors')
        summary,config=fixture();summary['rows'][1]['mechanisms']['log_available']=False
        self.assertEqual(classify_group(summary,config),'missing-runtime-log')
        summary,config=fixture();summary['rows'][1]['mechanisms']['candidates']={'copy':{'activation_log_observed':False}}
        self.assertEqual(classify_group(summary,config),'requested-path-not-observed')

    def test_incomplete_and_small_gain_are_not_accepted(self):
        summary,config=fixture();summary['groups'][0]['complete']=False
        self.assertEqual(classify_group(summary,config),'incomplete')
        summary,config=fixture();summary['groups'][0]['comparison']['candidate_fps_change_percent_vs_controls']=[1,2]
        self.assertEqual(classify_group(summary,config),'no-confirmed-gain')


if __name__=='__main__':unittest.main()
