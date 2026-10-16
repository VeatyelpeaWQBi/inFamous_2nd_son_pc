# SPDX-License-Identifier: GPL-2.0-or-later
"""Post RenderDoc's F8 only to the validated diagnostic SDL window."""
import argparse
import ctypes
from ctypes import wintypes
import os
from pathlib import Path
import time

from project_paths import ROOT, project_child, read_object
from window_branding import filetime


def trigger(session_value):
    session = project_child(session_value)
    if not session.is_relative_to(project_child('state/sessions')):
        raise ValueError('Untrusted diagnostic session')
    launch = read_object(project_child(str(Path(session_value) / 'launch.json')))
    manifest = read_object(ROOT / 'project.json')
    expected = project_child(manifest['runtime_path'])
    if (launch.get('schema') != 3 or launch.get('diagnostic') not in ('renderdoc', 'full')
            or not launch.get('game_launched') or launch.get('complete')
            or project_child(launch['runtime']) != expected
            or launch['runtime_sha256'] != manifest['runtime_sha256']):
        raise ValueError('This is not a live RenderDoc session')
    if os.name != 'nt' or ctypes.sizeof(ctypes.c_void_p) != 8:
        raise ValueError('Existing Windows x64 Python is required')
    kernel = ctypes.WinDLL('kernel32', use_last_error=True)
    user = ctypes.WinDLL('user32', use_last_error=True)
    kernel.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
    kernel.OpenProcess.restype = wintypes.HANDLE
    kernel.CloseHandle.argtypes = [wintypes.HANDLE]
    kernel.WaitForSingleObject.argtypes = [wintypes.HANDLE, wintypes.DWORD]
    kernel.WaitForSingleObject.restype = wintypes.DWORD
    kernel.QueryFullProcessImageNameW.argtypes = [wintypes.HANDLE, wintypes.DWORD, wintypes.LPWSTR, ctypes.POINTER(wintypes.DWORD)]
    kernel.GetProcessTimes.argtypes = [wintypes.HANDLE] + [ctypes.POINTER(wintypes.FILETIME)] * 4
    callback_type = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
    user.EnumWindows.argtypes = [callback_type, wintypes.LPARAM]
    user.GetWindowThreadProcessId.argtypes = [wintypes.HWND, ctypes.POINTER(wintypes.DWORD)]
    user.GetClassNameW.argtypes = [wintypes.HWND, wintypes.LPWSTR, ctypes.c_int]
    user.IsWindowVisible.argtypes = [wintypes.HWND]
    user.PostMessageW.argtypes = [wintypes.HWND, wintypes.UINT, wintypes.WPARAM, wintypes.LPARAM]
    user.PostMessageW.restype = wintypes.BOOL
    user.MapVirtualKeyW.argtypes = [wintypes.UINT, wintypes.UINT]
    handle = kernel.OpenProcess(0x100000 | 0x1000, False, launch['pid'])
    if not handle:
        raise ctypes.WinError(ctypes.get_last_error())
    try:
        name = ctypes.create_unicode_buffer(32768)
        length = wintypes.DWORD(len(name))
        if not kernel.QueryFullProcessImageNameW(handle, 0, name, ctypes.byref(length)):
            raise ctypes.WinError(ctypes.get_last_error())
        times = [wintypes.FILETIME() for _ in range(4)]
        if not kernel.GetProcessTimes(handle, *(ctypes.byref(t) for t in times)):
            raise ctypes.WinError(ctypes.get_last_error())
        started = times[0].dwLowDateTime | times[0].dwHighDateTime << 32
        if (os.path.normcase(str(Path(name.value).resolve())) != os.path.normcase(str(expected))
                or abs(started - filetime(launch['process_started'])) > 10
                or kernel.WaitForSingleObject(handle, 0) != 258):
            raise ValueError('Diagnostic process identity changed')
        windows = []

        @callback_type
        def visit(hwnd, unused):
            pid = wintypes.DWORD()
            user.GetWindowThreadProcessId(hwnd, ctypes.byref(pid))
            cls = ctypes.create_unicode_buffer(128)
            user.GetClassNameW(hwnd, cls, len(cls))
            if pid.value == launch['pid'] and user.IsWindowVisible(hwnd) and cls.value.lower().startswith('sdl'):
                windows.append(hwnd)
            return True

        user.EnumWindows(visit, 0)
        if len(windows) != 1:
            raise ValueError('Expected exactly one diagnostic SDL window')
        hwnd = windows[0]
        scan = user.MapVirtualKeyW(0x77, 0)  # VK_F8
        down = 1 | scan << 16
        if not user.PostMessageW(hwnd, 0x100, 0x77, down):
            raise ctypes.WinError(ctypes.get_last_error())
        try:
            time.sleep(0.15)
        finally:
            pid = wintypes.DWORD()
            user.GetWindowThreadProcessId(hwnd, ctypes.byref(pid))
            if pid.value == launch['pid']:
                user.PostMessageW(hwnd, 0x101, 0x77, down | 0xC0000000)
    finally:
        kernel.CloseHandle(handle)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--session', required=True)
    args = parser.parse_args()
    trigger(args.session)
