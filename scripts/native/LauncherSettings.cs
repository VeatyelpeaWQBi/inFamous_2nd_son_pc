// SPDX-License-Identifier: GPL-2.0-or-later
using System;
using System.Collections.Generic;
using System.IO;

namespace Ifss.Native {
    public sealed class Resolution {
        public readonly string Key, Profile, Patch; public readonly int Width, Height;
        public Resolution(string key, int width, int height, string profile, string patch) {
            Key = key; Width = width; Height = height; Profile = profile; Patch = patch;
        }
        public override string ToString() { return Width + " × " + Height + (Profile == "pro4k" ? "（PS4 Pro）" : ""); }
    }
    public sealed class Language {
        public readonly string Name, Asset; public readonly int Code;
        public Language(string name, int code, string asset) { Name = name; Code = code; Asset = asset; }
        public override string ToString() { return Name; }
    }
    public static class LauncherSettings {
        public static readonly Resolution[] Resolutions = {
            new Resolution("720p",1280,720,"1080p","Native 1280x720 (render and output)"),
            new Resolution("900p",1600,900,"1080p","Native 1600x900 (render and output)"),
            new Resolution("1080p",1920,1080,"1080p",null),
            new Resolution("1440p",2560,1440,"pro4k","PS4 Pro mode: native 2560x1440 without checkerboard"),
            new Resolution("1800p",3200,1800,"pro4k","PS4 Pro mode: native 3200x1800 without checkerboard"),
            new Resolution("pro4k",3840,2160,"pro4k","PS4 Pro mode: native 3840x2160 without checkerboard")
        };
        public static readonly Language[] Languages = {
            new Language("繁體中文（香港）",10,"chinese"), new Language("English",1,"english"), new Language("한국어（韩语）",9,"korean")
        };
        public static Resolution Find(string mode) {
            foreach (var item in Resolutions) if (item.Key == mode) return item;
            throw new InvalidDataException("不支持的渲染分辨率：" + mode);
        }
        public static List<Language> Available(ProjectPaths paths, string gameDirectory = null) {
            string game = gameDirectory == null
                ? paths.Child(Json.Text(Json.Read(Path.Combine(paths.Root,"project.json")),"game_relative_path","game/CUSA00309"))
                : Path.GetFullPath(gameDirectory);
            var options = new List<Language>();
            foreach (var language in Languages)
                if (File.Exists(Path.Combine(game,"art","cache","lang_" + language.Asset + "_text.xpps"))) options.Add(language);
            return options;
        }
        public static Dictionary<string, object> Validate(Dictionary<string, object> settings, IList<Language> options) {
            long schema = Json.Integer(Json.Get(settings,"schema",0));
            string display = Json.Text(settings,"display_mode","fullscreen"), resolution = Json.Text(settings,"resolution","1080p");
            Find(resolution);
            long language = Json.Integer(Json.Get(settings,"console_language",-1));
            bool found = false; foreach (var item in options) found |= item.Code == language;
            if (schema < 1 || schema > 3 || !found || !settings.ContainsKey("is_circle_enter") || !settings.ContainsKey("show_fps") || (display != "fullscreen" && display != "windowed" && display != "borderless"))
                throw new InvalidDataException("启动设置无效，或所选语言资源不存在。");
            return new Dictionary<string,object> {
                {"schema",3},{"display_mode",display},{"resolution",resolution},{"console_language",language},
                {"is_circle_enter",Json.Bool(settings,"is_circle_enter",false)},{"show_fps",Json.Bool(settings,"show_fps",false)}
            };
        }
        static string PreviousResolution(ProjectPaths paths) {
            string file = paths.Child("state/play/user/custom_configs/CUSA00309.json");
            if (File.Exists(file)) {
                var gpu = Json.Group(Json.Read(file),"GPU");
                long width = Json.Integer(Json.Get(gpu,"window_width",0)), height = Json.Integer(Json.Get(gpu,"window_height",0));
                foreach (var item in Resolutions) if (width == item.Width && height == item.Height) return item.Key;
            }
            return "1080p";
        }
        public static Dictionary<string, object> Load(ProjectPaths paths, string mode, string gameDirectory = null) {
            var options = Available(paths,gameDirectory);
            if (options.Count == 0) throw new InvalidDataException("未找到游戏语言资源，请检查项目中的游戏目录。");
            string file = paths.Child("state/play/launcher.json");
            Dictionary<string,object> settings;
            if (File.Exists(file)) {
                settings = Json.Read(file);
                if (!settings.ContainsKey("resolution")) settings["resolution"] = PreviousResolution(paths);
            } else {
                string global = paths.Child("state/play/user/config.json"), game = paths.Child("state/play/user/custom_configs/CUSA00309.json");
                if (!File.Exists(global)) global = paths.Child("state/baseline/user/config.json");
                if (!File.Exists(game)) game = paths.Child("state/baseline/user/custom_configs/CUSA00309.json");
                // A release host supplies its resolved game directory and applies its own engine config.
                // Main still requires its existing global/baseline config before launch.
                var globalConfig = File.Exists(global) || gameDirectory == null ? Json.Read(global) : new Dictionary<string,object>();
                var gameConfig = File.Exists(game) ? Json.Read(game) : new Dictionary<string,object>();
                var general = new Dictionary<string,object>(Json.Group(globalConfig,"General"));
                foreach (var entry in Json.Group(gameConfig,"General")) general[entry.Key] = entry.Value;
                var inputs = new Dictionary<string,object>(Json.Group(globalConfig,"Input"));
                foreach (var entry in Json.Group(gameConfig,"Input")) inputs[entry.Key] = entry.Value;
                long language = Json.Integer(Json.Get(general,"console_language",options[0].Code));
                bool found = false; foreach (var item in options) found |= item.Code == language;
                settings = new Dictionary<string,object> {
                    {"schema",3},{"display_mode","fullscreen"},{"resolution",PreviousResolution(paths)},
                    {"console_language",found ? language : options[0].Code},
                    {"is_circle_enter",Json.Bool(inputs,"is_circle_enter",false)},
                    {"show_fps",Json.Bool(Json.Group(globalConfig,"General"),"show_fps_counter",false)}
                };
            }
            settings = Validate(settings,options);
            if (mode != "auto") { Find(mode); settings["resolution"] = mode; }
            return settings;
        }
        public static Dictionary<string,object> Save(ProjectPaths paths, Dictionary<string,object> settings, string gameDirectory = null) {
            settings = Validate(settings,Available(paths,gameDirectory));
            paths.WriteJson(paths.Child("state/play/launcher.json"),settings); return settings;
        }
        public static void ApplyResolution(Dictionary<string,object> config, string mode) {
            var item = Find(mode); Json.Group(config,"General")["neo_mode"] = item.Profile == "pro4k";
            var gpu = Json.Group(config,"GPU"); gpu["window_width"] = item.Width; gpu["window_height"] = item.Height;
        }
        public static void Apply(Dictionary<string,object> config, Dictionary<string,object> settings) {
            Json.Group(config,"General")["console_language"] = settings["console_language"];
            Json.Group(config,"Input")["is_circle_enter"] = settings["is_circle_enter"];
            var display = Json.Text(settings,"display_mode","fullscreen"); var gpu = Json.Group(config,"GPU");
            gpu["full_screen"] = display != "windowed";
            gpu["full_screen_mode"] = display == "fullscreen" ? "Fullscreen" : display == "windowed" ? "Windowed" : "Fullscreen (Borderless)";
        }
    }
}
