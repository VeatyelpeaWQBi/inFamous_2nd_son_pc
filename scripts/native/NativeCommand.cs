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
    public static partial class Command {
        static partial void RunDiagnostic(ProjectPaths paths,string module,string[] args,ref CommandResult result);
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
                case "build-identity": {
                    var a=new ToolArguments(args,new[]{"--configuration","--source-sha256"},new string[0],true);
                    if(a.Positional=="snapshot") output=ProjectBuild.Snapshot(paths);
                    else if(a.Positional=="activate") {
                        string source=a.Get("--source-sha256",null);
                        if(source==null) throw new InvalidDataException("The pre-build source fingerprint is required.");
                        output=ProjectBuild.Activate(paths,a.Get("--configuration","RelWithDebInfo"),source);
                    } else throw new InvalidDataException("Unknown build identity action.");
                    break;
                }
                default: {
                    CommandResult extension=null;RunDiagnostic(paths,module,args,ref extension);
                    if(extension!=null) return extension;
                    throw new InvalidDataException("Unknown native module: " + module);
                }
                }
                return new CommandResult{Output=output==null ? null : Json.Encode(output)};
            } catch (Exception error) {
                return new CommandResult{ExitCode=module=="launch-menu" || module=="exit-menu" ? 2 : 1,Error=module+": "+error.GetBaseException().Message};
            }
        }
    }
}
