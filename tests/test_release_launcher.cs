// SPDX-License-Identifier: GPL-2.0-or-later
using System;
using System.Collections.Generic;
using System.IO;
namespace Ifss.Native {
    public static class ReleaseMenuTests {
        static void Check(bool value,string message){if(!value)throw new InvalidDataException(message);}
        public static void Run(string root) {
            var paths=new ProjectPaths(root);string game=paths.Child("game/CUSA00004");
            Directory.CreateDirectory(Path.Combine(game,"art/cache"));
            File.WriteAllText(Path.Combine(game,"art/cache/lang_english_text.xpps"),"");
            var first=ReleaseEntry.Defaults(paths,game,"pro4k","borderless");
            Check((string)first["resolution"]=="pro4k"&&(string)first["display_mode"]=="borderless","First launch must inherit root defaults.");
            Check(Json.Integer(first["console_language"])==1,"Missing Chinese resources must select existing English.");
            var selected=new Dictionary<string,object>(first);selected["resolution"]="1080p";selected["display_mode"]="windowed";selected["is_circle_enter"]=true;
            LauncherSettings.Save(paths,selected,game);
            var again=ReleaseEntry.Defaults(paths,game,"pro4k","fullscreen");
            Check((string)again["resolution"]=="1080p"&&(string)again["display_mode"]=="windowed"&&(bool)again["is_circle_enter"],"Saved menu values must survive the next launch.");
            Check((string)LauncherSettings.Load(paths,"pro4k",game)["resolution"]=="pro4k","Explicit command line mode must override initial menu selection.");
            var options=LauncherSettings.Available(paths,game);
            foreach(var resolution in LauncherSettings.Resolutions)foreach(var display in new[]{"fullscreen","windowed","borderless"}){
                selected["resolution"]=resolution.Key;selected["display_mode"]=display;
                LauncherSettings.Save(paths,selected,game);
                var loaded=LauncherSettings.Load(paths,"auto",game);
                Check((string)loaded["resolution"]==resolution.Key&&(string)loaded["display_mode"]==display,"Every display/resolution combination must round trip.");
            }
            bool blocked=false;try{paths.Child("../escaped.json");}catch(InvalidDataException){blocked=true;}
            Check(blocked,"Output traversal must be rejected.");
            Console.WriteLine("Release menu regressions passed: defaults, persistence, language resources, override and 18 mode combinations. No UI/game was opened.");
        }
    }
}
