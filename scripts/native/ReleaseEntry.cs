// SPDX-License-Identifier: GPL-2.0-or-later
using System;
using System.Collections.Generic;
using System.IO;
using System.Globalization;
using System.Runtime.InteropServices;
using System.Windows.Forms;

namespace Ifss.Native {
    public static class ReleaseEntry {
        [DllImport("user32.dll",SetLastError=true)]
        [return: MarshalAs(UnmanagedType.Bool)]
        static extern bool AllowSetForegroundWindow(uint processId);

        internal static uint EngineProcessId(string value) {
            uint processId;
            if(!UInt32.TryParse(value,NumberStyles.None,CultureInfo.InvariantCulture,out processId) ||
               processId==0 || processId==UInt32.MaxValue)
                throw new InvalidDataException("Invalid game process ID.");
            return processId;
        }

        // The engine passes only local output paths; game resources are read-only.
        public static Dictionary<string,object> Defaults(ProjectPaths paths,string game,string mode,string display) {
            var settings=LauncherSettings.Load(paths,"auto",game);
            if(!File.Exists(paths.Child("state/play/launcher.json"))) {
                settings["resolution"]=LauncherSettings.Find(mode).Key;
                settings["display_mode"]=display=="window"?"windowed":display;
                LauncherSettings.Save(paths,settings,game);
            }
            return settings;
        }
        [STAThread]
        public static int Main(string[] args) {
            try {
                if(args.Length!=7)throw new InvalidDataException("The game executable must open this menu.");
                uint engineProcessId=EngineProcessId(args[6]);
                var paths=new ProjectPaths(args[0]);string game=Path.GetFullPath(args[1]);
                string response=paths.Child(args[5]);
                if(!args[5].StartsWith("state/runtime/launch-menu-",StringComparison.Ordinal)||!args[5].EndsWith(".json",StringComparison.Ordinal)||File.Exists(response))
                    throw new InvalidDataException("Invalid launch response path.");
                Defaults(paths,game,args[2],args[3]);
                // Grant activation while the clicked menu still owns the foreground.
                var settings=LaunchMenu.Show(paths,args[4],game,delegate {
                    AllowSetForegroundWindow(engineProcessId);
                });
                if(settings==null)return 2;
                paths.WriteJson(response,settings);return 0;
            } catch(Exception error) {
                MessageBox.Show(error.Message,GameIdentity.Name,MessageBoxButtons.OK,MessageBoxIcon.Error);return 1;
            }
        }
    }
}
