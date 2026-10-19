// SPDX-License-Identifier: GPL-2.0-or-later
using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Linq;
namespace Ifss.Native {
    public sealed class TimingEvent {
        public long Ns,Thread;public string Kind;public ulong A,B,C,D,Id,Sequence,GuestFrame;
        public TimingEvent(long ns,long thread,string kind,ulong a=0,ulong b=0,ulong c=0,ulong d=0){Ns=ns;Thread=thread;Kind=kind;A=a;B=b;C=c;D=d;}
    }
    public sealed class TimingFrames {
        public Dictionary<string,object> Report;public Dictionary<ulong,Tuple<long,long>> Rows=new Dictionary<ulong,Tuple<long,long>>();
    }
    public static partial class TimingAnalysis {
        public static readonly string[] HostPhases={"acquire","present_flush","present_mutex","queue_present","submit_mutex","queue_submit","swapchain_recreate"};
        static readonly HashSet<string> ProducerKinds=new HashSet<string>(new[]{"read_producer","read_producer_range","read_producer_time","read_producer_domain"});
        static Dictionary<string,object> M(params object[] values){return DiagnosticData.Map(values);}
        static void Check(bool condition,string error){DiagnosticData.Check(condition,error);}
        static long L(Dictionary<string,object> row,string key,long fallback=0){return Convert.ToInt64(DiagnosticData.Get(row,key,fallback),CultureInfo.InvariantCulture);}
        static ulong U(Dictionary<string,object> row,string key){return Convert.ToUInt64(DiagnosticData.Get(row,key,0UL),CultureInfo.InvariantCulture);}
        static long Count(Dictionary<string,long> values,string key){long value;return values.TryGetValue(key,out value)?value:0;}
        static void Add(Dictionary<string,List<long>> values,string key,long number){List<long> list;if(!values.TryGetValue(key,out list))values[key]=list=new List<long>();list.Add(number);}
        static void Add(Dictionary<string,object> values,string key,long number){values[key]=checked(L(values,key)+number);}
        public static TimingEvent One(IEnumerable<TimingEvent> events,string kind){var found=events.Where(e=>e.Kind==kind).ToArray();Check(found.Length==1,"expected one "+kind+", got "+found.Length);return found[0];}
        public static long Ordered(TimingEvent begin,TimingEvent end){Check(end.Ns>=begin.Ns,"negative CPU interval");return checked(end.Ns-begin.Ns);}
        public static Dictionary<string,object> Stats(IEnumerable<long> source){var values=source.OrderBy(value=>value).ToArray();if(values.Length==0)return M("count",0);return M("count",values.Length,"mean_ms",values.Sum(value=>(double)value)/values.Length/1e6,"p50_ms",values[(int)Math.Ceiling(values.Length*.5)-1]/1e6,"p95_ms",values[(int)Math.Ceiling(values.Length*.95)-1]/1e6,"p99_ms",values[(int)Math.Ceiling(values.Length*.99)-1]/1e6,"max_ms",values[values.Length-1]/1e6);}
        public static Dictionary<string,object> ReadPath(IEnumerable<TimingEvent> input){
            var events=input.OrderBy(e=>e.Ns).ToList();var start=One(events,"read_request");var end=One(events,"read_return");long total=Ordered(start,end);Check(start.Thread==end.Thread,"read return changed caller thread");
            var initial=events.Where(e=>e.Kind=="read_cp_begin"&&e.A!=1).ToArray();Check(end.A<=2&&initial.Length==1&&initial[0].A==(end.A==0?2UL:end.A==1?0UL:3UL),"initial command-processor phase disagrees with read mode");
            var cp=new Dictionary<Tuple<ulong,ulong>,TimingEvent>();var seen=new HashSet<Tuple<ulong,ulong>>();var intervals=new List<Tuple<long,long>>();var result=M("total_ns",total);
            foreach(var e in events){
                Check(start.Ns<=e.Ns&&e.Ns<=end.Ns,"read event outside handler lifetime");var key=Tuple.Create(e.A,e.B);
                if(e.Kind=="read_cp_begin"){
                    Check(seen.Add(key),"duplicate command-processor phase");cp[key]=e;TimingEvent request=start;string label="initial_queue_ns";
                    if(e.A==1){var requests=events.Where(r=>r.Kind=="read_completion_request"&&r.A==e.B).ToArray();Check(requests.Length==1,"completion request missing or repeated");request=requests[0];label="completion_queue_ns";}
                    Check(request.Thread==start.Thread,"completion request changed caller thread");Add(result,label,Ordered(request,e));intervals.Add(Tuple.Create(request.Ns,e.Ns));
                }else if(e.Kind=="read_cp_end"){
                    TimingEvent begin;Check(cp.TryGetValue(key,out begin)&&begin.Thread==e.Thread,"unpaired command-processor phase");cp.Remove(key);Add(result,e.A==1?"completion_cp_ns":"initial_cp_ns",Ordered(begin,e));intervals.Add(Tuple.Create(begin.Ns,e.Ns));
                }
            }
            Check(cp.Count==0,"unfinished command-processor phase");var waits=new Dictionary<Tuple<ulong,ulong,ulong>,TimingEvent>();
            foreach(var e in events){var key=Tuple.Create(e.A,e.B,e.C);if(e.Kind=="read_wait_begin"){Check(!waits.ContainsKey(key)&&e.Thread==start.Thread,"duplicate wait or wrong waiting thread");waits[key]=e;}else if(e.Kind=="read_wait_end"){TimingEvent begin;Check(waits.TryGetValue(key,out begin)&&e.Thread==start.Thread,"unpaired semaphore wait");waits.Remove(key);Add(result,"cpu_wait_ns",Ordered(begin,e));intervals.Add(Tuple.Create(begin.Ns,e.Ns));}}
            Check(waits.Count==0,"unfinished semaphore wait");Check(events.Any(e=>e.Kind=="read_cp_begin"),"missing initial command-processor phase");
            long previous=start.Ns,covered=0;foreach(var range in intervals.OrderBy(value=>value.Item1).ThenBy(value=>value.Item2)){Check(range.Item1>=previous,"overlapping primary stages within one read request");previous=range.Item2;covered=checked(covered+range.Item2-range.Item1);}Check(covered<=total,"primary stages exceed handler lifetime");result["handoff_and_unattributed_ns"]=total-covered;
            var fallback=new Dictionary<ulong,TimingEvent>();foreach(var e in events){if(e.Kind=="read_fallback_begin"){Check(!fallback.ContainsKey(e.A),"repeated fallback");fallback[e.A]=e;}else if(e.Kind=="read_fallback_end"){TimingEvent begin;Check(fallback.TryGetValue(e.A,out begin)&&begin.Thread==e.Thread,"unpaired synchronous fallback");fallback.Remove(e.A);Add(result,"nested_fallback",Ordered(begin,e));}}
            Check(fallback.Count==0,"unfinished synchronous fallback");result["mode"]=end.A;result["retry"]=end.B;result["nothing"]=end.C;result["site"]=start.C;result["copies"]=events.Where(e=>e.Kind=="read_copy"&&e.D!=0).Select(e=>e.D).Distinct().OrderBy(value=>value).ToArray();return result;
        }
        public static Dictionary<string,object> FlipPath(IEnumerable<TimingEvent> events){string[] names={"flip_request","flip_prepare","flip_ready","flip_consume","flip_present_return","flip_done"},labels={"cp_handoff_ns","prepare_cpu_ns","ready_queue_ns","present_call_cpu_ns","status_events_cpu_ns"};var points=names.Select(name=>One(events,name)).ToArray();var result=M();for(int i=0;i<labels.Length;++i)result[labels[i]]=Ordered(points[i],points[i+1]);result["total_ns"]=Ordered(points[0],points[points.Length-1]);return result;}
        public static Tuple<string,long> HostPhase(IEnumerable<TimingEvent> events){var begin=One(events,"host_phase_begin");var end=One(events,"host_phase_end");Check(begin.Thread==end.Thread&&begin.A==end.A&&begin.A<(ulong)HostPhases.Length,"host phase identity changed");return Tuple.Create(HostPhases[(int)begin.A],Ordered(begin,end));}
        public static TimingFrames FrameSummary(TextReader reader,double start,double? duration){
            var selected=new TimingFrames();var intervals=new List<long>();string[] header=null;int[] indexes=null;long previous=-1,previousElapsed=0,crossing=0;
            foreach(var row in Metrics.Csv(reader)){
                if(header==null){header=row;indexes=new[]{"frame","elapsed_ns","interval_ns"}.Select(key=>Array.IndexOf(header,key)).ToArray();Check(indexes.All(column=>column>=0),"frames.csv columns are missing");continue;}
                Check(row.Length>indexes.Max(),"frames.csv row width mismatch");long index=DiagnosticData.ParseInt(row[indexes[0]]),elapsed=DiagnosticData.ParseInt(row[indexes[1]]),interval=DiagnosticData.ParseInt(row[indexes[2]]);
                Check(index==previous+1&&elapsed>=previousElapsed&&interval>=0,"frames.csv index/order/interval is invalid");if(index>0)Check(elapsed-previousElapsed==interval,"frames.csv elapsed and interval disagree");previous=index;previousElapsed=elapsed;
                if(elapsed>=start*1e9&&(!duration.HasValue||elapsed<(start+duration.Value)*1e9)){if(interval!=0&&elapsed-interval<start*1e9){crossing++;continue;}selected.Rows[(ulong)index]=Tuple.Create(elapsed,interval);if(interval!=0)intervals.Add(interval);}
            }
            var report=Stats(intervals);report["intervals_crossing_start_excluded"]=crossing;if(intervals.Count>0){double sum=intervals.Sum(value=>(double)value);report["fps"]=1e9*intervals.Count/sum;report["elapsed_s"]=sum/1e9;}report["scene_verified"]=false;report["interpretation"]="Guest flip cadence in the selected window, including any menus/loading. Not GPU render time.";selected.Report=report;return selected;
        }
        public static bool ValidRange(ulong address,ulong size){return size>0&&size<=UInt64.MaxValue-address;}
        static bool Overlaps(ulong a,ulong size,ulong b,ulong span){return a<b+span&&b<a+size;}
        public static Dictionary<string,object> ProducerPath(List<TimingEvent> events){
            var start=One(events,"read_request");var coverage=One(events,"read_producer_coverage");var window=One(events,"read_producer_window");var begin=One(events.Where(e=>e.Kind=="read_cp_begin"&&e.A==0),"read_cp_begin");var end=One(events.Where(e=>e.Kind=="read_cp_end"&&e.A==0),"read_cp_end");
            foreach(var e in events.Where(e=>e.Kind.StartsWith("read_producer",StringComparison.Ordinal)))Check(e.Thread==begin.Thread&&begin.Ns<=e.Ns&&e.Ns<=end.Ns,"producer metadata outside initial command-processor phase");
            Check(ValidRange(start.A,start.B)&&ValidRange(window.A,window.B)&&window.C==start.A&&window.D==start.B&&window.A<=start.A&&start.A+start.B<=window.A+window.B,"producer window does not cover the requested bytes");Check(coverage.A<=coverage.B&&(coverage.D==4||coverage.D==5),"invalid declared-producer coverage metadata");
            var groups=events.Where(e=>ProducerKinds.Contains(e.Kind)).GroupBy(e=>e.A).ToArray();Check(groups.Length<=4,"producer history emitted more than four candidates");var candidates=new List<object>();var occupied=new HashSet<Tuple<ulong,ulong>>();
            foreach(var group in groups){ulong sequence=group.Key;var p=One(group,"read_producer");var r=One(group,"read_producer_range");var t=One(group,"read_producer_time");var d=One(group,"read_producer_domain");ulong queue=p.D&255,kind=(p.D>>8)&255,scopes=p.D>>16;
                Check(coverage.A<=sequence&&sequence<=coverage.B&&sequence!=0&&queue<=1&&kind<=4&&scopes>=1&&scopes<=3&&t.D==p.B&&(kind!=0)==(p.B!=0)&&ValidRange(r.B,r.C)&&r.D!=0&&t.Ns>=0&&t.B<=(ulong)t.Ns&&(t.C==0||(t.C>=t.B&&t.C<=(ulong)t.Ns)),"invalid declared-producer identity/range/time");
                if((scopes&1)!=0)Check(Overlaps(r.B,r.C,start.A,start.B),"producer request-range candidate does not overlap");if((scopes&2)!=0)Check(Overlaps(r.B,r.C,window.A,window.B),"producer window-range candidate does not overlap");foreach(ulong scope in new[]{1UL,2UL})if((scopes&scope)!=0)Check(occupied.Add(Tuple.Create(queue,scope)),"duplicate producer for queue/range");
                candidates.Add(M("sequence",sequence,"command",p.B,"shader",p.C,"kind",kind,"queue",queue,"scopes",scopes,"tick",r.D,"domain",d.B,"submitted_tick_snapshot",d.C,"known_gpu_tick_snapshot",d.D,"declared_ns",t.B,"recorded_ns",t.C,"observed_ns",d.Ns,"address",r.B,"size",r.C));
            }
            return M("oldest_sequence",coverage.A,"write_sequence",coverage.B,"overwritten",coverage.C,"blind_spot",(coverage.D&1)!=0,"candidates",candidates,"site",start.C);
        }
        public static Dictionary<Tuple<ulong,ulong>,Dictionary<string,TimingEvent>> Submissions(Dictionary<ulong,List<TimingEvent>> groups,out Dictionary<string,long> partial){
            var byKey=new Dictionary<Tuple<ulong,ulong>,List<TimingEvent>>();var owners=new Dictionary<Tuple<ulong,ulong,string>,ulong>();partial=new Dictionary<string,long>();
            foreach(var group in groups)foreach(var e in group.Value)if(e.Kind=="submit_queued"||e.Kind=="submit_driver_begin"||e.Kind=="submit_driver_end"){
                Check(e.A!=0&&e.B!=0,"zero submit semaphore domain/tick");var key=Tuple.Create(e.A,e.B);List<TimingEvent> list;if(!byKey.TryGetValue(key,out list))byKey[key]=list=new List<TimingEvent>();list.Add(e);owners[Tuple.Create(e.A,e.B,e.Kind)]=group.Key;
            }
            var result=new Dictionary<Tuple<ulong,ulong>,Dictionary<string,TimingEvent>>();foreach(var pair in byKey){Check(pair.Value.GroupBy(e=>e.Kind).All(group=>group.Count()==1),"duplicate submit stage for semaphore domain/tick");var row=pair.Value.ToDictionary(e=>e.Kind);TimingEvent begin,end,queued;row.TryGetValue("submit_driver_begin",out begin);row.TryGetValue("submit_driver_end",out end);row.TryGetValue("submit_queued",out queued);
                if(begin!=null&&end!=null){Check(begin.Thread==end.Thread&&begin.C==end.C&&owners[Tuple.Create(pair.Key.Item1,pair.Key.Item2,"submit_driver_begin")]==owners[Tuple.Create(pair.Key.Item1,pair.Key.Item2,"submit_driver_end")],"submit driver identity/command count changed");Ordered(begin,end);}else DiagnosticData.AddCount(partial,"driver_pair");
                if(end!=null)Check(end.D==1,"Vulkan submission reported failure");if(queued!=null)Check(queued.D==0||queued.D==1,"invalid submit queue mode");if(queued!=null&&begin!=null)Ordered(queued,begin);else DiagnosticData.AddCount(partial,"enqueue_pair");result[pair.Key]=row;
            }return result;
        }
    }
}
