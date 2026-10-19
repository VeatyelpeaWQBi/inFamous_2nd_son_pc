﻿// SPDX-License-Identifier: GPL-2.0-or-later
using System;
using System.Collections.Generic;
using System.ComponentModel;
using System.Globalization;
using System.IO;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading;

namespace Ifss.Native {
    public static class WindowBranding {
        [DllImport("kernel32.dll",SetLastError=true)] static extern IntPtr OpenProcess(uint access, bool inherit, uint pid);
        [DllImport("kernel32.dll")] static extern bool CloseHandle(IntPtr handle);
        [DllImport("kernel32.dll")] static extern uint WaitForSingleObject(IntPtr handle, uint milliseconds);
        [DllImport("kernel32.dll",CharSet=CharSet.Unicode,SetLastError=true)] static extern bool QueryFullProcessImageName(IntPtr handle,uint flags,StringBuilder path,ref uint length);
        [DllImport("kernel32.dll",SetLastError=true)] static extern bool GetProcessTimes(IntPtr handle,out long created,out long exited,out long kernel,out long user);
        delegate bool EnumWindow(IntPtr hwnd,IntPtr parameter);
        [DllImport("user32.dll")] static extern bool EnumWindows(EnumWindow callback,IntPtr parameter);
        [DllImport("user32.dll")] static extern uint GetWindowThreadProcessId(IntPtr hwnd,out uint pid);
        [DllImport("user32.dll")] static extern bool IsWindowVisible(IntPtr hwnd);
        [DllImport("user32.dll")] static extern IntPtr GetWindow(IntPtr hwnd,uint command);
        [DllImport("user32.dll",CharSet=CharSet.Unicode)] static extern int GetClassName(IntPtr hwnd,StringBuilder name,int size);
        [DllImport("user32.dll",CharSet=CharSet.Unicode)] static extern int GetWindowText(IntPtr hwnd,StringBuilder name,int size);
        [DllImport("user32.dll",CharSet=CharSet.Unicode)] static extern IntPtr SendMessageTimeout(IntPtr hwnd,uint message,IntPtr wparam,IntPtr lparam,uint flags,uint timeout,out IntPtr result);
        [DllImport("user32.dll",CharSet=CharSet.Unicode)] static extern IntPtr LoadImage(IntPtr instance,string name,uint type,int width,int height,uint flags);
        [DllImport("user32.dll")] static extern bool DestroyIcon(IntPtr icon);
        [DllImport("user32.dll")] static extern bool ShowWindowAsync(IntPtr hwnd,int command);
        static bool Send(IntPtr hwnd,uint message,IntPtr wparam,IntPtr lparam) {
            IntPtr result; return SendMessageTimeout(hwnd,message,wparam,lparam,3,100,out result) != IntPtr.Zero;
        }
        public static void Run(ProjectPaths paths,string relative) {
            var session = paths.Child(relative);
            if (!ProjectPaths.Inside(paths.Child("state/sessions"),session)) throw new InvalidDataException("Session belongs to another project.");
            if (IntPtr.Size != 8) throw new InvalidOperationException("Window helper requires the existing Windows x64 PowerShell.");
            var launch = Json.Read(paths.Child(relative + "/launch.json")); var manifest = Json.Read(Path.Combine(paths.Root,"project.json"));
            var expected = paths.Resolve(Json.Text(manifest,"runtime_path",""));
            if (!String.Equals(expected,paths.Resolve(Json.Text(launch,"runtime","")),StringComparison.OrdinalIgnoreCase) ||
                Json.Text(launch,"runtime_sha256","") != Json.Text(manifest,"runtime_sha256",""))
                throw new InvalidDataException("Window helper runtime differs from the pinned engine.");
            uint pid = checked((uint)Json.Integer(launch["pid"]));
            IntPtr process = OpenProcess(0x100000 | 0x1000,false,pid);
            if (process == IntPtr.Zero) throw new Win32Exception(Marshal.GetLastWin32Error());
            var icons = new Dictionary<int,IntPtr>(); IntPtr title = IntPtr.Zero;
            try {
                var name = new StringBuilder(32768); uint length = (uint)name.Capacity;
                if (!QueryFullProcessImageName(process,0,name,ref length)) throw new Win32Exception(Marshal.GetLastWin32Error());
                long created,exited,kernel,user;
                if (!GetProcessTimes(process,out created,out exited,out kernel,out user)) throw new Win32Exception(Marshal.GetLastWin32Error());
                var start = DateTime.Parse(Json.Text(launch,"process_started",""),CultureInfo.InvariantCulture,DateTimeStyles.RoundtripKind).ToUniversalTime().ToFileTimeUtc();
                if (!String.Equals(Path.GetFullPath(name.ToString()),expected,StringComparison.OrdinalIgnoreCase) || created != start)
                    throw new InvalidDataException("Window helper rejected a reused PID or another executable.");
                var iconPath = Path.Combine(paths.Root,GameIdentity.Name + ".ico");
                if (File.Exists(iconPath)) foreach (int size in new [] {32,16}) {
                    var icon = LoadImage(IntPtr.Zero,iconPath,1,size,size,0x10); if (icon != IntPtr.Zero) icons[size] = icon;
                }
                title = Marshal.StringToHGlobalUni(GameIdentity.Name);
                var applied = new HashSet<IntPtr>(); var sized = new HashSet<IntPtr>();
                var settings = Json.Get(launch,"launcher_settings",null) as Dictionary<string,object>;
                bool windowed = settings != null && Json.Text(settings,"display_mode","") == "windowed";
                EnumWindow visit = delegate(IntPtr hwnd,IntPtr unused) {
                    uint actual; GetWindowThreadProcessId(hwnd,out actual);
                    if (actual != pid || !IsWindowVisible(hwnd) || GetWindow(hwnd,4) != IntPtr.Zero) return true;
                    var windowClass = new StringBuilder(128); GetClassName(hwnd,windowClass,windowClass.Capacity);
                    if (!windowClass.ToString().StartsWith("SDL",StringComparison.OrdinalIgnoreCase)) return true;
                    var current = new StringBuilder(512); GetWindowText(hwnd,current,current.Capacity);
                    if (current.ToString() != GameIdentity.Name && !Send(hwnd,0x0c,IntPtr.Zero,title)) return true;
                    if (windowed && !sized.Contains(hwnd) && ShowWindowAsync(hwnd,3)) sized.Add(hwnd);
                    if (!applied.Contains(hwnd)) {
                        bool success = icons.Count != 0;
                        foreach (var icon in icons) success = Send(hwnd,0x80,new IntPtr(icon.Key == 32 ? 1 : 0),icon.Value) && success;
                        if (success) applied.Add(hwnd);
                    }
                    return true;
                };
                for (;;) {
                    uint status = WaitForSingleObject(process,0);
                    if (status == 0) break;
                    if (status != 258) throw new Win32Exception(Marshal.GetLastWin32Error());
                    EnumWindows(visit,IntPtr.Zero); Thread.Sleep(500);
                }
                GC.KeepAlive(visit);
            } finally {
                if (title != IntPtr.Zero) Marshal.FreeHGlobal(title);
                foreach (var icon in icons.Values) DestroyIcon(icon);
                CloseHandle(process);
            }
        }
    }
}
