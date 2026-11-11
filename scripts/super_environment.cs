// SPDX-License-Identifier: GPL-2.0-or-later
// Read-only, bounded observer. Loaded only by the native super-capture driver.
// Uses installed Windows/.NET APIs; no service, driver, configuration or process changes.
using System;
using System.Collections.Generic;
using System.ComponentModel;
using System.Diagnostics;
using System.Globalization;
using System.Runtime.InteropServices;
using System.Text.RegularExpressions;

public sealed class IfssSuperEnvironment : IDisposable {
    [DllImport("kernel32.dll", SetLastError=true)] static extern bool GetSystemTimes(out long idle, out long kernel, out long user);
    [DllImport("kernel32.dll", SetLastError=true)] static extern bool GetProcessTimes(IntPtr p, out long created, out long exited, out long kernel, out long user);
    [DllImport("kernel32.dll", SetLastError=true)] static extern IntPtr OpenProcess(uint access, bool inherit, uint pid);
    [DllImport("kernel32.dll")] static extern bool CloseHandle(IntPtr handle);
    [DllImport("kernel32.dll")] static extern IntPtr GetCurrentProcess();
    [DllImport("kernel32.dll")] static extern uint GetCurrentProcessId();
    [DllImport("kernel32.dll")] static extern ushort GetActiveProcessorGroupCount();
    [DllImport("kernel32.dll")] static extern uint GetActiveProcessorCount(ushort group);
    [DllImport("user32.dll")] static extern bool GetLastInputInfo(ref LastInput data);
    [StructLayout(LayoutKind.Sequential)] struct LastInput { public uint size, tick; }
    [DllImport("pdh.dll", CharSet=CharSet.Unicode)] static extern uint PdhOpenQueryW(string source, UIntPtr data, out IntPtr query);
    [DllImport("pdh.dll", CharSet=CharSet.Unicode)] static extern uint PdhAddEnglishCounterW(IntPtr query, string path, UIntPtr data, out IntPtr counter);
    [DllImport("pdh.dll")] static extern uint PdhCollectQueryData(IntPtr query);
    [DllImport("pdh.dll")] static extern uint PdhCloseQuery(IntPtr query);
    [DllImport("pdh.dll")] static extern uint PdhGetFormattedCounterValue(IntPtr counter, uint format, out uint type, out Formatted value);
    [DllImport("pdh.dll", CharSet=CharSet.Unicode)] static extern uint PdhGetFormattedCounterArrayW(IntPtr counter, uint format, ref uint bytes, out uint count, IntPtr buffer);
    [StructLayout(LayoutKind.Explicit, Size=16)] struct Formatted {
        [FieldOffset(0)] public uint status;
        [FieldOffset(8)] public double value;
    }
    [StructLayout(LayoutKind.Sequential)] struct Item { public IntPtr name; public Formatted data; }
    readonly Dictionary<string,uint> initialization = new Dictionary<string,uint>();
    readonly uint processors = GetActiveProcessorCount(0xffff);
    readonly ushort groups = GetActiveProcessorGroupCount();
    IntPtr query, gpu, diskBytes, diskQueue, processCpu, processIds, dpcTime, interruptTime, target;
    uint targetPid;
    long prevQpc, prevIdle, prevKernel, prevUser, prevGame, targetCreated;
    bool disposed;
    sealed class WatchedCpu { public uint pid; public string name; public IntPtr handle; public long created, last; }
    readonly Dictionary<uint,WatchedCpu> watched = new Dictionary<uint,WatchedCpu>();
    readonly List<WatchedCpu> retired = new List<WatchedCpu>();
    long watchQpc, watchWall;
    int deniedWatch, watchPolls;
    double watchPollCpuMs;
    const uint Format = 0x200 | 0x8000; // PDH_FMT_DOUBLE | PDH_FMT_NOCAP100
    const uint MoreData = 0x800007d2;
    static readonly Regex Instance = new Regex(@"^pid_(\d+)_(.+)$", RegexOptions.CultureInvariant);

    public IfssSuperEnvironment() {
        initialization["query"] = PdhOpenQueryW(null, UIntPtr.Zero, out query);
        if (query != IntPtr.Zero) {
            initialization["gpu"] = PdhAddEnglishCounterW(query, @"\GPU Engine(*)\Utilization Percentage", UIntPtr.Zero, out gpu);
            initialization["disk_bytes"] = PdhAddEnglishCounterW(query, @"\PhysicalDisk(_Total)\Disk Bytes/sec", UIntPtr.Zero, out diskBytes);
            initialization["disk_queue"] = PdhAddEnglishCounterW(query, @"\PhysicalDisk(_Total)\Avg. Disk Queue Length", UIntPtr.Zero, out diskQueue);
            // V2 uses PID-qualified instance names; traditional Process(name#n) rate
            // counters can silently change identity as duplicate-name processes exit.
            initialization["process_cpu"] = PdhAddEnglishCounterW(query, @"\Process V2(*)\% Processor Time", UIntPtr.Zero, out processCpu);
            initialization["process_ids"] = PdhAddEnglishCounterW(query, @"\Process V2(*)\Process ID", UIntPtr.Zero, out processIds);
            initialization["dpc_time"] = PdhAddEnglishCounterW(query, @"\Processor(_Total)\% DPC Time", UIntPtr.Zero, out dpcTime);
            initialization["interrupt_time"] = PdhAddEnglishCounterW(query, @"\Processor(_Total)\% Interrupt Time", UIntPtr.Zero, out interruptTime);
            initialization["prime"] = PdhCollectQueryData(query);
        }
        PrimeCpu();
        watchWall=DateTime.UtcNow.ToFileTimeUtc(); watchQpc=Stopwatch.GetTimestamp();
        PollProcesses();
    }
    long ProcessTicks(IntPtr handle, long expectedCreated) {
        long created, exited, kernel, user;
        if (!GetProcessTimes(handle, out created, out exited, out kernel, out user))
            throw new Win32Exception(Marshal.GetLastWin32Error());
        if (expectedCreated != 0 && (created != expectedCreated || exited != 0))
            throw new InvalidOperationException("Owned game identity changed or exited.");
        return checked(kernel + user);
    }
    void PrimeCpu() {
        if (!GetSystemTimes(out prevIdle, out prevKernel, out prevUser))
            throw new Win32Exception(Marshal.GetLastWin32Error());
        prevGame = target == IntPtr.Zero ? 0 : ProcessTicks(target, targetCreated);
        prevQpc = Stopwatch.GetTimestamp();
    }
    public void SetTarget(uint pid, long createdFileTime) {
        if (disposed || target != IntPtr.Zero || pid == 0 || createdFileTime <= 0)
            throw new InvalidOperationException("Invalid or repeated target registration.");
        var handle = OpenProcess(0x1000, false, pid); // query limited information only
        if (handle == IntPtr.Zero) throw new Win32Exception(Marshal.GetLastWin32Error());
        try {
            ProcessTicks(handle, createdFileTime);
            target = handle; targetPid = pid; targetCreated = createdFileTime;
            PrimeCpu();
        } catch { CloseHandle(handle); target=IntPtr.Zero; targetPid=0; targetCreated=0; throw; }
    }
    // Retain minimal-query handles so CPU used by a process which exits between PDH
    // samples is still measurable. Polling never sends input or modifies any process.
    public void PollProcesses() {
        if(disposed) return;
        long ownBefore=ProcessTicks(GetCurrentProcess(),0);
        var stale=new List<uint>();
        foreach(var pair in watched) {
            long created,exited,kernel,user;
            if(GetProcessTimes(pair.Value.handle,out created,out exited,out kernel,out user) && exited!=0) {
                retired.Add(pair.Value); stale.Add(pair.Key);
            }
        }
        foreach(uint pid in stale) watched.Remove(pid);
        foreach(var process in Process.GetProcesses()) {
            IntPtr pending=IntPtr.Zero;
            try {
                uint pid=(uint)process.Id;
                if(pid==0 || watched.ContainsKey(pid) || watched.Count+retired.Count>=2048) continue;
                var handle=OpenProcess(0x1000,false,pid);
                if(handle==IntPtr.Zero) {deniedWatch++;continue;}
                pending=handle;
                long created,exited,kernel,user;
                if(!GetProcessTimes(handle,out created,out exited,out kernel,out user)) {deniedWatch++;continue;}
                watched.Add(pid,new WatchedCpu {pid=pid,name=process.ProcessName,handle=handle,created=created,
                    last=created>=watchWall ? 0 : kernel+user});
                pending=IntPtr.Zero;
            } catch {deniedWatch++;}
            finally {if(pending!=IntPtr.Zero) CloseHandle(pending);process.Dispose();}
        }
        watchPolls++;
        watchPollCpuMs+=(ProcessTicks(GetCurrentProcess(),0)-ownBefore)/1e4;
    }
    Dictionary<string,object> WatchedProcesses() {
        PollProcesses();
        long now=Stopwatch.GetTimestamp(); double seconds=(now-watchQpc)/(double)Stopwatch.Frequency;
        var sources=new List<Dictionary<string,object>>();
        var exits=new List<object>(); double sum=0,game=0;
        int failures=0;
        var all=new List<WatchedCpu>(watched.Values); all.AddRange(retired);
        foreach(var entry in all) {
            long created,exited,kernel,user;
            if(!GetProcessTimes(entry.handle,out created,out exited,out kernel,out user) || created!=entry.created || kernel+user<entry.last) {failures++;continue;}
            long ticks=kernel+user-entry.last; entry.last=kernel+user;
            double cores=ticks/1e7/Math.Max(seconds,1e-9);
            if(entry.pid==targetPid && created==targetCreated) game+=cores;
            else {
                sum+=cores;
                var record=new Dictionary<string,object>{{"pid",entry.pid},{"process",entry.name},
                    {"core_equiv",cores},{"cpu_seconds",ticks/1e7},{"exited",exited!=0}};
                sources.Add(record);
                if(exited!=0 && exits.Count<32) exits.Add(record);
            }
        }
        sources.Sort((a,b)=>((double)b["core_equiv"]).CompareTo((double)a["core_equiv"]));
        if(sources.Count>12) sources.RemoveRange(12,sources.Count-12);
        foreach(var entry in retired) CloseHandle(entry.handle);
        retired.Clear();
        var result=new Dictionary<string,object>{{"interval_seconds",seconds},{"nongame_observed_core_equiv",sum},
            {"game_core_equiv",game},{"tracked_processes",watched.Count},{"denied_queries",deniedWatch},
            {"failed_queries",failures},{"polls",watchPolls},{"poll_cpu_ms",watchPollCpuMs},
            {"top_nongame_sources",sources},{"exited_sources",exits},
            {"scope","Retained limited-query handles. Processes born/exited entirely between polls and protected processes remain unaccounted; not an absence-of-load proof."}};
        watchQpc=now;watchWall=DateTime.UtcNow.ToFileTimeUtc();deniedWatch=0;watchPolls=0;watchPollCpuMs=0;
        return result;
    }
    double? Scalar(IntPtr counter) {
        uint type; Formatted data;
        if (counter == IntPtr.Zero || PdhGetFormattedCounterValue(counter, Format, out type, out data) != 0 ||
            data.status > 1 || Double.IsNaN(data.value) || Double.IsInfinity(data.value) || data.value < 0) return null;
        return data.value;
    }
    Dictionary<string,double> CounterItems(IntPtr counter, out uint error, out int invalid) {
        error=0; invalid=0;
        var values=new Dictionary<string,double>();
        if(counter==IntPtr.Zero) {error=0xffffffff;return values;}
        uint bytes=0,count=0;
        error=PdhGetFormattedCounterArrayW(counter,Format,ref bytes,out count,IntPtr.Zero);
        if(error!=MoreData || bytes==0 || bytes>2*1024*1024) return values;
        var buffer=Marshal.AllocHGlobal((int)bytes);
        try {
            error=PdhGetFormattedCounterArrayW(counter,Format,ref bytes,out count,buffer);
            int stride=Marshal.SizeOf(typeof(Item));
            if(error!=0 || (ulong)count*(uint)stride>bytes) {error=0xffffffff;return values;}
            for(uint i=0;i<count;i++) {
                var item=(Item)Marshal.PtrToStructure(IntPtr.Add(buffer,(int)i*stride),typeof(Item));
                if(item.data.status>1 || Double.IsNaN(item.data.value) || Double.IsInfinity(item.data.value) || item.data.value<0) {invalid++;continue;}
                string name=Marshal.PtrToStringUni(item.name);
                if(name==null || values.ContainsKey(name)) {invalid++;continue;}
                values.Add(name,item.data.value);
            }
            return values;
        } finally {Marshal.FreeHGlobal(buffer);}
    }
    Dictionary<string,object> CpuProcesses() {
        uint cpuError,idError; int invalidCpu,invalidIds;
        var cpus=CounterItems(processCpu,out cpuError,out invalidCpu);
        var ids=CounterItems(processIds,out idError,out invalidIds);
        var rows=new List<Tuple<uint,string,double>>();
        int unmatched=0;
        double owned=0, other=0, observer=0;
        foreach(var entry in cpus) {
            double number;
            if(!ids.TryGetValue(entry.Key,out number) || number!=Math.Truncate(number) || number>UInt32.MaxValue) {unmatched++;continue;}
            uint pid=(uint)number;
            if(pid==0) continue; // Idle/_Total are not independent processes.
            double cores=entry.Value/100.0; // 100% Process counter = one logical processor.
            if(pid==targetPid && targetPid!=0) {owned+=cores;continue;}
            other+=cores;
            if(pid==GetCurrentProcessId()) observer+=cores;
            rows.Add(Tuple.Create(pid,entry.Key,cores));
        }
        rows.Sort((a,b)=>b.Item3.CompareTo(a.Item3));
        var top=new List<object>();
        for(int i=0;i<Math.Min(12,rows.Count);i++)
            top.Add(new Dictionary<string,object>{{"pid",rows[i].Item1},{"instance",rows[i].Item2},{"core_equiv",rows[i].Item3}});
        return new Dictionary<string,object>{{"available",cpuError==0 && idError==0 && cpus.Count>0 &&
            invalidCpu==0 && invalidIds==0 && unmatched==0},{"cpu_status",cpuError},{"id_status",idError},
            {"invalid_cpu_instances",invalidCpu},{"invalid_id_instances",invalidIds},{"unmatched_instances",unmatched},
            {"game_core_equiv",owned},{"nongame_process_core_equiv",other},{"observer_core_equiv",observer},
            {"observer_pid",GetCurrentProcessId()},{"top_nongame_sources",top},
            {"scope","Process V2 rate counters, independently aligned PDH interval; includes collector/compositor/system process work. Does not reclassify or zero the system-time gate."}};
    }
    Dictionary<string,object> Gpu() {
        var result = new Dictionary<string,object>{{"available",false}};
        if (gpu == IntPtr.Zero) return result;
        uint bytes=0, count=0;
        uint status = PdhGetFormattedCounterArrayW(gpu, Format, ref bytes, out count, IntPtr.Zero);
        if (status != MoreData || bytes == 0 || bytes > 2*1024*1024) { result["status"]=status; return result; }
        var buffer = Marshal.AllocHGlobal((int)bytes);
        try {
            status=PdhGetFormattedCounterArrayW(gpu, Format, ref bytes, out count, buffer);
            int stride=Marshal.SizeOf(typeof(Item));
            if (status != 0 || (ulong)count*(uint)stride > bytes) { result["status"]=status; return result; }
            var engines=new Dictionary<string,double>();
            var unrelated=new Dictionary<string,double>();
            var compositor=new Dictionary<string,double>();
            var owned=new Dictionary<string,double>();
            var contributors=new List<Tuple<uint,string,double>>();
            var compositors=new HashSet<uint>();
            bool classified=true;
            int session;
            using (var process=Process.GetCurrentProcess()) session=process.SessionId;
            foreach(var process in Process.GetProcessesByName("dwm")) {
                try { if (process.SessionId==session) compositors.Add((uint)process.Id); }
                catch { classified=false; }
                finally { process.Dispose(); }
            }
            if (compositors.Count==0) classified=false;
            int valid=0, invalid=0;
            for (uint i=0;i<count;i++) {
                var item=(Item)Marshal.PtrToStructure(IntPtr.Add(buffer,(int)i*stride),typeof(Item));
                if (item.data.status > 1 || Double.IsNaN(item.data.value) || Double.IsInfinity(item.data.value) || item.data.value < 0) { invalid++; continue; }
                string name=Marshal.PtrToStringUni(item.name);
                var match=Instance.Match(name ?? "");
                uint pid;
                if (!match.Success || !UInt32.TryParse(match.Groups[1].Value,NumberStyles.None,CultureInfo.InvariantCulture,out pid)) { invalid++; continue; }
                valid++;
                string engine=match.Groups[2].Value;
                double previous;
                if (pid == targetPid && targetPid != 0) {
                    owned.TryGetValue(engine,out previous); owned[engine]=previous+item.data.value;
                    continue;
                }
                engines.TryGetValue(engine,out previous);
                engines[engine]=previous+item.data.value; // same hardware engine only
                var category=compositors.Contains(pid) ? compositor : unrelated;
                category.TryGetValue(engine,out previous); category[engine]=previous+item.data.value;
                if (item.data.value>=0.1 && contributors.Count<512)
                    contributors.Add(Tuple.Create(pid,engine,item.data.value));
            }
            var peaks=new List<KeyValuePair<string,double>>(engines);
            peaks.Sort((a,b)=>b.Value.CompareTo(a.Value));
            var top=new List<object>();
            for (int i=0;i<Math.Min(4,peaks.Count);i++)
                top.Add(new Dictionary<string,object>{{"engine",peaks[i].Key},{"percent",peaks[i].Value}});
            result["available"]=valid>0 && invalid==0;
            result["classifier_available"]=classified;
            result["valid_instances"]=valid; result["invalid_instances"]=invalid;
            result["nongame_max_engine_percent"]=peaks.Count == 0 ? 0 : peaks[0].Value;
            result["top_nongame_engines"]=top;
            double unrelatedMax=0, compositorMax=0, ownedMax=0;
            foreach(double value in unrelated.Values) unrelatedMax=Math.Max(unrelatedMax,value);
            foreach(double value in compositor.Values) compositorMax=Math.Max(compositorMax,value);
            foreach(double value in owned.Values) ownedMax=Math.Max(ownedMax,value);
            result["unrelated_max_engine_percent"]=unrelatedMax;
            result["compositor_max_engine_percent"]=compositorMax;
            result["game_max_engine_percent"]=ownedMax;
            var gameEngines=new List<object>();
            foreach(var entry in owned)
                if(entry.Value>0.01 && gameEngines.Count<16)
                    gameEngines.Add(new Dictionary<string,object>{{"engine",entry.Key},{"percent",entry.Value}});
            result["game_engines"]=gameEngines;
            result["compositor_pids"]=new List<uint>(compositors);
            contributors.Sort((a,b)=>b.Item3.CompareTo(a.Item3));
            var sources=new List<object>();
            for(int i=0;i<Math.Min(8,contributors.Count);i++) {
                var entry=contributors[i]; string name="unavailable";
                try { using(var process=Process.GetProcessById((int)entry.Item1)) name=process.ProcessName; } catch { }
                sources.Add(new Dictionary<string,object>{{"pid",entry.Item1},{"process",name},
                    {"engine",entry.Item2},{"percent",entry.Item3},{"compositor",compositors.Contains(entry.Item1)}});
            }
            result["top_nongame_sources"]=sources;
            return result;
        } finally { Marshal.FreeHGlobal(buffer); }
    }
    public Dictionary<string,object> Read() {
        if (disposed) throw new ObjectDisposedException("IfssSuperEnvironment");
        long begin=Stopwatch.GetTimestamp(), ownBefore=ProcessTicks(GetCurrentProcess(),0);
        long idle, kernel, user;
        bool cpu=GetSystemTimes(out idle,out kernel,out user);
        long game=prevGame;
        string gameError=null;
        try { if (target != IntPtr.Zero) game=ProcessTicks(target,targetCreated); }
        catch (Exception e) { cpu=false; gameError=e.Message; }
        long cpuQpc=Stopwatch.GetTimestamp();
        double seconds=(double)(cpuQpc-prevQpc)/Stopwatch.Frequency;
        double system=(kernel-prevKernel+user-prevUser-idle+prevIdle)/1e7/Math.Max(seconds,1e-9);
        double gameCore=(game-prevGame)/1e7/Math.Max(seconds,1e-9);
        cpu=cpu && groups==1 && seconds>=0.5 && seconds<=10 &&
            idle>=prevIdle && kernel>=prevKernel && user>=prevUser && game>=prevGame &&
            system>=0 && system<=processors+0.25 && gameCore>=0 && gameCore<=system+0.1;
        var result=new Dictionary<string,object>{{"schema",2},{"utc",DateTime.UtcNow.ToString("o")},
            {"qpc_before",begin},{"qpc_cpu",cpuQpc},{"qpc_frequency",Stopwatch.Frequency},
            {"interval_start_qpc",prevQpc},{"interval_seconds",seconds},{"target_pid",targetPid},
            {"cpu_available",cpu},{"logical_processors",processors},{"processor_groups",groups},
            {"system_busy_core_equiv",system},{"game_core_equiv",gameCore},
            {"nongame_core_equiv",Math.Max(0,system-gameCore)},{"target_error",gameError}};
        var last=new LastInput { size=(uint)Marshal.SizeOf(typeof(LastInput)) };
        result["last_input_idle_seconds"]=GetLastInputInfo(ref last) ?
            (object)(unchecked((uint)Environment.TickCount-last.tick)/1000.0) : null;
        prevQpc=cpuQpc; prevIdle=idle; prevKernel=kernel; prevUser=user; prevGame=game;
        uint pdh=query==IntPtr.Zero ? 0xffffffff : PdhCollectQueryData(query);
        result["pdh_status"]=pdh;
        result["gpu"]=pdh==0 ? Gpu() : new Dictionary<string,object>{{"available",false}};
        result["cpu_processes"]=pdh==0 ? CpuProcesses() : new Dictionary<string,object>{{"available",false}};
        result["cpu_process_watch"]=WatchedProcesses();
        double? dpc=pdh==0 ? Scalar(dpcTime) : null, interrupt=pdh==0 ? Scalar(interruptTime) : null;
        result["system_cpu_context"]=new Dictionary<string,object>{{"available",dpc.HasValue && interrupt.HasValue},
            {"dpc_core_equiv",dpc.HasValue ? (object)(dpc.Value*processors/100.0) : null},
            {"interrupt_core_equiv",interrupt.HasValue ? (object)(interrupt.Value*processors/100.0) : null},
            {"scope","Separate normalized Processor(_Total) counters; not added to process or system CPU time."}};
        double? disk=pdh==0 ? Scalar(diskBytes) : null, queue=pdh==0 ? Scalar(diskQueue) : null;
        result["disk_available"]=disk.HasValue && queue.HasValue;
        result["disk_bytes_per_second"]=disk; result["disk_average_queue"]=queue;
        result["monitor_cpu_ms"]=(ProcessTicks(GetCurrentProcess(),0)-ownBefore)/1e4;
        long end=Stopwatch.GetTimestamp(); result["qpc_after"]=end;
        result["read_wall_ms"]=(end-begin)*1000.0/Stopwatch.Frequency;
        return result;
    }
    public Dictionary<string,uint> Initialization { get { return new Dictionary<string,uint>(initialization); } }
    public void Dispose() {
        if (disposed) return;
        disposed=true;
        foreach(var entry in watched.Values) CloseHandle(entry.handle);
        foreach(var entry in retired) CloseHandle(entry.handle);
        watched.Clear();retired.Clear();
        if (target != IntPtr.Zero) { CloseHandle(target); target=IntPtr.Zero; }
        if (query != IntPtr.Zero) { PdhCloseQuery(query); query=IntPtr.Zero; }
    }
}
