// SPDX-License-Identifier: GPL-2.0-or-later
using System;
using System.Collections.Generic;
using System.IO;
namespace Ifss.Native {
    public static partial class Command {
        static partial void RunDiagnostic(ProjectPaths paths,string module,string[] args,ref CommandResult result) {
            object output=null;
            switch(module) {
            case "metrics": {
                var a=new ToolArguments(args,new[]{"--start","--end","--mode","--output"},new[]{"--scene-confirmed","--brightness-confirmed","--particles-confirmed","--resolution-confirmed"},true);
                if(a.Positional==null)throw new InvalidDataException("Missing frame CSV path.");
                var confirmations=new Dictionary<string,object>();foreach(string key in new[]{"scene","brightness","particles","resolution"})confirmations[key]=a.Flag("--"+key+"-confirmed");
                using(var reader=new StreamReader(paths.Child(a.Positional)))output=Metrics.Summarize(reader,a.Number("--start"),a.Number("--end"),a.Get("--mode",""),confirmations);
                string destination=a.Get("--output",null);if(destination!=null){string path=paths.Child(destination);if(!ProjectPaths.Inside(paths.Child("state/sessions"),path))throw new InvalidDataException("Measurement output must stay below state/sessions/.");paths.WriteJson(path,output);}break;
            }
            case "analyze-game": {
                var a=new ToolArguments(args,new[]{"--output","--llvm"},new string[0],false);var report=AnalyzeGame.Analyze(paths,a.Get("--output","doc/binary-analysis"),a.Get("--llvm",null));
                output=DiagnosticData.Map("sha256",report["sha256"],"windows",((List<object>)report["windows"]).Count,"checks",report["checks"],"llvm",report["llvm"]);break;
            }
            case "diagnostic-quality": {
                var a=new ToolArguments(args,new[]{"--session"},new string[0],false);string session=Session(paths,a.Get("--session",null));var report=DiagnosticQuality.Inspect(paths,session);paths.WriteJson(Path.Combine(session,"diagnostics/data-quality.json"),report);output=DiagnosticData.Map("complete",report["complete"],"errors",report["errors"]);break;
            }
            case "analyze-timing": {
                var a=new ToolArguments(args,new[]{"--session","--start","--duration","--output"},new string[0],false);string session=Session(paths,a.Get("--session",null));double? start=a.Get("--start",null)==null?(double?)null:a.Number("--start"),duration=a.Get("--duration",null)==null?(double?)null:a.Number("--duration");
                var report=TimingAnalysis.Inspect(session,start,duration);string configured=a.Get("--output",null),target=configured==null?Path.Combine(session,"timing-summary.json"):paths.Child(configured);
                if(!ProjectPaths.Inside(session,target))throw new InvalidDataException("Timing summary must remain inside its session.");paths.WriteJson(target,report);bool usable=DiagnosticData.Flag(report,"format_usable");
                string message="Timing evidence: "+(usable?"usable":"incomplete/invalid")+"; "+Path.GetFileName(target);foreach(object error in DiagnosticData.Array(DiagnosticData.Get(report,"errors")))message+=Environment.NewLine+"  "+error;
                result=new CommandResult{ExitCode=usable?0:1,Output=message};return;
            }
            default:return;
            }
            result=new CommandResult{Output=Json.Encode(output)};
        }
        static string Session(ProjectPaths paths,string relative){if(relative==null)throw new InvalidDataException("Missing --session.");string session=paths.Child(relative);if(!ProjectPaths.Inside(paths.Child("state/sessions"),session))throw new InvalidDataException("Diagnostic session must stay below state/sessions/.");return session;}
    }
}
