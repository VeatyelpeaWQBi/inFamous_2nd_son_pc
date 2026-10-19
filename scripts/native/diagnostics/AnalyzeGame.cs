﻿// SPDX-License-Identifier: GPL-2.0-or-later
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Text;
using System.Text.RegularExpressions;

namespace Ifss.Native {
    public static class AnalyzeGame {
        sealed class Window {
            public string Name; public ulong Address; public int Size;
            public Window(string name,ulong address,int size) { Name=name; Address=address; Size=size; }
        }
        static readonly Window[] Windows = {
            new Window("exposure-allocation",0x172320,160),new Window("exposure-consumer",0x171680,768),
            new Window("particle-manager",0x3fe890,384),new Window("emitter-prepare",0x3efde0,640),
            new Window("feedback-consumer",0x3f3ed0,512),new Window("feedback-list",0x3edef0,640),
            new Window("particle-postprocess",0x3fed00,512),new Window("particle-resource-bindings",0x3fab70,640),
            new Window("particle-sort-stages",0x3f1a60,640),new Window("emitter-program-selection",0x3f2bd0,512),
            new Window("auxiliary-range-reservation",0x16dd50,512),new Window("particle-submit",0x1691e0,640),
            new Window("compute-stage-sync",0x1694e0,256),new Window("compute-completion-poll",0x1695e0,256),
            new Window("busy-wait-helper",0x53b2b0,160),new Window("light-list-allocation",0x16bdf0,96),
            new Window("resolution-setup",0x1eff10,128),new Window("pro-resolution-setup",0x1f0c80,256)
        };
        static void Section(BinaryWriter writer,uint name,uint type,ulong flags,ulong address,ulong offset,ulong size,ulong alignment) {
            writer.Write(name); writer.Write(type); writer.Write(flags); writer.Write(address); writer.Write(offset); writer.Write(size);
            writer.Write((uint)0); writer.Write((uint)0); writer.Write(alignment); writer.Write((ulong)0);
        }
        public static byte[] ElfForWindow(ulong address,byte[] raw) {
            byte[] names = Encoding.ASCII.GetBytes("\0.text\0.shstrtab\0"); ulong namesAt = 64UL + (ulong)raw.Length;
            ulong table = (namesAt + (ulong)names.Length + 7) & ~7UL;
            using (var stream = new MemoryStream()) {
                using (var writer = new BinaryWriter(stream,Encoding.UTF8,true)) {
                    writer.Write(new byte[] {0x7f,0x45,0x4c,0x46,2,1,1}); writer.Write(new byte[9]);
                    writer.Write((ushort)2); writer.Write((ushort)62); writer.Write((uint)1); writer.Write(address);
                    writer.Write((ulong)0); writer.Write(table); writer.Write((uint)0);
                    foreach (ushort value in new ushort[] {64,0,0,64,3,2}) writer.Write(value);
                    writer.Write(raw); writer.Write(names); writer.Write(new byte[checked((int)(table - (ulong)stream.Position))]); writer.Write(new byte[64]);
                    Section(writer,1,1,6,address,64,(ulong)raw.Length,16); Section(writer,7,3,0,0,namesAt,(ulong)names.Length,1);
                }
                return stream.ToArray();
            }
        }
        static string Llvm(ProjectPaths paths,string configured) {
            if (!String.IsNullOrEmpty(configured)) {
                string tool = paths.Resolve(configured);
                if (!File.Exists(tool)) throw new FileNotFoundException("Configured LLVM tool does not exist.");
                return tool;
            }
            foreach (var target in new [] {EnvironmentVariableTarget.Process,EnvironmentVariableTarget.User,EnvironmentVariableTarget.Machine}) {
                foreach (var directory in (Environment.GetEnvironmentVariable("PATH",target) ?? "").Split(';')) {
                    if (String.IsNullOrWhiteSpace(directory)) continue;
                    var candidate = Path.Combine(Environment.ExpandEnvironmentVariables(directory.Trim().Trim('"')),"llvm-objdump.exe");
                    if (File.Exists(candidate)) return candidate;
                }
            }
            foreach (var basePath in new [] {Environment.GetFolderPath(Environment.SpecialFolder.ProgramFilesX86),Environment.GetFolderPath(Environment.SpecialFolder.ProgramFiles)}) {
                var studio = Path.Combine(basePath,"Microsoft Visual Studio"); if (!Directory.Exists(studio)) continue;
                foreach (var version in Directory.GetDirectories(studio).OrderByDescending(x => x,StringComparer.OrdinalIgnoreCase))
                    foreach (var edition in Directory.GetDirectories(version).OrderByDescending(x => x,StringComparer.OrdinalIgnoreCase)) {
                        var candidate = Path.Combine(edition,"VC","Tools","Llvm","x64","bin","llvm-objdump.exe");
                        if (File.Exists(candidate)) return candidate;
                    }
            }
            return null;
        }
        static string Disassemble(string tool,byte[] data,string workingDirectory) {
            var start = new ProcessStartInfo {
                FileName=tool,Arguments="-d --x86-asm-syntax=intel -",WorkingDirectory=workingDirectory,
                UseShellExecute=false,CreateNoWindow=true,RedirectStandardInput=true,RedirectStandardOutput=true,RedirectStandardError=true
            };
            using (var process = Process.Start(start)) {
                var output = process.StandardOutput.ReadToEndAsync(); var errors = process.StandardError.ReadToEndAsync();
                process.StandardInput.BaseStream.Write(data,0,data.Length); process.StandardInput.Close();
                if (!process.WaitForExit(30000)) { process.Kill(); process.WaitForExit(); throw new TimeoutException("LLVM disassembly timed out."); }
                string text = output.GetAwaiter().GetResult(), error = errors.GetAwaiter().GetResult();
                if (process.ExitCode != 0) throw new InvalidOperationException("LLVM failed: " + error);
                return text;
            }
        }
        static bool Contains(byte[] source,byte[] needle) { return Find(source,needle,0) >= 0; }
        static int Find(byte[] source,byte[] needle,int start) {
            for (int i=start;i<=source.Length-needle.Length;++i) {
                int k=0; while (k<needle.Length && source[i+k]==needle[k]) ++k;
                if (k==needle.Length) return i;
            }
            return -1;
        }
        public static Dictionary<string,object> Analyze(ProjectPaths paths,string output,string llvm) {
            var project = Json.Read(Path.Combine(paths.Root,"project.json"));
            var game = paths.Child(Json.Text(project,"game_relative_path","game/CUSA00309"));
            var image = new GameImage(Path.Combine(game,"eboot.bin")); string hash = ProjectPaths.Hash(image.Data);
            if (hash != Json.Text(project,"game_sha256","")) throw new InvalidDataException("Unexpected executable: refuse to interpret known addresses.");
            var identity = GameImage.Identity(Path.Combine(game,"sce_sys","param.sfo"));
            if (Json.Text(identity,"TITLE_ID","") != Json.Text(project,"title","") || Json.Text(identity,"APP_VER","") != Json.Text(project,"version",""))
                throw new InvalidDataException("Game identity does not match the pinned project.");
            string destination = paths.Child(output), tool = Llvm(paths,llvm);
            if (!ProjectPaths.Inside(paths.Child("doc"),destination)) throw new InvalidDataException("Analysis output must stay below doc/.");
            var windows = new List<object>();
            foreach (var window in Windows) {
                var raw = image.Read(window.Address,window.Size); var calls = new List<string>();
                var item = new Dictionary<string,object> {
                    {"name",window.Name},{"rva","0x"+window.Address.ToString("x")},{"size",window.Size},{"sha256",ProjectPaths.Hash(raw)},
                    {"direct_calls",calls},{"scope","bounded instruction window, not a recovered complete function"}
                };
                if (tool != null) {
                    string assembly = Disassemble(tool,ElfForWindow(window.Address,raw),paths.Root);
                    string name = window.Name + ".asm.txt"; paths.WriteText(Path.Combine(destination,name),assembly); item["disassembly"]=name;
                    foreach (Match call in Regex.Matches(assembly,@"\bcall\s+0x([0-9a-f]+)")) calls.Add("0x"+UInt64.Parse(call.Groups[1].Value,NumberStyles.HexNumber,CultureInfo.InvariantCulture).ToString("x"));
                }
                windows.Add(item);
            }
            var checks = new Dictionary<string,object> {
                {"particle_feedback_load",ProjectPaths.Hex(image.Read(0x3f3eff,3))=="8b4004"},
                {"particle_count_consumed",ProjectPaths.Hex(image.Read(0x3f3f09,3))=="894108"},
                {"exposure_hook_original",ProjectPaths.Hex(image.Read(0x171704,7))=="48c1e7204809c7"},
                {"cpu_poll_uses_rdtsc",Contains(image.Read(0x53b2b0,160),new byte[]{0x0f,0x31})}
            };
            var symbols = new List<object>();
            foreach (var token in new [] {"CParticleSystemInstance","psysnew.cpp","luminance","Luminance"}) {
                var offsets = new List<int>(); var bytes = Encoding.ASCII.GetBytes(token); int at=0;
                while (offsets.Count<16 && (at=Find(image.Data,bytes,at))>=0) { offsets.Add(at); ++at; }
                symbols.Add(new Dictionary<string,object>{{"token",token},{"file_offsets",offsets}});
            }
            var report = new Dictionary<string,object> {
                {"schema",1},{"identity",identity},{"sha256",hash},{"input_bytes",image.Data.Length},{"segments",image.Segments},
                {"llvm",tool==null ? null : Path.GetFileName(tool)},{"windows",windows},{"checks",checks},{"strings",symbols},
                {"source_game_written",false},{"runtime_measured",false},{"function_recovery_complete",false}
            };
            paths.WriteJson(Path.Combine(destination,"game-analysis.json"),report);
            if (checks.Values.Any(value => !Object.Equals(value,true))) throw new InvalidDataException("Named instruction checks failed; review the report.");
            return report;
        }
    }
}
