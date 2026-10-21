# SPDX-License-Identifier: GPL-2.0-or-later
import sys
from pathlib import Path
import unittest
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'scripts'))
from analyze_timing import Event
from analyze_consumers import decode_snapshot,validate_metadata,HIGH_BIT


def fixture(site=0x3f3eff):
    base=0x7e30000;address=0x1004 if site==0x3f3eff else 0x1006
    events=[Event(1,0,'read_request',address,8,base+site),Event(2,0,'read_consumer',site,4,1,base),
            Event(3,0,'read_consumer_regs0',0x1000,0,0,0),Event(4,0,'read_consumer_regs1',0,0,6,0),
            Event(5,0,'read_consumer_regs2',0,0,0,0),Event(10,0,'read_return',1)]
    return base,events


class ConsumerAnalysisTests(unittest.TestCase):
    def test_count_object_and_buffer_pair(self):
        base,events=fixture();row=decode_snapshot(HIGH_BIT|1,events,base,64)
        self.assertEqual(row['emitter_pointer'],6);self.assertEqual(row['feedback_pointer'],0x1000)
        self.assertEqual(row['instruction_width'],4);self.assertEqual(row['fault_handler_span'],8)
        self.assertEqual(row['request_id'],str(HIGH_BIT|1)) # Exact across JSON readers with floating-point numbers.

    def test_record_operand_does_not_invent_emitter(self):
        base,events=fixture(0x3ff65e);row=decode_snapshot(HIGH_BIT|2,events,base,64)
        self.assertIsNone(row['emitter_pointer']);self.assertEqual(row['request_address'],0x1006)

    def test_record_copy_uses_rbx_rcx_and_preserves_access_width(self):
        base,events=fixture()
        for site,width,offset in ((0x3ff6d9,4,8),(0x3ff6e1,8,0)):
            changed=events.copy();changed[0]=Event(1,0,'read_request',0x2030+offset,8,base+site)
            changed[1]=Event(2,0,'read_consumer',site,width,1,base)
            changed[2]=Event(3,0,'read_consumer_regs0',0,0x30,0,0x2000)
            row=decode_snapshot(HIGH_BIT|1,changed,base,64)
            self.assertEqual(row['record_source_base'],0x2000);self.assertEqual(row['record_source_offset'],0x30)
            self.assertEqual(row['instruction_width'],width);self.assertIsNone(row['emitter_pointer'])

    def test_missing_or_duplicate_register_chunk_rejected(self):
        base,events=fixture()
        for changed in (events[:-2]+events[-1:],events+[events[2]]):
            with self.assertRaises(ValueError):decode_snapshot(HIGH_BIT|1,changed,base,64)

    def test_unknown_site_base_width_and_budget_rejected(self):
        base,events=fixture()
        for header in (Event(2,0,'read_consumer',123,4,1,base),Event(2,0,'read_consumer',0x3f3eff,8,1,base),
                       Event(2,0,'read_consumer',0x3f3eff,4,65,base),Event(2,0,'read_consumer',0x3f3eff,4,1,base+1)):
            changed=events.copy();changed[1]=header
            with self.assertRaises(ValueError):decode_snapshot(HIGH_BIT|1,changed,base,64)

    def test_mismatched_fault_address_rejected(self):
        base,events=fixture();events[0]=Event(1,0,'read_request',0x1008,8,base+0x3f3eff)
        with self.assertRaises(ValueError):decode_snapshot(HIGH_BIT|1,events,base,64)

    def test_thread_and_tag_validation(self):
        base,events=fixture()
        with self.assertRaises(ValueError):decode_snapshot(1,events,base,64)
        events[2]=Event(3,1,'read_consumer_regs0',0x1000,0,0,0)
        with self.assertRaises(ValueError):decode_snapshot(HIGH_BIT|1,events,base,64)

    def test_transport_and_clock_metadata_rejected(self):
        meta={'schema':1,'clock':'steady_clock_ns','complete':True,'consumer_only':True,
              'gpu_profile_at_start':0,'compute_hash_at_start':0,
              'consumer_evidence':{'enabled':True,'limit_per_site':64,'site_counts':[1,0,0,0,0,0]}}
        self.assertTrue(validate_metadata(meta))
        for key,value in (('clock','wall_clock_ns'),('complete',False),('consumer_only',False),('dropped',1),('gpu_profile_at_start',2)):
            with self.assertRaises(ValueError):validate_metadata(dict(meta,**{key:value}))

    def test_malformed_budget_metadata_rejected(self):
        meta={'schema':1,'clock':'steady_clock_ns','complete':True,'consumer_only':True,
              'gpu_profile_at_start':0,'compute_hash_at_start':0}
        for limit,counts in ((0,[0]*6),(1025,[0]*6),(64,[0]),(64,[-1]*6),(64,[65]*6)):
            with self.assertRaises(ValueError):validate_metadata(dict(meta,consumer_evidence={'enabled':True,'limit_per_site':limit,'site_counts':counts}))


if __name__=='__main__':unittest.main()
