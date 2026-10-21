# SPDX-License-Identifier: GPL-2.0-or-later
"""Read-only analysis of low-rate ordinary observations; never launches any tool."""
import argparse
from collections import Counter, defaultdict
import csv
import json
import hashlib
import math
from pathlib import Path
import statistics


def numeric(values):
    values=sorted(values)
    if not values:return {'count':0}
    return {'count':len(values),'min':values[0],'median':statistics.median(values),
            'mean':statistics.mean(values),'p95':values[math.ceil(.95*len(values))-1],'max':values[-1]}


def number(value):
    try:
        result=float(value)
        return result if math.isfinite(result) else None
    except (ValueError,TypeError):return None


def read_json(path):return json.loads(path.read_text(encoding='utf-8-sig'))


def qpc_window(anchor,frequency,start,duration):
    """Use conservative sides of the first flip's QPC bracket, never wall UTC."""
    if frequency<=0 or start<0 or duration<=0 or not math.isfinite(start+duration):
        raise ValueError('Invalid QPC frequency or observation window')
    before,after=int(anchor['qpc_before']),int(anchor['qpc_after'])
    if after<before:raise ValueError('Reversed first-flip QPC bracket')
    elapsed=int(anchor['elapsed_ns'])
    begin=after+math.ceil((start-elapsed/1e9)*frequency)
    end=before+math.floor((start+duration-elapsed/1e9)*frequency)
    if end<=begin:raise ValueError('QPC uncertainty consumes the observation window')
    return begin,end


def gpu_summary(rows,begin,end,frequency):
    groups=defaultdict(list)
    for original in rows:
        row={key.strip():value.strip() for key,value in original.items()}
        first,last=int(row['query_start_qpc']),int(row['query_observed_end_qpc'])
        if last<first:raise ValueError('Reversed GPU query bracket')
        if first<begin or last>end:continue
        groups[(row['index'],row['name'])].append(row)
    result=[]
    columns={'utilization_gpu_percent':'utilization.gpu [%]','temperature_c':'temperature.gpu',
             'power_w':'power.draw [W]','sm_clock_mhz':'clocks.current.sm [MHz]',
             'memory_clock_mhz':'clocks.current.memory [MHz]'}
    for (index,name),selected in sorted(groups.items()):
        entry={'index':index,'name':name,'samples':len(selected),'metrics':{},'missing':{}}
        for label,column in columns.items():
            values=[number(row[column]) for row in selected]
            entry['metrics'][label]=numeric([value for value in values if value is not None])
            entry['missing'][label]=sum(value is None for value in values)
        entry['pstates']=dict(Counter(row['pstate'] for row in selected))
        entry['clock_event_masks']=dict(Counter(row['clocks_event_reasons.active'] for row in selected))
        entry['query_observation_ms']=numeric([(int(row['query_observed_end_qpc'])-int(row['query_start_qpc']))*1000/frequency for row in selected])
        entry['first_query_start_qpc']=int(selected[0]['query_start_qpc'])
        entry['last_query_observed_end_qpc']=int(selected[-1]['query_observed_end_qpc'])
        result.append(entry)
    return result


def cpu_summary(rows,begin,end,frequency):
    selected=[]
    for row in rows:
        first,last=int(row['qpc_begin']),int(row['qpc_end'])
        if last<=first:raise ValueError('Invalid CPU observation interval')
        if first>=begin and last<=end:selected.append(row)
    fields=('game_cpu_seconds','observer_cpu_seconds','other_accessible_cpu_seconds')
    result={'samples':len(selected),'metrics':{},'scope':'Accessible surviving process deltas only; not total system CPU. Snapshot times have enumeration jitter.'}
    elapsed=sum((int(row['qpc_end'])-int(row['qpc_begin']))/frequency for row in selected)
    result['covered_interval_s']=elapsed
    for field in fields:
        rates=[];total=0.0
        for row in selected:
            value=number(row[field])
            if value is None or value<0:raise ValueError('Invalid CPU delta')
            seconds=(int(row['qpc_end'])-int(row['qpc_begin']))/frequency
            rates.append(value/seconds);total+=value
        result['metrics'][field]={'total_cpu_s':total,'weighted_cpu_cores':total/elapsed if elapsed else None,
                                  'sample_cpu_cores':numeric(rates)}
    result['processes_unreadable']=numeric([int(row['processes_unreadable']) for row in selected])
    return result


def observation_window(metadata, start=None, duration=None):
    if metadata.get('observer_revision', 0) < 3:
        return (90 if start is None else start, 60 if duration is None else duration)
    receipt = metadata.get('scene_receipt') or {}
    first, last = metadata.get('window_start_seconds'), metadata.get('window_end_seconds')
    if (not metadata.get('scene_confirmed') or receipt.get('evidence') not in ('agent-reviewed-gameplay-image','automatic-ocr-pause-render-counters') or
            not receipt.get('gameplay_confirmed') or not receipt.get('view_confirmed') or
            receipt.get('scene_key') != metadata.get('scene_key') or
            not isinstance(first, (int, float)) or not isinstance(last, (int, float)) or
            not math.isfinite(first + last) or last <= first or
            first < receipt.get('elapsed_seconds', float('inf')) + metadata['warmup_seconds'] or
            last - first != metadata['measure_seconds']):
        raise ValueError('No valid scene-confirmed observation window')
    if (start is not None and start != first) or (duration is not None and duration != last - first):
        raise ValueError('Requested window differs from the frozen scene-relative window')
    return first, last - first


def observation_summary(session,start=None,duration=None):
    directory=session/'ordinary-observer'
    metadata=read_json(directory/'observation.json');status=read_json(directory/'sensors-status.json')
    launch=read_json(session/'launch.json')
    if metadata['runtime_sha256']!=launch['runtime_sha256']:raise ValueError('Observer/runtime identity differs')
    start,duration=observation_window(metadata,start,duration)
    if metadata.get('observer_revision', 0) >= 3:
        receipt=metadata['scene_receipt']
        for field in ('session','runtime_sha256','game_sha256','pid','process_started'):
            if receipt.get(field) != launch.get(field):raise ValueError('Scene/process identity differs')
        from project_paths import project_child
        image=project_child(receipt['screenshot'])
        if not image.is_relative_to(session.resolve()):raise ValueError('Scene image outside session')
        if hashlib.sha256(image.read_bytes()).hexdigest()!=receipt['screenshot_sha256']:raise ValueError('Reviewed image changed')
    with (session/'frames.csv').open(encoding='utf-8-sig',newline='') as stream:anchor=next(csv.DictReader(stream))
    frequency=int(metadata['qpc_frequency']);begin,end=qpc_window(anchor,frequency,start,duration)
    with (directory/'nvidia.csv').open(encoding='utf-8-sig',newline='') as stream:gpu=gpu_summary(csv.DictReader(stream),begin,end,frequency)
    with (directory/'cpu.csv').open(encoding='utf-8-sig',newline='') as stream:cpu=cpu_summary(csv.DictReader(stream),begin,end,frequency)
    return {'schema':1,'complete':metadata['complete'] and metadata['quit_requested'] and status['complete'],
            'window':{'start_s':start,'duration_s':duration,'qpc_begin_inclusive':begin,'qpc_end_inclusive':end,
                      'selection':'Only complete query/CPU intervals wholly inside the conservative first-flip QPC window.'},
            'gpu':gpu,'cpu':cpu,'sensor_status':status,'performance_acceptance':False,
            'limits':['Low-rate sensors do not prove continuous clocks or isolate thermal causes.',
                      'GPU query observed time brackets include controller polling latency, not sensor CPU cost.',
                      'All observed comparisons share this monitoring overhead; do not mix with earlier unobserved runs.',
                      'Temperature, utilization and clock samples are not per-frame GPU execution timings.']}


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--session',type=Path,required=True)
    parser.add_argument('--start',type=float)
    parser.add_argument('--duration',type=float)
    parser.add_argument('--output',type=Path)
    args=parser.parse_args()
    result=observation_summary(args.session,args.start,args.duration)
    target=args.output or args.session/'ordinary-observer/summary.json'
    target.write_text(json.dumps(result,indent=2)+'\n',encoding='utf-8')
    print(json.dumps(result,indent=2))


if __name__=='__main__':main()
