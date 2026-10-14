"""Prelaunch menu adapted from infamous2ndson_pc's language chooser.

Uses only the installed Python's Tkinter. Preferences stay in this project.
"""
import argparse
import sys
from game_identity import GAME_NAME, set_menu_icon
from launcher_settings import (BUTTONS, DISPLAY_MODES, RESOLUTIONS, RESOLUTION_CONFIG,
                               available_languages, load_settings, save_selection)


class LaunchMenu:
    def __init__(self, mode):
        import tkinter as tk
        from tkinter import ttk
        self.settings = load_settings(mode)
        self.options = available_languages()
        self.root = tk.Tk()
        self.result = 1
        self.root.title(GAME_NAME+' · 启动设置')
        set_menu_icon(self.root)
        self.root.resizable(False, False)
        content = ttk.Frame(self.root, padding=24)
        content.grid(sticky='nsew')
        ttk.Label(content, text='inFAMOUS Second Son', font=('Segoe UI', 16, 'bold')).grid(
            row=0, column=0, columnspan=2, sticky='w', pady=(0, 10))
        ttk.Label(content, text='分辨率').grid(
            row=1, column=0, sticky='w', padx=(0, 18), pady=(0, 18))
        self.resolution = ttk.Combobox(content, state='readonly', width=35,
                                       values=[name for name, _ in RESOLUTIONS])
        self.resolution.grid(row=1, column=1, sticky='ew', pady=(0, 18))
        self.resolution.current(next(i for i, (_, key) in enumerate(RESOLUTIONS)
                                     if key == self.settings['resolution']))
        ttk.Label(content, text='游戏语言').grid(row=2, column=0, sticky='w', padx=(0, 18))
        self.language = ttk.Combobox(content, state='readonly', width=28,
                                     values=[name for name, _ in self.options])
        self.language.grid(row=2, column=1, sticky='ew')
        self.language.current(next(i for i, (_, code) in enumerate(self.options)
                                   if code == self.settings['console_language']))
        ttk.Label(content, text='确认按键').grid(row=3, column=0, sticky='w', pady=(14, 0))
        self.button = ttk.Combobox(content, state='readonly', width=28,
                                   values=[name for name, _ in BUTTONS])
        self.button.grid(row=3, column=1, sticky='ew', pady=(14, 0))
        self.button.current(0 if self.settings['is_circle_enter'] else 1)
        ttk.Label(content, text='显示模式').grid(row=4, column=0, sticky='w', pady=(14, 0))
        self.display = ttk.Combobox(content, state='readonly', width=28,
                                     values=[name for name, _ in DISPLAY_MODES])
        self.display.grid(row=4, column=1, sticky='ew', pady=(14, 0))
        self.display.current(next(i for i, (_, mode) in enumerate(DISPLAY_MODES)
                                  if mode == self.settings['display_mode']))
        self.show_fps = tk.BooleanVar(value=self.settings['show_fps'])
        ttk.Checkbutton(content, text='在游戏左上角显示帧数（FPS）', variable=self.show_fps).grid(
            row=5, column=0, columnspan=2, sticky='w', pady=(16, 0))
        help_box = ttk.LabelFrame(content, text='鼠标体感与触摸板', padding=12)
        help_box.grid(row=6, column=0, columnspan=2, sticky='ew', pady=(16, 0))
        ttk.Label(help_box, text='F6  开关鼠标体感：移动控制方向，来回移动模拟摇晃。\n'
                  '体感模式按住左键喷漆；Home 回正。\n'
                  'Delete  开关触摸板：鼠标左键触摸／拖动，右键按压；\n'
                  '同时按住鼠标左右键模拟双指触摸。\n'
                  'Esc 或切换窗口释放鼠标体感／触摸板。',
                  justify='left', wraplength=460).pack(anchor='w')
        help_box = ttk.LabelFrame(content, text='游戏期间', padding=12)
        help_box.grid(row=7, column=0, columnspan=2, sticky='ew', pady=(14, 0))
        ttk.Label(help_box, text='F1  打开退出菜单：Enter / × / Xbox A 退出；\n'
                  'Esc / ○ / Xbox B 取消，继续游戏。\n'
                  'F10  开关帧数显示    F12  截图    F3  打开设置\n'
                  '也可双击 quit.bat 打开中文退出窗口。',
                  justify='left', wraplength=460).pack(anchor='w')
        ttk.Label(content, text='语音：英语。窗口默认最大化；全屏／无边框覆盖屏幕。\n分辨率对应游戏渲染尺寸；1440p 及以上自动使用 PS4 Pro。\n设置保存在本项目，下次启动继续使用。',
                  wraplength=480).grid(row=8, column=0, columnspan=2, sticky='w', pady=(16, 0))
        self.error = tk.StringVar()
        ttk.Label(content, textvariable=self.error, foreground='#b00020', wraplength=480).grid(
            row=9, column=0, columnspan=2, sticky='w', pady=(6, 0))
        actions = ttk.Frame(content)
        actions.grid(row=10, column=0, columnspan=2, sticky='e', pady=(12, 0))
        ttk.Button(actions, text='取消', command=self.cancel).pack(side='left', padx=(0, 10))
        ttk.Button(actions, text='启动游戏', command=self.start).pack(side='left')
        self.root.protocol('WM_DELETE_WINDOW', self.cancel)
        self.root.bind('<Escape>', lambda _: self.cancel())
        self.root.bind('<Return>', lambda _: self.start())
        self.language.focus_set()

    def start(self):
        try:
            self.settings = save_selection({'schema': 3,
                            'resolution': RESOLUTIONS[self.resolution.current()][1],
                            'console_language': self.options[self.language.current()][1],
                            'is_circle_enter': BUTTONS[self.button.current()][1],
                            'show_fps': self.show_fps.get(),
                            'display_mode': DISPLAY_MODES[self.display.current()][1]})
        except (OSError, ValueError) as error:
            self.error.set(f'无法保存设置：{error}')
            return
        self.result = 0
        self.root.destroy()

    def cancel(self):
        self.result = 1
        self.root.destroy()

    def run(self):
        self.root.mainloop()
        return self.result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--mode', choices=('auto', *RESOLUTION_CONFIG), default='auto')
    args = parser.parse_args()
    try:
        import tkinter as tk
    except ImportError as error:
        print(f'现有 Python 的 Tkinter 不可用：{error}。未安装任何依赖。', file=sys.stderr)
        return 2
    try:
        menu = LaunchMenu(args.mode)
        result = menu.run()
        if result == 0:
            print(menu.settings['resolution'])
        return result
    except (OSError, ValueError, KeyError, tk.TclError) as error:
        print(f'无法打开启动设置：{error}', file=sys.stderr)
        return 2


if __name__ == '__main__':
    sys.exit(main())