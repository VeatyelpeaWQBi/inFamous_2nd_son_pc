// SPDX-License-Identifier: GPL-2.0-or-later
using System;
using System.Collections.Generic;
using System.IO;
using System.Windows.Forms;

namespace Ifss.Native {
    public static class ReleaseEntry {
        // The engine passes only local output paths; game resources are read-only.
        public static Dictionary<string,object> Defaults(ProjectPaths paths,string game,string mode,string display) {
            var settings=LauncherSettings.Load(paths,"auto",game);
            if(!File.Exists(paths.Child("state/play/launcher.json"))) {
                settings["resolution"]=LauncherSettings.Find(mode).Key;
                settings["display_mode"]=display=="window"?"windowed":display;
                settings["show_fps"]=false;
                LauncherSettings.Save(paths,settings,game);
            }
            return settings;
        }
        [STAThread]
        public static int Main(string[] args) {
            try {
                if(args.Length!=6)throw new InvalidDataException("The game executable must open this menu.");
                var paths=new ProjectPaths(args[0]);string game=Path.GetFullPath(args[1]);
                string response=paths.Child(args[5]);
                if(!args[5].StartsWith("state/runtime/launch-menu-",StringComparison.Ordinal)||!args[5].EndsWith(".json",StringComparison.Ordinal)||File.Exists(response))
                    throw new InvalidDataException("Invalid launch response path.");
                Defaults(paths,game,args[2],args[3]);
                var settings=LaunchMenu.Show(paths,args[4],game);
                if(settings==null)return 2;
                paths.WriteJson(response,settings);return 0;
            } catch(Exception error) {
                MessageBox.Show(error.Message,GameIdentity.Name,MessageBoxButtons.OK,MessageBoxIcon.Error);return 1;
            }
        }
    }
}
