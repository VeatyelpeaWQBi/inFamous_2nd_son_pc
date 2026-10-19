﻿// SPDX-License-Identifier: GPL-2.0-or-later
using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Xml;
using System.Xml.Linq;

namespace Ifss.Native {
    public static class Preflight {
        public static bool ResolutionPatch(string name) {
            return name.StartsWith("Native ",StringComparison.Ordinal) || name.StartsWith("PS4 Pro mode:",StringComparison.Ordinal);
        }
        public static XElement PrepareXml(string template, string mode) {
            if (new FileInfo(template).Length > 1024 * 1024) throw new InvalidDataException("Patch XML exceeds supported size.");
            var settings = new XmlReaderSettings { DtdProcessing = DtdProcessing.Prohibit, XmlResolver = null, MaxCharactersInDocument = 1024 * 1024 };
            XElement root;
            using (var reader = XmlReader.Create(template,settings)) root = XElement.Load(reader);
            SelectResolution(root,mode); return root;
        }
        public static void SelectResolution(XElement root, string mode) {
            if (root.Name != "Patch") throw new InvalidDataException("Expected a Patch XML document.");
            var selected = LauncherSettings.Find(mode).Patch;
            foreach (var meta in root.Elements("Metadata"))
                if (ResolutionPatch((string)meta.Attribute("Name") ?? ""))
                    meta.SetAttributeValue("isEnabled",(string)meta.Attribute("Name") == selected ? "true" : "false");
        }
        public static List<object> AuditXml(IGameImage image, XElement root, Dictionary<string,object> identity, string mode) {
            var title = Json.Text(identity,"TITLE_ID",""); var version = Json.Text(identity,"APP_VER","");
            if (!root.Elements("TitleID").Elements("ID").Any(id => id.Value == title))
                throw new InvalidDataException("Patch title does not match the game.");
            var patches = new List<object>(); var used = new Dictionary<ulong,byte>(); var active = new List<string>();
            foreach (var meta in root.Elements("Metadata")) {
                if ((string)meta.Attribute("isEnabled") != "true") continue;
                if ((string)meta.Attribute("AppVer") != version || (string)meta.Attribute("AppElf") != "eboot.bin")
                    throw new InvalidDataException("Enabled patch has an incompatible version or executable.");
                string name = (string)meta.Attribute("Name") ?? "";
                if (ResolutionPatch(name)) active.Add(name);
                var lines = new List<object>();
                foreach (var node in meta.Elements("PatchList").Elements("Line")) {
                    if ((string)node.Attribute("Type") != "bytes") throw new InvalidDataException("Only verified literal byte patches are accepted.");
                    string address = ((string)node.Attribute("Address") ?? "").Trim();
                    if (address.StartsWith("0x",StringComparison.OrdinalIgnoreCase)) address = address.Substring(2);
                    ulong absolute = UInt64.Parse(address,NumberStyles.HexNumber,CultureInfo.InvariantCulture);
                    if (absolute < 0x400000) throw new InvalidDataException("Patch address lies below the executable base.");
                    ulong offset = absolute - 0x400000;
                    byte[] value = ProjectPaths.Unhex((string)node.Attribute("Value") ?? "");
                    if (value.Length == 0 || value.Length > 65536) throw new InvalidDataException("Empty or oversized patch payload.");
                    string expected = ((string)node.Attribute("OriginalHash") ?? "").ToLowerInvariant();
                    string actual = GameImage.Fnv1a(image.Read(offset,value.Length));
                    if (expected.Length == 0 || actual != expected) throw new InvalidDataException("OriginalHash mismatch: " + name + " at RVA 0x" + offset.ToString("x"));
                    for (int i = 0; i < value.Length; ++i) {
                        ulong at = checked(offset + (ulong)i); byte previous;
                        if (used.TryGetValue(at,out previous) && previous != value[i])
                            throw new InvalidDataException("Conflicting enabled patches at RVA 0x" + at.ToString("x"));
                        used[at] = value[i];
                    }
                    lines.Add(new Dictionary<string,object> {{"rva","0x"+offset.ToString("x")},{"size",value.Length},{"original_fnv1a",actual}});
                }
                if (lines.Count == 0) throw new InvalidDataException("Enabled patch has no verified payload: " + name);
                patches.Add(new Dictionary<string,object> {{"name",name},{"lines",lines}});
            }
            var selected = LauncherSettings.Find(mode).Patch;
            if (selected == null ? active.Count != 0 : active.Count != 1 || active[0] != selected)
                throw new InvalidDataException("Resolution patch selection is inconsistent.");
            return patches;
        }
        public static void CheckConfig(Dictionary<string,object> config, Resolution resolution) {
            var gpu = Json.Group(config,"GPU"); var general = Json.Group(config,"General");
            if (Json.Integer(Json.Get(gpu,"readbacks_mode",-1)) != 2 || Json.Bool(gpu,"hdr_allowed",true))
                throw new InvalidDataException("The correctness baseline requires precise readbacks and SDR output.");
            if (Json.Bool(general,"neo_mode",false) != (resolution.Profile == "pro4k"))
                throw new InvalidDataException("The selected console mode is inconsistent.");
            if (Json.Integer(Json.Get(gpu,"window_width",0)) != resolution.Width || Json.Integer(Json.Get(gpu,"window_height",0)) != resolution.Height)
                throw new InvalidDataException("Window dimensions do not match the selected mode.");
        }
        public static Dictionary<string,object> Inspect(ProjectPaths paths, string mode, string runtime, string session, bool frozen) {
            if (frozen && mode != "1080p" && mode != "pro4k") throw new InvalidDataException("Frozen benchmarks require an explicit 1080p or pro4k baseline.");
            var preferences = frozen ? null : LauncherSettings.Load(paths,mode);
            if (preferences != null) mode = Json.Text(preferences,"resolution","1080p");
            var resolution = LauncherSettings.Find(mode); var project = Json.Read(Path.Combine(paths.Root,"project.json"));
            var game = paths.Child(Json.Text(project,"game_relative_path","game/CUSA00309"));
            var executable = paths.Child(runtime ?? Json.Text(project,"runtime_path",""));
            var runtimeBuild = ProjectBuild.Verify(paths,project,executable);
            if (ProjectPaths.HashFile(executable) != Json.Text(project,"runtime_sha256",""))
                throw new InvalidDataException("Project runtime fingerprint changed. Run build.bat before use.");
            var image = new GameImage(Path.Combine(game,"eboot.bin"));
            if (ProjectPaths.Hash(image.Data) != Json.Text(project,"game_sha256","")) throw new InvalidDataException("Game executable fingerprint does not match the baseline.");
            var identity = GameImage.Identity(Path.Combine(game,"sce_sys","param.sfo"));
            if (Json.Text(identity,"TITLE_ID","") != Json.Text(project,"title","") || Json.Text(identity,"APP_VER","") != Json.Text(project,"version",""))
                throw new InvalidDataException("Game identity does not match the baseline.");
            var profile = Json.Read(paths.Child("profiles/" + resolution.Profile + ".json"));
            string configPath = paths.Child("state/play/user/custom_configs/CUSA00309.json");
            if (frozen || !File.Exists(configPath)) configPath = paths.Child("state/baseline/user/custom_configs/CUSA00309.json");
            var config = Json.Read(configPath);
            foreach (var group in Json.Object(profile["overrides"]))
                foreach (var value in Json.Object(group.Value)) Json.Group(config,group.Key)[value.Key] = value.Value;
            if (preferences != null) LauncherSettings.Apply(config,preferences);
            LauncherSettings.ApplyResolution(config,mode); CheckConfig(config,resolution);
            var xml = PrepareXml(paths.Child("profiles/CUSA00309.xml"),mode);
            var patches = AuditXml(image,xml,identity,mode);
            var report = new Dictionary<string,object> {
                {"schema",3},{"path_base","project"},{"mode",mode},{"runtime",paths.Relative(executable)},
                {"resolution",new Dictionary<string,object>{{"width",resolution.Width},{"height",resolution.Height}}},
                {"baseline_profile",resolution.Profile},{"runtime_sha256",project["runtime_sha256"]},{"runtime_build",runtimeBuild},{"identity",identity},
                {"game_sha256",project["game_sha256"]},{"patches",patches},{"gpu_config",config["GPU"]},
                {"neo_mode",Json.Group(config,"General")["neo_mode"]},{"launcher_settings",preferences},
                {"runtime_capabilities",new Dictionary<string,object>{{"benchmark_dir",true},{"wait_for_debugger",true},
                    {"evidence","project build receipt and checked source snapshot"},{"dynamic_probe_executed",false}}},
                {"source_game_written",false},{"game_launched",false},{"brightness_verified",false},{"particles_verified",false},{"fps_verified",false}
            };
            if (!String.IsNullOrEmpty(session)) {
                var target = paths.Child(session);
                if (!ProjectPaths.Inside(paths.Child("state/sessions"),target)) throw new InvalidDataException("Session must be below state/sessions/.");
                paths.WriteJson(Path.Combine(target,"custom_configs","CUSA00309.json"),config);
                string patch = Path.Combine(target,"patches","CUSA00309.xml");
                paths.WriteText(patch,"<?xml version=\"1.0\" encoding=\"utf-8\"?>\n" + xml.ToString() + "\n");
                report["patch_xml"] = paths.Relative(patch); paths.WriteJson(Path.Combine(target,"preflight.json"),report);
            }
            return report;
        }
    }
}
