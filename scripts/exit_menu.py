"""A Chinese exit confirmation that requests normal shutdown of one pinned session."""
import os
from pathlib import Path
import subprocess
import sys
from game_identity import GAME_NAME, set_menu_icon
from project_paths import ROOT, project_child, relative_path, read_object


def main():
    try:
        import tkinter as tk
        from tkinter import messagebox, ttk
    except ImportError as error:
        print(f'现有 Python 的 Tkinter 不可用：{error}', file=sys.stderr)
        return 2
    try:
        root = tk.Tk()
        root.title(GAME_NAME+' · 退出游戏')
        set_menu_icon(root)
        root.resizable(False, False)
        root.withdraw()
        active_path = project_child('state/active.json')
        if not active_path.is_file():
            messagebox.showinfo('退出游戏', '当前项目还没有启动过游戏。', parent=root)
            root.destroy()
            return 0
        active = read_object(active_path)
        session = project_child(active['session'])
        allowed = project_child('state/sessions')
        if session == allowed or not session.is_relative_to(allowed):
            raise ValueError('会话路径不属于当前项目。')
        launch = read_object(project_child(relative_path(session / 'launch.json')))
        if launch.get('complete'):
            messagebox.showinfo('退出游戏', '这轮游戏已经结束。', parent=root)
            root.destroy()
            return 0
        root.deiconify()
        content = ttk.Frame(root, padding=24)
        content.pack(fill='both', expand=True)
        ttk.Label(content, text='退出游戏？', font=('Segoe UI', 16, 'bold')).pack(anchor='w')
        ttk.Label(content, text='请先完成游戏内保存。\n确认后将请求游戏正常退出。',
                  justify='left').pack(anchor='w', pady=(14, 18))
        error_text = tk.StringVar()
        ttk.Label(content, textvariable=error_text, foreground='#b00020',
                  wraplength=420).pack(anchor='w')
        actions = ttk.Frame(content)
        actions.pack(anchor='e', pady=(12, 0))

        def quit_game():
            try:
                powershell = Path(os.environ['SystemRoot']) / 'System32/WindowsPowerShell/v1.0/powershell.exe'
                completed = subprocess.run(
                    [str(powershell), '-NoLogo', '-NoProfile', '-ExecutionPolicy', 'Bypass',
                     '-File', str(ROOT / 'scripts/control.ps1'), '-Action', 'quit',
                     '-Session', relative_path(session)], cwd=ROOT, capture_output=True,
                    creationflags=subprocess.CREATE_NO_WINDOW, timeout=10)
                if completed.returncode:
                    error_text.set('退出请求未送达。游戏可能已结束，请检查当前游戏窗口。')
                    return
            except (OSError, ValueError, subprocess.TimeoutExpired) as error:
                error_text.set(f'无法发送退出请求：{error}')
                return
            root.destroy()

        ttk.Button(actions, text='继续游戏', command=root.destroy).pack(side='left', padx=(0, 10))
        ttk.Button(actions, text='退出游戏', command=quit_game).pack(side='left')
        root.protocol('WM_DELETE_WINDOW', root.destroy)
        root.bind('<Escape>', lambda _: root.destroy())
        root.mainloop()
        return 0
    except (OSError, ValueError, KeyError, tk.TclError) as error:
        print(f'无法打开退出菜单：{error}', file=sys.stderr)
        return 2


if __name__ == '__main__':
    sys.exit(main())