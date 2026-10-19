// SPDX-License-Identifier: GPL-2.0-or-later
using System;
using System.Diagnostics;
using System.Drawing;
using System.IO;
using System.Threading;
using System.Windows.Forms;

namespace Ifss.Native {
    public static class GameIdentity {
        public const string Name = "inFAMOUS Second Son";
        public static string PowerShell {
            get { return Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.System),"WindowsPowerShell","v1.0","powershell.exe"); }
        }
        public static string Quote(string value) {
            var text = new System.Text.StringBuilder("\""); int slashes = 0;
            foreach (char c in value) {
                if (c == '\\') { ++slashes; continue; }
                if (c == '"') { text.Append('\\',slashes * 2 + 1); text.Append(c); }
                else { text.Append('\\',slashes); text.Append(c); }
                slashes = 0;
            }
            text.Append('\\',slashes * 2); return text.Append('"').ToString();
        }
        public static T Sta<T>(Func<T> action) {
            T result = default(T); Exception failure = null;
            var thread = new Thread(delegate() { try { result = action(); } catch (Exception error) { failure = error; } });
            thread.SetApartmentState(ApartmentState.STA); thread.Start(); thread.Join();
            if (failure != null) throw new InvalidOperationException("Windows menu failed: " + failure.Message,failure);
            return result;
        }
        public static Form Form(ProjectPaths paths, string title, int width, int height) {
            Application.EnableVisualStyles();
            var form = new Form { Text = Name + " · " + title, ClientSize = new Size(width,height),
                Font = new Font("Segoe UI",10), FormBorderStyle = FormBorderStyle.FixedDialog,
                MaximizeBox = false, MinimizeBox = false, StartPosition = FormStartPosition.CenterScreen,
                AutoScaleMode = AutoScaleMode.Font };
            string icon = Path.Combine(paths.Root,Name + ".ico");
            if (File.Exists(icon)) form.Icon = new Icon(icon);
            return form;
        }
    }
}
