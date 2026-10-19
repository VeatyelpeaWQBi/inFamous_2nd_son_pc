using System;
using System.ComponentModel;
using System.Runtime.InteropServices;
using System.Text;
namespace Ifss {
    public sealed class ProcessSnapshot {
        public string Path;
        public long CreationFileTime;
    }
    public static class ProcessIdentity {
        [DllImport("kernel32.dll", CharSet=CharSet.Unicode, SetLastError=true)]
        private static extern bool QueryFullProcessImageName(IntPtr process, int flags, StringBuilder name, ref int length);
        [DllImport("kernel32.dll", SetLastError=true)]
        private static extern bool GetProcessTimes(IntPtr process, out long created, out long exited, out long kernel, out long user);
        [DllImport("kernel32.dll", SetLastError=true)]
        private static extern IntPtr OpenProcess(int access, bool inherit, int processId);
        [DllImport("kernel32.dll")]
        private static extern bool CloseHandle(IntPtr handle);
        public static ProcessSnapshot ReadLimited(int processId) {
            var handle=OpenProcess(0x1000, false, processId);
            if(handle==IntPtr.Zero) throw new Win32Exception(Marshal.GetLastWin32Error());
            try { return Read(handle); }
            finally { CloseHandle(handle); }
        }
        public static ProcessSnapshot Read(IntPtr process) {
            int length=32768;
            var name=new StringBuilder(length);
            if(!QueryFullProcessImageName(process, 0, name, ref length))
                throw new Win32Exception(Marshal.GetLastWin32Error());
            long created, exited, kernel, user;
            if(!GetProcessTimes(process, out created, out exited, out kernel, out user))
                throw new Win32Exception(Marshal.GetLastWin32Error());
            return new ProcessSnapshot { Path=name.ToString(), CreationFileTime=created };
        }
    }
}
