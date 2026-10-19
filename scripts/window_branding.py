# SPDX-License-Identifier: GPL-2.0-or-later
"""Set game title/icons on the validated BAT-started SDL window; no game-memory access."""
import argparse
import ctypes
from ctypes import wintypes
from datetime import datetime, timezone
import os
from pathlib import Path
import re
import time
from game_identity import ROOT, GAME_NAME
from project_paths import project_child, relative_path, read_object, write_json


def filetime(value):
    parsed=datetime.fromisoformat(value.replace('Z','+00:00')).astimezone(timezone.utc)
    delta=parsed-datetime(1601,1,1,tzinfo=timezone.utc)
    ticks=(delta.days*86400+delta.seconds)*10_000_000+parsed.microsecond*10
    fraction=re.search(r'\.(\d+)',value)
    return ticks+(int(fraction[1][6]) if fraction and len(fraction[1])>6 else 0)


def run(session):
    if os.name!='nt' or ctypes.sizeof(ctypes.c_void_p)!=8:raise ValueError('Existing Windows x64 Python is required')
    launch=read_object(project_child(relative_path(session/'launch.json')))
    manifest=read_object(ROOT/'project.json')
    kernel=ctypes.WinDLL('kernel32',use_last_error=True);user=ctypes.WinDLL('user32',use_last_error=True)
    kernel.OpenProcess.argtypes=[wintypes.DWORD,wintypes.BOOL,wintypes.DWORD];kernel.OpenProcess.restype=wintypes.HANDLE
    kernel.CloseHandle.argtypes=[wintypes.HANDLE]
    kernel.WaitForSingleObject.argtypes=[wintypes.HANDLE,wintypes.DWORD];kernel.WaitForSingleObject.restype=wintypes.DWORD
    kernel.QueryFullProcessImageNameW.argtypes=[wintypes.HANDLE,wintypes.DWORD,wintypes.LPWSTR,ctypes.POINTER(wintypes.DWORD)]
    kernel.GetProcessTimes.argtypes=[wintypes.HANDLE]+[ctypes.POINTER(wintypes.FILETIME)]*4
    callback_type=ctypes.WINFUNCTYPE(wintypes.BOOL,wintypes.HWND,wintypes.LPARAM)
    user.EnumWindows.argtypes=[callback_type,wintypes.LPARAM]
    user.GetWindowThreadProcessId.argtypes=[wintypes.HWND,ctypes.POINTER(wintypes.DWORD)]
    user.IsWindowVisible.argtypes=[wintypes.HWND];user.IsWindowVisible.restype=wintypes.BOOL
    user.GetWindow.argtypes=[wintypes.HWND,wintypes.UINT];user.GetWindow.restype=wintypes.HWND
    user.GetClassNameW.argtypes=[wintypes.HWND,wintypes.LPWSTR,ctypes.c_int]
    user.GetWindowTextW.argtypes=[wintypes.HWND,wintypes.LPWSTR,ctypes.c_int]
    user.SendMessageTimeoutW.argtypes=[wintypes.HWND,wintypes.UINT,wintypes.WPARAM,wintypes.LPARAM,
                                       wintypes.UINT,wintypes.UINT,ctypes.POINTER(ctypes.c_size_t)]
    user.SendMessageTimeoutW.restype=ctypes.c_size_t
    user.LoadImageW.argtypes=[wintypes.HINSTANCE,wintypes.LPCWSTR,wintypes.UINT,ctypes.c_int,ctypes.c_int,wintypes.UINT]
    user.LoadImageW.restype=wintypes.HANDLE
    user.DestroyIcon.argtypes=[wintypes.HANDLE]
    user.ShowWindowAsync.argtypes=[wintypes.HWND,ctypes.c_int]
    user.ShowWindowAsync.restype=wintypes.BOOL
    handle=kernel.OpenProcess(0x100000|0x1000,False,int(launch['pid']))
    if not handle:raise ctypes.WinError(ctypes.get_last_error())
    icons=[];applied={};sized=set()
    display_mode=(launch.get('launcher_settings') or {}).get('display_mode')
    try:
        name=ctypes.create_unicode_buffer(32768);length=wintypes.DWORD(len(name))
        if not kernel.QueryFullProcessImageNameW(handle,0,name,ctypes.byref(length)):raise ctypes.WinError(ctypes.get_last_error())
        expected=os.path.normcase(str(project_child(manifest['runtime_path'])))
        if os.path.normcase(str(Path(name.value).resolve()))!=expected or os.path.normcase(str(project_child(launch['runtime'])))!=expected:
            raise ValueError('Window branding process path differs from the pinned runtime')
        times=[wintypes.FILETIME() for _ in range(4)]
        if not kernel.GetProcessTimes(handle,*(ctypes.byref(t) for t in times)):raise ctypes.WinError(ctypes.get_last_error())
        started=times[0].dwLowDateTime|(times[0].dwHighDateTime<<32)
        if abs(started-filetime(launch['process_started']))>10:raise ValueError('Window branding PID was reused')
        icon_path=ROOT/(GAME_NAME+'.ico')
        if icon_path.is_file():
            for size in [32,16]:
                icon=user.LoadImageW(None,str(icon_path),1,size,size,0x10)
                if icon:icons.append((size,icon))
        title=ctypes.create_unicode_buffer(GAME_NAME)
        def send(hwnd,message,wparam,lparam):
            value=ctypes.c_size_t()
            return bool(user.SendMessageTimeoutW(hwnd,message,wparam,lparam,0x2|0x1,100,ctypes.byref(value)))
        @callback_type
        def visit(hwnd,unused):
            pid=wintypes.DWORD();user.GetWindowThreadProcessId(hwnd,ctypes.byref(pid))
            if pid.value!=launch['pid'] or not user.IsWindowVisible(hwnd) or user.GetWindow(hwnd,4):return True
            cls=ctypes.create_unicode_buffer(128);user.GetClassNameW(hwnd,cls,len(cls))
            if not cls.value.lower().startswith('sdl'):return True
            current=ctypes.create_unicode_buffer(512);user.GetWindowTextW(hwnd,current,len(current))
            if current.value!=GAME_NAME and not send(hwnd,0xC,0,ctypes.cast(title,ctypes.c_void_p).value):return True
            # Apply startup maximization once; later manual resizing remains available.
            if display_mode == 'windowed' and hwnd not in sized:
                if user.ShowWindowAsync(hwnd,3):
                    sized.add(hwnd)
            if hwnd not in applied:
                succeeded=bool(icons)
                for size,icon in icons:
                    succeeded=send(hwnd,0x80,1 if size==32 else 0,int(icon)) and succeeded
                if succeeded:
                    applied[hwnd]=True
            return True
        while kernel.WaitForSingleObject(handle,0)==258:
            user.EnumWindows(visit,0)
            time.sleep(0.5)
    finally:
        for _,icon in icons:user.DestroyIcon(icon)
        kernel.CloseHandle(handle)


def main():
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--session',type=Path,required=True);args=parser.parse_args()
    session=project_child(args.session);allowed=project_child('state/sessions')
    if session==allowed or not session.is_relative_to(allowed):parser.error('Session belongs to another project')
    try:run(session)
    except (OSError,ValueError,KeyError) as error:
        write_json(session/'window-branding-error.json', {'error':str(error)})
        return 1
    return 0


if __name__=='__main__':raise SystemExit(main())
