// SPDX-License-Identifier: GPL-2.0-or-later
using System;
using System.Diagnostics;
using System.Drawing;
using System.IO;
using System.Windows.Forms;

namespace Ifss.Native {
    public static class ExitMenu {
        public static void Show(ProjectPaths paths) {
            GameIdentity.Sta(delegate {
                string activePath = paths.Child("state/active.json");
                if (!File.Exists(activePath)) { MessageBox.Show("当前项目还没有启动过游戏。","退出游戏"); return true; }
                var active = Json.Read(activePath); string relative = Json.Text(active,"session","");
                var session = paths.Child(relative);
                if (!ProjectPaths.Inside(paths.Child("state/sessions"),session)) throw new InvalidDataException("会话路径不属于当前项目。");
                var launch = Json.Read(paths.Child(relative + "/launch.json"));
                if (Json.Bool(launch,"complete",false)) { MessageBox.Show("这轮游戏已经结束。","退出游戏"); return true; }
                using (var form = GameIdentity.Form(paths,"退出游戏",500,230)) {
                    var title = new Label { Text = "退出游戏？", Font = new Font("Segoe UI",16,FontStyle.Bold), AutoSize = true, Location = new Point(24,20) };
                    var hint = new Label { Text = "请先完成游戏内保存。\n确认后将请求游戏正常退出。", AutoSize = true, Location = new Point(24,65) };
                    var errorLabel = new Label { ForeColor = Color.Firebrick, MaximumSize = new Size(450,0), AutoSize = true, Location = new Point(24,115) };
                    var cancel = new Button { Text = "继续游戏", AutoSize = true, Location = new Point(260,178), DialogResult = DialogResult.Cancel };
                    var quit = new Button { Text = "退出游戏", AutoSize = true, Location = new Point(365,178) };
                    form.Controls.AddRange(new Control[] {title,hint,errorLabel,cancel,quit}); form.CancelButton = cancel; form.AcceptButton = quit;
                    quit.Click += delegate {
                        try {
                            // Reuse each branch's control protocol and process identity checks.
                            // In particular, do not replace the development branch's FIFO queue with command.json.
                            var start = new ProcessStartInfo {
                                FileName = GameIdentity.PowerShell, WorkingDirectory = paths.Root, UseShellExecute = false,
                                CreateNoWindow = true, RedirectStandardOutput = true, RedirectStandardError = true,
                                Arguments = "-NoLogo -NoProfile -ExecutionPolicy Bypass -File " + GameIdentity.Quote(Path.Combine(paths.Root,"scripts","control.ps1")) +
                                            " -Action quit -Session " + GameIdentity.Quote(relative)
                            };
                            using (var process = Process.Start(start)) {
                                var output = process.StandardOutput.ReadToEndAsync(); var errors = process.StandardError.ReadToEndAsync();
                                if (!process.WaitForExit(10000)) { process.Kill(); process.WaitForExit(); throw new TimeoutException("退出控制器未响应。"); }
                                output.GetAwaiter().GetResult(); string detail = errors.GetAwaiter().GetResult();
                                if (process.ExitCode != 0) throw new InvalidOperationException("退出请求未送达。" + detail);
                            }
                            form.Close();
                        } catch (Exception error) { errorLabel.Text = "无法发送退出请求：" + error.Message; }
                    };
                    form.ShowDialog(); return true;
                }
            });
        }
    }
}
