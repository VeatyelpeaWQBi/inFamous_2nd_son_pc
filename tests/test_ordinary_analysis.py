# SPDX-License-Identifier: GPL-2.0-or-later
import sys
from pathlib import Path
import unittest
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'scripts'))
from analyze_ordinary import qpc_window,gpu_summary,cpu_summary,observation_window


def gpu(first,last,index='0',clock='2000'):
    return {'query_start_qpc':str(first),'query_observed_end_qpc':str(last),' index':index,' name':' GPU',
            ' utilization.gpu [%]':'95',' temperature.gpu':'70',' power.draw [W]':'130',
            ' clocks.current.sm [MHz]':clock,' clocks.current.memory [MHz]':'9001',' pstate':'P0',
            ' clocks_event_reasons.active':'0x4'}


class OrdinaryAnalysisTests(unittest.TestCase):
    def test_scene_relative_window_and_legacy_defaults(self):
        self.assertEqual(observation_window({}),(90,60))
        metadata={'observer_revision':3,'scene_confirmed':True,'scene_key':'fixed',
                  'warmup_seconds':30,'measure_seconds':20,'window_start_seconds':45,'window_end_seconds':65,
                  'scene_receipt':{'evidence':'agent-reviewed-gameplay-image','gameplay_confirmed':True,
                                   'view_confirmed':True,'scene_key':'fixed','elapsed_seconds':15}}
        self.assertEqual(observation_window(metadata),(45,20))
        with self.assertRaises(ValueError):observation_window(metadata,46,19)
        for key,value in (('scene_confirmed',False),('window_start_seconds',44),('window_end_seconds',66)):
            invalid=dict(metadata);invalid[key]=value
            with self.assertRaises(ValueError):observation_window(invalid)

    def test_qpc_uncertainty_is_conservative(self):
        self.assertEqual(qpc_window({'qpc_before':'100','qpc_after':'102','elapsed_ns':'0'},10,2,3),(122,150))
        self.assertEqual(qpc_window({'qpc_before':'120','qpc_after':'122','elapsed_ns':'2000000000'},10,2,3),(122,150))

    def test_invalid_clocks_and_windows_rejected(self):
        for frequency,start,duration in ((0,0,1),(1,-1,2),(1,0,0),(1,0,float('nan'))):
            with self.assertRaises(ValueError):qpc_window({'qpc_before':'0','qpc_after':'0','elapsed_ns':'0'},frequency,start,duration)
        with self.assertRaises(ValueError):gpu_summary([gpu(20,10)],0,30,10)

    def test_crossing_queries_excluded_and_devices_separate(self):
        result=gpu_summary([gpu(9,12),gpu(10,20),gpu(20,31),gpu(10,20,'1','1500')],10,30,10)
        self.assertEqual([row['samples'] for row in result],[1,1])
        self.assertEqual([row['metrics']['sm_clock_mhz']['mean'] for row in result],[2000,1500])

    def test_unavailable_sensor_not_zero(self):
        result=gpu_summary([gpu(10,20,clock='[N/A]')],10,20,10)[0]
        self.assertEqual(result['metrics']['sm_clock_mhz'],{'count':0})
        self.assertEqual(result['missing']['sm_clock_mhz'],1)

    def test_cpu_rates_weighted_by_actual_time(self):
        def row(first,last,value):return {'qpc_begin':str(first),'qpc_end':str(last),
            'game_cpu_seconds':str(value),'observer_cpu_seconds':'0','other_accessible_cpu_seconds':'0','processes_unreadable':'10'}
        result=cpu_summary([row(0,10,1),row(10,30,4),row(30,40,99)],0,30,10)
        self.assertEqual(result['samples'],2)
        self.assertAlmostEqual(result['metrics']['game_cpu_seconds']['weighted_cpu_cores'],5/3)
        self.assertEqual(result['metrics']['game_cpu_seconds']['sample_cpu_cores']['max'],2)

    def test_negative_cpu_delta_rejected(self):
        with self.assertRaises(ValueError):cpu_summary([{'qpc_begin':'0','qpc_end':'10','game_cpu_seconds':'-1'}],0,10,10)


if __name__=='__main__':unittest.main()
