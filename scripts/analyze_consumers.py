# SPDX-License-Identifier: GPL-2.0-or-later
"""Offline bounded fault-context evidence; never certifies dependency removal or FPS gains."""
import argparse
from collections import Counter,defaultdict
import csv
import json
from pathlib import Path
from analyze_timing import Event,FIELDS,one,read_path,producer_path,stats

HIGH_BIT=1<<63
MASK=(1<<64)-1
SITES={0x3f3eff:'feedback_count',0x3f3fcc:'auxiliary_count',0x3ff65e:'record_x',
       0x3ff664:'record_z',0x3ff66b:'record_y',0x169624:'completion_compare',
       0x3ff6d9:'record_copy_z',0x3ff6e1:'record_copy_xy'}
WIDTHS={site:(8 if site==0x3ff6e1 else 4) for site in SITES}
ALLOWED=set('''read_request read_cp_begin read_cp_end read_completion_request read_dependencies
read_copy read_wait_begin read_wait_end read_fallback_begin read_fallback_end read_return
read_producer read_producer_range read_producer_time read_producer_domain read_producer_coverage
read_producer_window read_consumer read_consumer_regs0 read_consumer_regs1 read_consumer_regs2'''.split())


def decode_snapshot(ident,events,base,limit):
    if not ident&HIGH_BIT:raise ValueError('Consumer request has no reserved ID tag')
    request=one(events,'read_request');header=one(events,'read_consumer')
    parts=[one(events,f'read_consumer_regs{i}') for i in range(3)]
    if header.a not in SITES or header.b!=WIDTHS[header.a] or not 1<=header.c<=limit or header.d!=base:
        raise ValueError('Consumer site, width, ordinal or base mismatch')
    if request.c!=base+header.a or request.b<4 or request.d!=0:
        raise ValueError('Consumer request address-space or access mode mismatch')
    if any(part.thread!=request.thread or not request.ns<=part.ns<=one(events,'read_return').ns for part in [header]+parts):
        raise ValueError('Fault snapshot outside its requesting thread/lifetime')
    values=[value for part in parts for value in (part.a,part.b,part.c,part.d)]
    r=dict(zip(('rax','rcx','rdx','rbx','rsi','rdi','r15','r14','rsp','rbp','r13','r12'),values))
    expected={0x3f3eff:r['rax']+4,0x3f3fcc:r['rax']+0x28,
              0x3ff65e:r['rax']+r['r15'],0x3ff664:r['rax']+r['r15']+8,
              0x3ff66b:r['rax']+r['r15']+4,0x169624:r['rcx']+8*r['rax'],
              0x3ff6d9:r['rbx']+r['rcx']+8,0x3ff6e1:r['rbx']+r['rcx']}[header.a]&MASK
    if expected!=request.a:raise ValueError('Fault registers do not reproduce the actual operand address')
    return {'request_id':str(ident),'site':hex(header.a),'role':SITES[header.a],'ordinal':header.c,
            'request_address':request.a,'fault_handler_span':request.b,'instruction_width':header.b,
            'rip':request.c,'snapshot_ns':header.ns,'registers':r,
            'emitter_pointer':r['r15'] if header.a in (0x3f3eff,0x3f3fcc) else None,
            'feedback_pointer':r['rax'] if header.a in (0x3f3eff,0x3f3fcc) else None,
            'record_source_base':r['rbx'] if header.a in (0x3ff6d9,0x3ff6e1) else None,
            'record_source_offset':r['rcx'] if header.a in (0x3ff6d9,0x3ff6e1) else None,
            'record_destination_base_register':r['r13'] if header.a in (0x3ff6d9,0x3ff6e1) else None}


def validate_metadata(meta):
    evidence=meta.get('consumer_evidence',{})
    if (meta.get('schema')!=1 or meta.get('clock')!='steady_clock_ns' or not meta.get('complete') or not meta.get('consumer_only') or
            any(meta.get(key) for key in ('dropped','cutoff_publishers','io_failed')) or
            not evidence.get('enabled') or meta.get('gpu_profile_at_start')!=0 or meta.get('compute_hash_at_start')!=0):
        raise ValueError('Consumer-only transport/pinning/completeness checks failed')
    limit=evidence.get('limit_per_site');counts=evidence.get('site_counts')
    site_rvas=evidence.get('site_rvas',list(SITES)[:6]) # Historical six-site preparation is still readable.
    if (not isinstance(site_rvas,list) or not site_rvas or len(set(site_rvas))!=len(site_rvas) or any(site not in SITES for site in site_rvas) or
            type(limit) is not int or not 1<=limit<=1024 or not isinstance(counts,list) or len(counts)!=len(site_rvas) or
            any(type(count) is not int or not 0<=count<=limit for count in counts)):
        raise ValueError('Invalid consumer evidence budget metadata')
    return evidence


def analyze(session):
    load=lambda path:json.loads(path.read_text(encoding='utf-8-sig'))
    launch=load(session/'launch.json');meta=load(session/'path-trace.json')
    evidence=validate_metadata(meta)
    if launch.get('timing_profile')!='consumer' or not launch.get('audio_muted'):
        raise ValueError('Not the isolated muted consumer evidence profile')
    groups=defaultdict(list);sequences={};counts=Counter();rows=0;contexts={}
    with (session/'path-trace.csv').open(encoding='utf-8-sig',newline='') as stream:
        reader=csv.DictReader(stream)
        if reader.fieldnames!=FIELDS:raise ValueError('Trace header mismatch')
        for row in reader:
            if None in row or any(row.get(key) is None for key in FIELDS):raise ValueError('Truncated trace row')
            values={key:int(row[key]) for key in FIELDS if key!='event'}
            if (row['event'] not in ALLOWED or any(not 0<=value<=MASK for value in values.values()) or
                    not values['id']&HIGH_BIT or values['thread']>=meta['registered_threads']):
                raise ValueError('Invalid or unrelated consumer-only trace event')
            thread=values['thread'];sequence=values['seq'];ns=values['ns']
            previous_sequence,previous_ns=sequences.get(thread,(-1,0))
            if sequence!=previous_sequence+1 or ns<previous_ns:raise ValueError('Thread sequence/clock gap')
            sequences[thread]=(sequence,ns);rows+=1;counts[row['event']]+=1
            groups[values['id']].append(Event(ns,thread,row['event'],*(values[key] for key in ('a','b','c','d'))))
            if row['event']=='read_request':contexts[values['id']]=values['guest_frame']
    if rows!=meta['events']:raise ValueError('Trace event total mismatch')
    decoded=[];ordinals=defaultdict(set);pairs=defaultdict(set);commands=defaultdict(lambda:defaultdict(set))
    for ident,events in sorted(groups.items()):
        item=decode_snapshot(ident,events,evidence['base'],evidence['limit_per_site'])
        item['submit_done_context_not_generation']=contexts[ident]
        item['source_prefix_live_checked']=bool(evidence.get('source_prefix_checked'))
        if item['ordinal'] in ordinals[item['site']]:raise ValueError('Duplicate per-site sample ordinal')
        ordinals[item['site']].add(item['ordinal'])
        intervals=read_path(events)
        item['read_intervals_ns_not_additive']=dict(intervals)
        if any(event.kind=='read_producer_coverage' for event in events):
            item['declared_producer_evidence']=producer_path(events)
            for producer in item['declared_producer_evidence']['candidates']:
                if producer['scopes']&1:
                    key=(producer['domain'],producer['command'],producer['shader'])
                    if producer['command']:commands[key][item['role']].add(ident)
        else:
            item['declared_producer_evidence']=None # e.g. nothing/pending-copy path, not proof of no writer.
        if item['emitter_pointer'] is not None:pairs[item['emitter_pointer']].add(item['feedback_pointer'])
        decoded.append(item)
    if not decoded:raise ValueError('No matching fault contexts were captured')
    site_rvas=evidence.get('site_rvas',list(SITES)[:6])
    if any(int(site,16) not in site_rvas for site in ordinals):raise ValueError('Captured site was not enabled in metadata')
    actual_counts=[len(ordinals[hex(site)]) for site in site_rvas]
    if any(actual>reserved for actual,reserved in zip(actual_counts,evidence['site_counts'])):
        raise ValueError('More emitted samples than admitted fault contexts')
    shared=[]
    for (domain,command,shader),roles in commands.items():
        if 'feedback_count' in roles and any(role.startswith('record_') for role in roles):
            shared.append({'domain':domain,'command':command,'shader':hex(shader),
                           'request_ids_by_role':{role:[str(ident) for ident in sorted(ids)] for role,ids in roles.items()},
                           'interpretation':'Shared latest declared request-range candidate only; not proof of actual bytes or same generation.'})
    return {'schema':1,'format_usable':True,'session':launch['session'],'runtime_sha256':launch['runtime_sha256'],
            'game_sha256':launch['game_sha256'],'trace_metadata':meta,'event_counts':dict(counts),
            'samples_by_site':{hex(site):actual_counts[index] for index,site in enumerate(site_rvas)},
            'reserved_but_not_emitted':[reserved-actual for reserved,actual in zip(evidence['site_counts'],actual_counts)],
            'emitter_feedback_pairs':[{'emitter_pointer':hex(emitter),'feedback_buffers':[hex(value) for value in sorted(buffers)],
                                      'logical_identity_or_generation_proven':False} for emitter,buffers in sorted(pairs.items())],
            'shared_declared_request_candidates':shared,'requests':decoded,
            'read_total_ms':stats([item['read_intervals_ns_not_additive']['total_ns'] for item in decoded]),
            'safe_to_remove_dependencies':False,'generation_proven':False,'performance_acceptance':False,
            'limits':['Only faults at the listed pinned instructions are sampled, with an independent lifetime cap per site. Already-readable accesses are invisible.',
                      'A repeated object address can refer to reused or changing state; address equality never establishes logical identity or generation.',
                      'Registers are captured before the original instruction resumes; no object dereferences or additional GPU buffer data reads.',
                      'Emitter/feedback inference uses the pinned dominating load; record register roles beyond exact operand arithmetic remain unproven.',
                      'guest_frame is SubmitDone context, not a causal frame or generation identifier.',
                      'Latest declared overlap and shared command IDs do not prove actual shader stores, complete consumption or object lifetime.',
                      'Filtered trace intentionally excludes global submission/copy/frame events; do not use it for a complete critical path or FPS gain.']}


def main():
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--session',required=True,type=Path)
    args=parser.parse_args()
    try:result=analyze(args.session)
    except (ValueError,OSError,KeyError,TypeError,csv.Error) as exc:
        result={'schema':1,'format_usable':False,'errors':[str(exc)],'performance_acceptance':False}
    (args.session/'consumer-summary.json').write_text(json.dumps(result,ensure_ascii=False,indent=2)+'\n',encoding='utf-8')
    print('Consumer evidence:', 'usable' if result['format_usable'] else 'invalid',result.get('samples_by_site',result.get('errors')))
    return 0 if result['format_usable'] else 1


if __name__=='__main__':raise SystemExit(main())
