﻿// SPDX-License-Identifier: GPL-2.0-or-later
using System;
using System.Collections.Generic;
using System.Drawing;
using System.Windows.Forms;

namespace Ifss.Native {
    public static class LaunchMenu {
        static void Add(TableLayoutPanel table, string caption, Control control) {
            int row = table.RowCount++; table.RowStyles.Add(new RowStyle(SizeType.AutoSize));
            var label = new Label { Text = caption, AutoSize = true, Anchor = AnchorStyles.Left, Margin = new Padding(0,10,18,10) };
            control.Dock = DockStyle.Fill; control.Margin = new Padding(0,6,0,6);
            table.Controls.Add(label,0,row); table.Controls.Add(control,1,row);
        }
        static void Text(TableLayoutPanel table, string text, bool title) {
            int row = table.RowCount++; table.RowStyles.Add(new RowStyle(SizeType.AutoSize));
            var label = new Label { Text = text, AutoSize = true, MaximumSize = new Size(520,0), Margin = new Padding(0,10,0,10) };
            if (title) label.Font = new Font("Segoe UI",16,FontStyle.Bold);
            table.Controls.Add(label,0,row); table.SetColumnSpan(label,2);
        }
        static ComboBox Choices(object[] values, int selected) {
            var combo = new ComboBox { DropDownStyle = ComboBoxStyle.DropDownList, DropDownHeight = 300 };
            combo.Items.AddRange(values); combo.SelectedIndex = selected; return combo;
        }
        public static Dictionary<string,object> Show(ProjectPaths paths, string mode, string gameDirectory = null, Action beforeLaunch = null) {
            return GameIdentity.Sta(delegate {
                var settings = LauncherSettings.Load(paths,mode,gameDirectory); var languages = LauncherSettings.Available(paths,gameDirectory);
                using (var form = GameIdentity.Form(paths,"启动设置",580,640)) {
                    var scroll = new Panel { Dock = DockStyle.Fill, AutoScroll = true, Padding = new Padding(24) };
                    var table = new TableLayoutPanel { AutoSize = true, Dock = DockStyle.Top, ColumnCount = 2, RowCount = 0 };
                    table.ColumnStyles.Add(new ColumnStyle(SizeType.AutoSize)); table.ColumnStyles.Add(new ColumnStyle(SizeType.Percent,100));
                    scroll.Controls.Add(table); form.Controls.Add(scroll);
                    Text(table,GameIdentity.Name,true);
                    int resolutionIndex = Array.FindIndex(LauncherSettings.Resolutions,x => x.Key == (string)settings["resolution"]);
                    var resolution = Choices(LauncherSettings.Resolutions,resolutionIndex); Add(table,"分辨率",resolution);
                    int languageIndex = languages.FindIndex(x => x.Code == Json.Integer(settings["console_language"]));
                    var language = Choices(languages.ToArray(),languageIndex); Add(table,"游戏语言",language);
                    var button = Choices(new object[] {"○ 确认 / Xbox B","× 确认 / Xbox A"},(bool)settings["is_circle_enter"] ? 0 : 1); Add(table,"确认按键",button);
                    string displayKey = (string)settings["display_mode"];
                    var display = Choices(new object[] {"全屏","窗口","无边框窗口"},displayKey == "fullscreen" ? 0 : displayKey == "windowed" ? 1 : 2); Add(table,"显示模式",display);
                    var fps = new CheckBox { Text = "显示帧数（FPS）和帧时间", Checked = (bool)settings["show_fps"], AutoSize = true };
                    Add(table,"帧数显示",fps);
#if IFSS_RELEASE
                    Text(table,"游戏内辅助菜单：按 Insert 显示／隐藏\n能量快速恢复：每秒恢复上限的 5%，每 10 秒恢复一枚炸弹槽。\n快速积攒大招：每次有效业力行动增加三分之一，三次充满。\n业力经验倍率：在辅助菜单中点击，依次切换 5 倍／10 倍／关闭。\n每次启动默认全部关闭；隐藏菜单不会关闭已开启的功能。\n仅匹配的游戏版本可用；版本不匹配时功能禁用。",false);
#endif
                    Text(table,"鼠标体感与触摸板\nF6 开关鼠标体感；体感模式按住左键喷漆，Home 回正。\nDelete 开关触摸板：左键触摸／拖动，右键按压；双键模拟双指。\nEsc 或切换窗口释放鼠标体感／触摸板。",false);
#if IFSS_RELEASE
                    Text(table,"自动识别手柄（以启动时玩家 1 为准）\nPS 系列：保留原提示和操作，不启用这两项改动。\nXbox 系列：按配置启用 Xbox 提示和触摸板组合键。\n未连接／未知类型：默认不启用。请先连接再启动。\n游戏中换系列或启动后才连接，请重启游戏更新提示。\nDS4Windows／Steam Input 若映射为虚拟 Xbox，\n将按虚拟 Xbox 识别，无法保证识别其实体手柄。",false);
                    Text(table,"Xbox 操作（可在 launcher.json 关闭对应改动）\nRT + RB：触摸板右侧按压；LT + LB：左侧按压。\n两组同按：双指按压；扳机需按过一半。\n同组两键尽量同时按，组合识别窗口约 80 毫秒；单按这些键也会短暂等待。\n组合释放后需先松开同组两键，再恢复技能。\n共用按钮图标按 Xbox 显示；触摸板按压文案随组合键更新。\n单指指纹：LT+LB 或 RT+RB；双指：两组同按。\n滑动操作不变；其他独立手掌动画尚未完整替换。",false);
                    Text(table,"游戏期间\nF1 打开退出确认：Enter / 手柄 A 退出；Esc / 手柄 B 继续。\nF10 开关帧数显示　F12 截图，Alt+F4 退出。",false);
#else
                    Text(table,"游戏期间\nF1 打开退出菜单：Enter / × / Xbox A 退出；Esc / ○ / Xbox B 继续。\nF10 开关帧数显示　F12 截图　F3 打开设置。\n也可双击 quit.bat 打开外部退出窗口。",false);
#endif
                    Text(table,"语音：英语。窗口默认最大化；全屏／无边框覆盖屏幕。\n分辨率对应游戏渲染尺寸；1440p 及以上自动使用 PS4 Pro。\n设置保存在本项目，下次启动继续使用。",false);
                    var errorLabel = new Label { AutoSize = true, ForeColor = Color.Firebrick, MaximumSize = new Size(500,0) };
                    Add(table,"",errorLabel);
                    var actions = new FlowLayoutPanel { AutoSize = true, FlowDirection = FlowDirection.RightToLeft };
                    var start = new Button { Text = "启动游戏", AutoSize = true }; var cancel = new Button { Text = "取消", AutoSize = true, DialogResult = DialogResult.Cancel };
                    actions.Controls.Add(start); actions.Controls.Add(cancel); Add(table,"",actions);
                    start.Click += delegate {
                        try {
                            settings = LauncherSettings.Save(paths,new Dictionary<string,object> {
                                {"schema",3},{"resolution",((Resolution)resolution.SelectedItem).Key},
                                {"console_language",((Language)language.SelectedItem).Code},{"is_circle_enter",button.SelectedIndex == 0},
                                {"show_fps",fps.Checked},{"display_mode",display.SelectedIndex == 0 ? "fullscreen" : display.SelectedIndex == 1 ? "windowed" : "borderless"}
                            },gameDirectory);
                            if(beforeLaunch != null) beforeLaunch();
                            form.DialogResult = DialogResult.OK; form.Close();
                        } catch (Exception error) { errorLabel.Text = "无法保存设置：" + error.Message; }
                    };
                    form.AcceptButton = start; form.CancelButton = cancel;
                    form.Shown += delegate { language.Focus(); };
                    return form.ShowDialog() == DialogResult.OK ? settings : null;
                }
            });
        }
    }
}
