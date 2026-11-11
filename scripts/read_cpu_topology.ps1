# SPDX-License-Identifier: GPL-2.0-or-later
# Read-only Windows core topology. Uses the installed PowerShell/.NET and kernel32 API;
# never changes affinity, priorities, power settings, services, or launches the game.
[CmdletBinding()]
param()
Set-StrictMode -Version Latest
$ErrorActionPreference='Stop'
if(-not [Environment]::Is64BitProcess){throw 'Run with installed 64-bit Windows PowerShell.'}
if(-not ('IfssReadOnlyCpuTopology' -as [type])){
Add-Type -TypeDefinition @'
using System;
using System.Collections.Generic;
using System.ComponentModel;
using System.Runtime.InteropServices;
public static class IfssReadOnlyCpuTopology {
    [DllImport("kernel32.dll", SetLastError=true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    static extern bool GetLogicalProcessorInformationEx(int relationship, IntPtr buffer, ref uint bytes);
    public sealed class Core {
        public int EfficiencyClass;
        public bool Smt;
        public int Group;
        public ulong Mask;
    }
    public static Core[] Read() {
        uint bytes=0;
        bool first=GetLogicalProcessorInformationEx(0,IntPtr.Zero,ref bytes);
        if(first || Marshal.GetLastWin32Error()!=122 || bytes<48 || bytes>16*1024*1024)
            throw new InvalidOperationException("Cannot query core topology size.");
        IntPtr memory=Marshal.AllocHGlobal(checked((int)bytes));
        try {
            if(!GetLogicalProcessorInformationEx(0,memory,ref bytes))
                throw new Win32Exception(Marshal.GetLastWin32Error());
            var result=new List<Core>();
            int position=0;
            while(position<bytes) {
                if(position>bytes-8) throw new InvalidOperationException("Truncated topology header.");
                IntPtr item=IntPtr.Add(memory,position);
                int relation=Marshal.ReadInt32(item,0), size=Marshal.ReadInt32(item,4);
                if(relation!=0 || size<48 || size>bytes-position)
                    throw new InvalidOperationException("Invalid core topology record.");
                int count=(ushort)Marshal.ReadInt16(item,30);
                if(count!=1 || 32+count*16>size)
                    throw new InvalidOperationException("Unexpected processor-core groups.");
                result.Add(new Core {
                    EfficiencyClass=Marshal.ReadByte(item,9),
                    Smt=(Marshal.ReadByte(item,8)&1)!=0,
                    Mask=unchecked((ulong)Marshal.ReadInt64(item,32)),
                    Group=(ushort)Marshal.ReadInt16(item,40)
                });
                position+=size;
            }
            return result.ToArray();
        } finally { Marshal.FreeHGlobal(memory); }
    }
}
'@
}
$cores=@([IfssReadOnlyCpuTopology]::Read()|ForEach-Object {
    $entry=$_;$logical=@(for($bit=0;$bit -lt 64;$bit++){
        if(($entry.Mask -band ([UInt64]1 -shl $bit)) -ne 0){$bit}
    })
    [ordered]@{group=$entry.Group;efficiency_class=$entry.EfficiencyClass;smt=$entry.Smt
        mask_hex=('{0:x}' -f $entry.Mask);logical_processors=$logical}
})
[ordered]@{schema=1;observed_utc=[DateTime]::UtcNow.ToString('o');read_only=$true
    api='GetLogicalProcessorInformationEx(RelationProcessorCore)';cores=$cores}|ConvertTo-Json -Depth 6
