# SPDX-License-Identifier: GPL-2.0-or-later
"""Game identity shared by BAT, Tk menus and the future engine build."""
from project_paths import ROOT
GAME_NAME = 'inFAMOUS Second Son'
GAME_ICON = ROOT / 'game/CUSA00309/sce_sys/icon0.png'


def set_menu_icon(root):
    import tkinter as tk
    packaged_icon = ROOT / (GAME_NAME + '.ico')
    if packaged_icon.is_file():
        root.iconbitmap(default=str(packaged_icon))
    elif GAME_ICON.is_file():
        root._game_icon = tk.PhotoImage(master=root, file=str(GAME_ICON))
        root.iconphoto(True, root._game_icon)
