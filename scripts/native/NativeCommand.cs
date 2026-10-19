// SPDX-License-Identifier: GPL-2.0-or-later
using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;

namespace Ifss.Native {
    public sealed class CommandResult {
        public int ExitCode; public string Output, Error;
    }
    sealed class ToolArguments {
        readonly Dictionary<string,string> values = new Dictionary<string,string>();
        readonly HashSet<string> flags = new HashSet<string>();
        public string Positional;
        public ToolArguments(string[] args,string[] options,string[] switches,bool positional) {
            var knownOptions = new HashSet<string>(options); var knownSwitches = new HashSet<string>(switches);
            for (int i=0;i<args.Length;++i) {
                string key=args[i];
                if (knownOptions.Contains(key)) {
                    if (values.ContainsKey(key) || i+1==args.Length || args[i+1].StartsWith("--",StringComparison.Ordinal))
                        throw new InvalidDataException("Missing or repeated option: " + key);
                    values.Add(key,args[++i]);
                } else if (knownSwitches.Contains(key)) {
                    if (!flags.Add(key)) throw new InvalidDataException("Repeated option: " + key);
                } else if (positional && Positional==null && !key.StartsWith("--",StringComparison.Ordinal)) Positional=key;
                else throw new InvalidDataException("Unknown argument: " + key);
            }
        }
        public string Get(string key,string fallback) { string value; return values.TryGetValue(key,out value) ? value : fallback; }
        public bool Flag(string key) { return flags.Contains(key); }
        public double Number(string key) {
            string value = Get(key,null);
            if (value==null) throw new InvalidDataException("Missing option: " + key);
            return Double.Parse(value,CultureInfo.InvariantCulture);
        }
    }
    public static class Command {
        public static CommandResult Run(string root,string module,string[] args) {
            var paths = new ProjectPaths(root);
            try {
                if (IntPtr.Size!=8) throw new InvalidOperationException("Use Windows x64 PowerShell.");
                object output=null;
                switch (module) {
                case "launch-menu": {
                    var a=new ToolArguments(args,new[]{"--mode","--game"},new string[0],false);
                    var settings=LaunchMenu.Show(paths,a.Get("--mode","auto"),a.Get("--game",null));
                    return settings==null ? new CommandResult{ExitCode=1} : new CommandResult{Output=(string)settings["resolution"]};
                }
                case "exit-menu":
                    new ToolArguments(args,new string[0],new string[0],false); ExitMenu.Show(paths); break;
                case "window-branding": {
                    var a=new ToolArguments(args,new[]{"--session"},new string[0],false);
                    string session=a.Get("--session",null);
                    if (session==null) throw new InvalidDataException("Missing --session.");
                    try { WindowBranding.Run(paths,session); }
                    catch (Exception error) {
                        try { paths.WriteJson(paths.Child(session+"/window-branding-error.json"),new Dictionary<string,object>{{"error",error.Message}}); } catch { }
                        throw;
                    }
                    break;
                }
                case "preflight": {
                    var a=new ToolArguments(args,new[]{"--mode","--runtime","--session"},new[]{"--frozen"},false);
                    output=Preflight.Inspect(paths,a.Get("--mode","auto"),a.Get("--runtime",null),a.Get("--session",null),a.Flag("--frozen")); break;
                }
                case "metrics": {
                    var a=new ToolArguments(args,new[]{"--start","--end","--mode","--output"},
                        new[]{"--scene-confirmed","--brightness-confirmed","--particles-confirmed","--resolution-confirmed"},true);
                    if (a.Positional==null) throw new InvalidDataException("Missing frame CSV path.");
                    var confirmations=new Dictionary<string,object>();
                    foreach (var key in new[]{"scene","brightness","particles","resolution"}) confirmations[key]=a.Flag("--"+key+"-confirmed");
                    using (var reader=new StreamReader(paths.Child(a.Positional)))
                        output=Metrics.Summarize(reader,a.Number("--start"),a.Number("--end"),a.Get("--mode",""),confirmations);
                    string destination=a.Get("--output",null);
                    if (destination!=null) {
                        string path=paths.Child(destination);
                        if (!ProjectPaths.Inside(paths.Child("state/sessions"),path)) throw new InvalidDataException("Measurement output must stay below state/sessions/.");
                        paths.WriteJson(path,output);
                    }
                    break;
                }
                case "analyze-game": {
                    var a=new ToolArguments(args,new[]{"--output","--llvm"},new string[0],false);
                    var report=AnalyzeGame.Analyze(paths,a.Get("--output","doc/binary-analysis"),a.Get("--llvm",null));
                    output=new Dictionary<string,object>{{"sha256",report["sha256"]},{"windows",((List<object>)report["windows"]).Count},{"checks",report["checks"]},{"llvm",report["llvm"]}}; break;
                }
                default: throw new InvalidDataException("Unknown native module: " + module);
                }
                return new CommandResult{Output=output==null ? null : Json.Encode(output)};
            } catch (Exception error) {
                return new CommandResult{ExitCode=module=="launch-menu" || module=="exit-menu" ? 2 : 1,Error=module+": "+error.GetBaseException().Message};
            }
        }
    }
}
