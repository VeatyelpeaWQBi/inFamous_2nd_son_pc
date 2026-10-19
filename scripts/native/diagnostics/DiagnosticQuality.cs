// SPDX-License-Identifier: GPL-2.0-or-later
using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Text;
using System.Text.RegularExpressions;

namespace Ifss.Native {
    public static class DiagnosticQuality {
        static Dictionary<string,object> M(params object[] values){return DiagnosticData.Map(values);}
        static void Check(bool condition,string error){DiagnosticData.Check(condition,error);}
        static string Read(string directory,string name){return File.ReadAllText(Path.Combine(directory,name),Encoding.UTF8);}
        static double Number(string value){return DiagnosticData.ParseNumber(value);}
        static double Utc(Dictionary<string,object> value,string key){return DiagnosticData.Utc(Json.Text(value,key,""));}
        static long SumMatches(string text,string pattern){return Regex.Matches(text,pattern,RegexOptions.Multiline).Cast<Match>().Sum(m=>DiagnosticData.ParseInt(m.Groups[1].Value));}
        public static Dictionary<string,object> Wpr(string text,double begin,double end,bool requireCpu=true) {
            var result=M();foreach(string kind in new[]{"Buffers","Events"}){
                var match=Regex.Match(text,"Total # Lost "+kind+@"\s*:\s*(\d+)");Check(match.Success,"WPR: missing loss counters");
                long count=DiagnosticData.ParseInt(match.Groups[1].Value);result["lost_"+kind.ToLowerInvariant()]=count;Check(count==0,"WPR: lost "+kind.ToLowerInvariant());
            }
            long gpu=0;foreach(Match match in Regex.Matches(text,@"^\{([0-9a-fA-F-]+)\}\s+(\d+)\s+\d+\s+.*$",RegexOptions.Multiline))
                if(match.Groups[1].Value.Equals("802ec45a-1e99-4b83-9920-87c98277ba9d",StringComparison.OrdinalIgnoreCase))gpu=DiagnosticData.ParseInt(match.Groups[2].Value);
            result["gpu_kernel_events"]=gpu;Check(gpu>0,"WPR: no DxgKrnl GPU kernel events; GPU scheduling coverage is missing");
            foreach(var pair in new[]{new[]{"gpu_queue_events","QueuePacket"},new[]{"gpu_dma_events","DmaPacket"}}){long count=SumMatches(text,@"(\d+)\s+\d+\s+Microsoft-Windows-DxgKrnl/"+pair[1]+"/");result[pair[0]]=count;Check(count>0,"WPR: missing "+pair[0]);}
            foreach(var pair in new[]{new[]{"cpu_samples","Sampled Profile"},new[]{"context_switches","Thread: CSwitch"}}){long count=SumMatches(text,@"(\d+)\s+\d+\s+"+Regex.Escape(pair[1])+@"\s*$");result[pair[0]]=count;Check(count>0 || (pair[0]=="cpu_samples"&&!requireCpu),"WPR: missing "+pair[0]);}
            result["timer_cpu_sampling_required"]=requireCpu;
            foreach(string label in new[]{"Start","End"}){
                var match=Regex.Match(text,label+@" time \(UTC\)\s*:\s*(\d{4}/\d\d/\d\d:\d\d:\d\d:\d\d)\.(\d+)");Check(match.Success,"WPR: missing trace time bounds");
                var time=DateTime.ParseExact(match.Groups[1].Value,"yyyy/MM/dd:HH:mm:ss",CultureInfo.InvariantCulture,DateTimeStyles.AssumeUniversal|DateTimeStyles.AdjustToUniversal);
                result[label.ToLowerInvariant()+"_unix_s"]=(time.Ticks-621355968000000000L)/1e7+Number("0."+match.Groups[2].Value);
            }
            Check((double)result["start_unix_s"]<=begin&&(double)result["end_unix_s"]>=end,"WPR: trace does not cover the gameplay window");return result;
        }
        public static Dictionary<string,object> WprGameplay(string directory,Dictionary<string,object> state,long pid) {
            var scope=DiagnosticData.Load(directory,"wpr-gameplay-cpu.json");Check(DiagnosticData.Int(scope,"target_pid")==pid&&new[]{"window_started","window_finished"}.All(key=>Object.Equals(scope[key],state[key])),"WPR: stale gameplay CPU report");
            long weight=SumMatches(Read(directory,"wpr-gameplay-cpu.txt"),@"^\s*[^,\n]+\(\s*"+pid+@"\)\s*,\s*(\d+)\s*,");Check(weight>0,"WPR: no target CPU samples inside the gameplay window; startup samples cannot prove coverage");
            return M("target_gameplay_profile_weight",weight,"target_gameplay_cpu_sampled",true,"method",scope["method"]);
        }
        public static Dictionary<string,object> VtuneCpu(string directory,Dictionary<string,object> state,long pid) {
            var scope=DiagnosticData.Load(directory,"vtune-gameplay-hotspots.json");Check(DiagnosticData.Int(scope,"target_pid")==pid&&new[]{"window_started","window_finished"}.All(key=>Object.Equals(scope[key],state[key])),"VTune CPU: stale gameplay report");
            double start=Utc(scope,"collection_start_utc"),begin=DiagnosticData.Number(scope["begin_seconds"]),end=DiagnosticData.Number(scope["end_seconds"]);
            Check(DiagnosticData.Finite(begin)&&DiagnosticData.Finite(end)&&end>begin&&Math.Abs(begin-Math.Max(0,Utc(state,"window_started")-start+1))<=.001&&Math.Abs(end-(Utc(state,"window_finished")-start-1))<=.001,"VTune CPU: gameplay time filter does not match the recorded window");
            string[] header=null;int count=0;double cpu=0;
            foreach(var row in DiagnosticData.Rows(Path.Combine(directory,"vtune-gameplay-hotspots.csv"))){
                if(header==null){header=row;Check(new[]{"PID","Function","CPU Time","Module"}.All(key=>header.Contains(key)),"VTune CPU: missing target/function sample columns");continue;}
                if(DiagnosticData.ParseInt(row[Array.IndexOf(header,"PID")])!=pid)continue;
                double value=Number(row[Array.IndexOf(header,"CPU Time")]);Check(DiagnosticData.Finite(value)&&value>=0,"VTune CPU: invalid sampled CPU time");if(value>0){count++;cpu+=value;}
            }
            Check(count>0&&cpu>0,"VTune CPU: no target CPU samples inside the gameplay window");return M("source","VTune hardware event-based sampling","target_pid",pid,"positive_hotspot_rows",count,"sampled_cpu_seconds",cpu,"report_window_seconds",end-begin,"utc_correlation_uncertainty_seconds",1,"wpr_timer_sampling_available",false);
        }
        public static Dictionary<string,object> Frames(string session,double begin,double end) {
            var clock=DiagnosticData.Load(session,"clock.json");long frequency=DiagnosticData.Int(clock,"qpc_frequency");Check(frequency>0,"Frames: invalid QPC frequency");
            long[] previous=null;string[] header=null;long count=0,steps=0;double first=0,last=0,bracket=0;
            string[] columns={"frame","elapsed_ns","interval_ns","utc_ns","qpc_before","qpc_after"};int[] indexes=null;
            foreach(var row in DiagnosticData.Rows(Path.Combine(session,"frames.csv"))){
                if(header==null){header=row;indexes=columns.Select(key=>Array.IndexOf(header,key)).ToArray();Check(indexes.All(index=>index>=0),"Frames: paired clock columns are missing");continue;}
                long[] value=indexes.Select(index=>DiagnosticData.ParseInt(row[index])).ToArray();Check(value[5]>=value[4]&&value[3]>0,"Frames: invalid paired clock sample");
                if(previous==null){Check(value[0]==0&&value[1]==0&&value[2]==0,"Frames: first frame must start at zero");first=value[3]/1e9;}
                else {Check(value[0]==previous[0]+1&&value[1]>previous[1]&&value[2]==value[1]-previous[1],"Frames: discontinuity or inconsistent intervals");Check(value[4]>=previous[5],"Frames: QPC moved backwards");if(Math.Abs((value[3]-previous[3])/1e9-(value[4]-previous[4])/(double)frequency)>.005)steps++;}
                count++;last=value[3]/1e9;bracket=Math.Max(bracket,(value[5]-value[4])*1e9/frequency);previous=value;
            }
            Check(count>=2&&first<=begin&&last>=end,"Frames: no absolute-clock coverage of the gameplay window");return M("rows",count,"qpc_frequency",frequency,"first_unix_s",first,"last_unix_s",last,"max_clock_read_bracket_ns",bracket,"wall_clock_steps_over_5ms",steps,"correlation","per-frame QPC bracket and precise UTC; not hardware scanout");
        }
        public static Dictionary<string,object> Sensors(string path,double begin,double end,double? utcOffset=null) {
            string[] header=null;var times=new List<double>();bool snapshots=false,observation=false;string source="query-process-exit UTC";
            foreach(var row in DiagnosticData.Rows(path)){
                if(header==null){header=row.Select(value=>value.Trim()).ToArray();Check(header.Length==13&&header[0]=="utc"&&header[1]=="query_start_qpc"&&(header[2]=="query_end_qpc"||header[2]=="query_observed_end_qpc"),"Sensors: missing UTC/QPC clock columns");snapshots=utcOffset.HasValue&&header[3]=="timestamp";if(snapshots)source="NVML snapshot local timestamp with recorded launch UTC offset";observation=true;continue;}
                double moment;if(snapshots){var local=DateTime.ParseExact(row[3].Trim(),"yyyy/MM/dd HH:mm:ss.fff",CultureInfo.InvariantCulture);moment=(local.Ticks-621355968000000000L)/1e7-utcOffset.Value;}else moment=DiagnosticData.Utc(row[0]);
                Check(DiagnosticData.ParseInt(row[2])>=DiagnosticData.ParseInt(row[1]),"Sensors: QPC moved backwards");times.Add(moment);
            }
            Check(times.Count>=2&&times.Zip(times.Skip(1),(a,b)=>b>=a).All(value=>value),"Sensors: insufficient or unordered data");Check(times[0]<=begin&&times[times.Count-1]>=end-3,"Sensors: samples do not cover the gameplay window");
            var window=new[]{begin}.Concat(times.Where(time=>begin<time&&time<end)).Concat(new[]{end}).ToArray();double gap=window.Zip(window.Skip(1),(a,b)=>b-a).Max();Check(gap<=5,"Sensors: a sampling gap longer than 5 seconds intersects the gameplay window");
            return M("complete_rows",times.Count,"first_unix_s",times[0],"last_unix_s",times[times.Count-1],"scope","whole GPU; not process-exclusive","max_gap_seconds",times.Zip(times.Skip(1),(a,b)=>b-a).Max(),"max_window_gap_seconds",gap,"utc_source",source,"qpc_end_is_observation_bound",observation);
        }
        public static Dictionary<string,object> Vtune(string directory,double begin,double end) {
            List<string[]> rows;using(var reader=new StreamReader(Path.Combine(directory,"vtune-summary.csv"),Encoding.UTF8,true))rows=Metrics.Csv(reader).ToList();
            var values=new Dictionary<string,string>();foreach(var row in rows)if(row.Length==3)values[row[1]]=row[2];var result=M();
            foreach(string label in new[]{"start","stop"}){var time=DateTime.ParseExact(values["Collection "+label+" time"],"HH:mm:ss dd/MM/yyyy 'UTC'",CultureInfo.InvariantCulture,DateTimeStyles.AssumeUniversal|DateTimeStyles.AdjustToUniversal);result[label+"_unix_s"]=(time.Ticks-621355968000000000L)/1e7;}
            Check((double)result["start_unix_s"]<=begin&&(double)result["stop_unix_s"]>=end-1,"VTune: sampling did not cover the gameplay window");string log=Read(directory,"vtune-errors.log");Check(!log.Contains("Measuring peak DRAM bandwidth is not enabled"),"VTune: DRAM peak bandwidth was not calibrated");
            var bandwidth=rows.First(row=>row.Length>=6&&row[1]=="DRAM, GB/sec");double peak=Number(bandwidth[2]),observed=Number(bandwidth[3]);Check(DiagnosticData.Finite(peak)&&peak>0&&observed<=peak*1.2,"VTune: bandwidth scale is inconsistent with observed bandwidth");bool measured=log.Contains("Peak bandwidth measurement finished.");Check(measured,"VTune: no completed peak bandwidth measurement was recorded");
            foreach(var pair in M("calibration_requested",true,"calibration_measurement_finished",measured,"peak_gbps",peak,"observed_gbps",observed,"raw_cpu_time_seconds",Number(values["CPU Time"]),"attach_calibration_uncertainty",log.Contains("may be not accurate in the attach mode")))result[pair.Key]=pair.Value;return result;
        }
        public static Dictionary<string,object> Exit(string directory,Dictionary<string,object> state,long pid) {
            var ready=DiagnosticData.Load(directory,"exit-ready.json");var receipt=DiagnosticData.Load(directory,"exit-completed.json");const string policy="engine-shutdown-self-terminate";
            Check(DiagnosticData.Int(ready,"pid")==pid&&DiagnosticData.Int(receipt,"pid")==pid&&Json.Text(ready,"policy","")==policy&&Json.Text(receipt,"policy","")==policy&&DiagnosticData.Int(ready,"nsight_exit_code",-1)==0&&DiagnosticData.Flag(ready,"vtune_completed")&&DiagnosticData.Int(state,"nsight_exit_code",-1)==0&&DiagnosticData.Int(state,"vtune_exit_code",-1)==0&&DiagnosticData.Flag(receipt,"shutdown_completed")&&DiagnosticData.Int(receipt,"exit_code",-1)==0&&DiagnosticData.Int(receipt,"exit_unix_ns")/1e9>=Utc(state,"window_finished"),"Diagnostic exit: collector completion or engine shutdown evidence is invalid");
            return M("policy",policy,"engine_shutdown_completed",true,"collectors_exported_before_exit",true,"limitation","reproduced Nsight DLL detach fault bypassed; third-party teardown is not repaired");
        }
        public static Dictionary<string,object> VtuneControl(string directory,Dictionary<string,object> state) {
            var receipt=DiagnosticData.Load(directory,"vtune-control.json");Check(DiagnosticData.Int(state,"vtune_exit_code",-1)==0&&DiagnosticData.Text(state,"vtune_stop_method","")=="native-itt-detach"&&Object.Equals(DiagnosticData.Get(receipt,"sequence"),DiagnosticData.Get(state,"vtune_detach_sequence"))&&Json.Text(receipt,"action","")=="vtune-detach"&&DiagnosticData.Flag(receipt,"accepted")&&String.IsNullOrEmpty(DiagnosticData.Text(receipt,"error",""))&&DiagnosticData.Int(receipt,"applied_unix_ns")>0&&DiagnosticData.Get(state,"vtune_detached_utc")!=null,"VTune: native detach or collector completion evidence is invalid");
            return M("method","native-itt-detach","collector_exit_code",0,"native_receipt_sequence",receipt["sequence"],"detached_utc",state["vtune_detached_utc"]);
        }
        public static Dictionary<string,object> Nsight(string session,Dictionary<string,object> state,long requested) {
            Check(DiagnosticData.Flag(state,"nsight_sdk_accepted"),"Nsight: missing native start acknowledgement");double start=Utc(state,"nsight_scene_anchor_utc");string capture=Path.Combine(session,"user/captures/nsight");
            Check(DiagnosticData.Files(capture,"*.ngfx-gputrace").Any(file=>new FileInfo(file).Length>0),"Nsight: no saved GPU trace");long events=0,metrics=0,frames=0;var tables=M();
            foreach(string path in DiagnosticData.Files(capture,"*.xls")){
                long rows=0,columns=0;string name=Path.GetFileName(path);foreach(var row in DiagnosticData.Rows(path,'\t')){
                    rows++;columns=row.Length;
                    if(name=="D3DPERF_EVENTS.xls"){if(rows==1)continue;Check(row.Length>1&&DiagnosticData.Finite(Number(row[1])),"Nsight: invalid event duration");events++;}
                    else if(name=="GPUTRACE_FRAME.xls"){Check(row.Skip(1).All(value=>DiagnosticData.Finite(Number(value))),"Nsight: invalid hardware metric");Check(frames==0||frames==row.Length-1,"Nsight: frame count changed within the metric table");frames=row.Length-1;metrics++;}
                }
                tables[path.Substring(capture.TrimEnd('\\').Length+1).Replace('\\','/')]=M("rows",rows,"columns",columns);
            }
            Check(events>0&&metrics>0,"Nsight: no decoded GPU events/metrics");Check(frames>=requested,"Nsight: fewer detailed frames than requested");string log=Read(Path.Combine(session,"diagnostics"),"nsight-output.log");
            return M("sdk_start_unix_s",start,"captured_frames",frames,"requested_frames",requested,"event_rows",events,"tables",tables,"frame_metric_rows",metrics,"periodic_samples_merged",log.Contains("samples merged"),"scope","detailed frame snapshot; continuous GPU scheduling is recorded by ETW","coverage_evidence","native SDK start receipt and decoded per-frame hardware metrics");
        }
        public static Dictionary<string,object> Symbols(ProjectPaths paths,Dictionary<string,object> launch) {
            string runtime=paths.Child(Json.Text(launch,"runtime",""));var receipt=Json.Read(Path.Combine(Path.GetDirectoryName(runtime),"build-receipt.json"));string pdb=paths.Child(Json.Text(receipt,"pdb",""));
            Check(ProjectPaths.HashFile(runtime)==Json.Text(launch,"runtime_sha256","")&&ProjectPaths.HashFile(pdb)==Json.Text(receipt,"pdb_sha256",""),"Symbols: recorded runtime or PDB has changed");byte[] info=null;
            using(var stream=File.OpenRead(pdb)){
                byte[] header=DiagnosticData.ReadAt(stream,0,56);Check(Encoding.ASCII.GetString(header).StartsWith("Microsoft C/C++ MSF 7.00",StringComparison.Ordinal),"Symbols: unsupported PDB format");
                uint block=GameImage.U32(header,32),size=GameImage.U32(header,44),map=GameImage.U32(header,52);Check(block>0,"Symbols: invalid PDB block size");int pages=checked((int)(((ulong)size+block-1)/block));byte[] table=DiagnosticData.ReadAt(stream,checked((long)map*block),checked(pages*4));
                var directory=new byte[checked((int)size)];int copied=0;for(int i=0;i<pages;++i){byte[] page=DiagnosticData.ReadAt(stream,checked((long)GameImage.U32(table,(ulong)i*4)*block),(int)Math.Min(block,(uint)(directory.Length-copied)));Buffer.BlockCopy(page,0,directory,copied,page.Length);copied+=page.Length;}
                uint count=GameImage.U32(directory,0);ulong offset=checked(4+(ulong)count*4);GameImage.Range(directory,4,(ulong)count*4);
                for(uint index=0;index<count;++index){uint length=GameImage.U32(directory,4+(ulong)index*4);ulong blocks=length==UInt32.MaxValue?0:((ulong)length+block-1)/block;GameImage.Range(directory,offset,blocks*4);if(index==1){Check(blocks>0,"Symbols: PDB info stream missing");info=DiagnosticData.ReadAt(stream,checked((long)GameImage.U32(directory,offset)*block),28);break;}offset+=blocks*4;}
            }
            Check(info!=null,"Symbols: PDB info stream missing");using(var stream=File.OpenRead(runtime)){
                byte[] dos=DiagnosticData.ReadAt(stream,0,64);long pe=GameImage.U32(dos,60);byte[] header=DiagnosticData.ReadAt(stream,pe,24);int sections=GameImage.U16(header,6),optional=GameImage.U16(header,20);byte[] debug=DiagnosticData.ReadAt(stream,pe+24+112+6*8,8);uint rva=GameImage.U32(debug,0),size=GameImage.U32(debug,4);byte[] table=DiagnosticData.ReadAt(stream,pe+24+optional,checked(sections*40));long directory=-1;
                for(int i=0;i<sections;++i){ulong at=(ulong)i*40;uint virtualSize=GameImage.U32(table,at+8),address=GameImage.U32(table,at+12),rawSize=GameImage.U32(table,at+16),raw=GameImage.U32(table,at+20);if(address<=rva&&(ulong)rva<(ulong)address+Math.Max(virtualSize,rawSize))directory=(long)raw+rva-address;}
                Check(directory>=0,"Symbols: missing CodeView directory");byte[] entries=DiagnosticData.ReadAt(stream,directory,checked((int)size));Check(size%28==0,"Symbols: truncated CodeView directory");
                for(ulong at=0;at<size;at+=28)if(GameImage.U32(entries,at+12)==2){byte[] value=DiagnosticData.ReadAt(stream,GameImage.U32(entries,at+24),checked((int)GameImage.U32(entries,at+16)));if(value.Length>=24&&Encoding.ASCII.GetString(value,0,4)=="RSDS"&&value.Skip(4).Take(16).SequenceEqual(info.Skip(12).Take(16))&&value.Skip(20).Take(4).SequenceEqual(info.Skip(8).Take(4)))return M("pdb_guid_age_match",true,"runtime_sha256_match",true,"pdb_sha256_match",true);}
            }
            throw new InvalidDataException("Symbols: EXE and PDB identity mismatch");
        }
        public static Dictionary<string,object> Inspect(ProjectPaths paths,string session) {
            string directory=Path.Combine(session,"diagnostics");var state=DiagnosticData.Load(directory,"status.json");var plan=DiagnosticData.Load(directory,"plan.json");var launch=DiagnosticData.Load(session,"launch.json");
            if(String.IsNullOrEmpty(DiagnosticData.Text(state,"window_started",""))||String.IsNullOrEmpty(DiagnosticData.Text(state,"window_finished","")))return M("schema",1,"complete",false,"status","incomplete","checks",M(),"errors",new[]{"No completed gameplay window was recorded"},"limitations",new string[0]);
            double begin=Utc(state,"window_started"),end=Utc(state,"window_finished");Check(end>begin,"Gameplay window is empty or reversed");long pid=DiagnosticData.Int(launch,"pid");var errors=new List<string>();var limitations=new List<string>{"Heavy instrumentation results are not normal-game FPS acceptance.","Guest code and anonymous/JIT samples need a guest address map.","Scene/brightness/particles require separate visual and semantic acceptance."};var checks=M();
            var result=M("schema",1,"validator_sha256",ProjectPaths.HashFile(paths.Child("scripts/native/diagnostics/DiagnosticQuality.cs")),"target_pid",pid,"window_seconds",end-begin,"checks",checks,"errors",errors,"limitations",limitations);
            var actions=new Dictionary<string,Func<Dictionary<string,object>>>();actions["wpr"]=delegate{bool timer=DiagnosticData.Text(plan,"cpu_sampling_backend","")!="vtune-hardware";var value=Wpr(Read(directory,"wpr-statistics.txt"),begin,end,timer);if(timer){foreach(var pair in WprGameplay(directory,state,pid))value[pair.Key]=pair.Value;}else value["scope"]="GPU, scheduling, wait stacks, memory and I/O; CPU instruction sampling belongs to VTune";return value;};
            actions["frames"]=()=>Frames(session,begin,end);actions["symbols"]=()=>Symbols(paths,launch);actions["vtune"]=()=>Vtune(directory,begin,end);
            if(DiagnosticData.Array(DiagnosticData.Get(plan,"tools")).Select(Json.Object).Any(tool=>Json.Text(tool,"name","")=="nvidia_smi"&&DiagnosticData.Flag(tool,"available"))){double offset=DateTimeOffset.Parse(Json.Text(launch,"prepared_at",""),CultureInfo.InvariantCulture).Offset.TotalSeconds;actions["sensors"]=()=>Sensors(Path.Combine(directory,"nvidia.csv"),begin,end,offset);}else limitations.Add("NVIDIA device telemetry was not available on this host.");
            if(Json.Text(plan,"gpu_capture_backend",null)=="nsight"){actions["nsight"]=()=>Nsight(session,state,DiagnosticData.Int(plan,"nsight_detail_frames",3));string scope=DiagnosticData.Text(plan,"validation_scope","");if(scope.Length>0)limitations.Add("Vulkan validation scope: "+scope);}
            if(DiagnosticData.Text(plan,"nsight_exit_policy","")=="engine-shutdown-self-terminate"){actions["diagnostic_exit"]=()=>Exit(directory,state,pid);limitations.Add("Nsight diagnostics exit after successful collector export and engine shutdown using self-termination; the reproduced third-party DLL detach fault is bypassed, not repaired.");}
            if(DiagnosticData.Text(plan,"cpu_sampling_backend","")=="vtune-hardware"){actions["cpu_hotspots"]=()=>VtuneCpu(directory,state,pid);limitations.Add("Concurrent VTune hardware sampling suppresses WPR timer CPU sampling; CPU hotspots must pass a separate target-PID and gameplay-window check using VTune. ETW wait/scheduling stacks remain enabled.");}
            if(DiagnosticData.Text(plan,"vtune_stop_method","")=="native-itt-detach")actions["vtune_control"]=()=>VtuneControl(directory,state);
            foreach(var action in actions){try{var value=action.Value();value["validated"]=true;checks[action.Key]=value;}catch(Exception error){if(error is OutOfMemoryException||error is StackOverflowException)throw;string message=action.Key+": "+error.GetBaseException().Message;checks[action.Key]=M("validated",false,"error",message);errors.Add(message);}}
            object check;if(checks.TryGetValue("vtune",out check)&&DiagnosticData.Flag(Json.Object(check),"attach_calibration_uncertainty"))limitations.Add("VTune measured peak bandwidth in attach mode; concurrent system load may bias the scale.");if(checks.TryGetValue("nsight",out check)&&DiagnosticData.Flag(Json.Object(check),"periodic_samples_merged"))limitations.Add("Nsight merged periodic samples; sampling resolution is limited.");
            result["complete"]=errors.Count==0;result["status"]=errors.Count==0?"complete-with-stated-limits":"incomplete";return result;
        }
    }
}
