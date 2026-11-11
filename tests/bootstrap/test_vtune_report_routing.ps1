# SPDX-License-Identifier: GPL-2.0-or-later
# Exercise production routing only; never start a profiler or game.
Set-StrictMode -Version Latest
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot '../../scripts/tools.ps1')
. (Join-Path $PSScriptRoot '../../scripts/diagnostic_tools.ps1')
$original=@('-report','hotspots','-report-output','D:\report path\result.csv','-result-dir','D:\existing result')
$plan=Resolve-DiagnosticNativeInvocation 'C:\installed\vtune.exe' $original 'D:\report path\export.log'
if(-not $plan.Guarded -or [IO.Path]::GetFileName($plan.Executable) -ne 'ifss_vtune_report_guard.exe'){
    throw 'Offline report did not use the compatibility helper.'
}
if($plan.Arguments[0] -ne '--xml-rng-lifetime' -or $plan.Arguments[2] -ne 'C:\installed\vtune.exe' -or
   ($plan.Arguments[3..($plan.Arguments.Count-1)] -join "`0") -cne ($original -join "`0")){
    throw 'The report command or its paths changed during routing.'
}
foreach($case in @(
    @{exe='C:\installed\vtune.exe';args=@('-collect','memory-access','-knob','analyze-mem-objects=true')},
    @{exe='C:\installed\vtune.exe';args=@('-finalize','-finalization-mode','full')},
    @{exe='C:\installed\vtune.exe';args=@('-command','stop')},
    @{exe='C:\installed\other.exe';args=$original}
)){
    $unchanged=Resolve-DiagnosticNativeInvocation $case.exe $case.args 'D:\unused.log'
    if($unchanged.Guarded -or $unchanged.Executable -cne $case.exe -or
       ($unchanged.Arguments -join "`0") -cne ($case.args -join "`0")){
        throw 'Collection, finalization, control or an unrelated tool was changed.'
    }
}
$root=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
$project=Read-JsonUtf8 (Join-Path $root 'project.json')
$helper=Join-Path (Split-Path -Parent (Resolve-ProjectChild $root $project.runtime_path)) 'ifss_vtune_report_guard.exe'
foreach($invalid in @(
    @('--xml-rng-lifetime','unused.dmp','not-vtune.exe','-report','hotspots'),
    @('--xml-rng-lifetime','unused.dmp','vtune.exe','-collect','hotspots'),
    @('--xml-rng-lifetime','unused.dmp','vtune.exe','-report','hotspots','-collect','hotspots')
)){
    $start=[Diagnostics.ProcessStartInfo]::new($helper)
    $start.Arguments=($invalid|ForEach-Object{Quote-WindowsArgument $_}) -join ' '
    $start.UseShellExecute=$false;$start.CreateNoWindow=$true
    $start.RedirectStandardOutput=$true;$start.RedirectStandardError=$true
    $process=[Diagnostics.Process]::Start($start)
    try{
        $output=$process.StandardOutput.ReadToEndAsync();$errors=$process.StandardError.ReadToEndAsync()
        if(-not $process.WaitForExit(10000)){$process.Kill();throw 'Native refusal exceeded its bound.'}
        if($process.ExitCode -ne 2){throw 'The native guard did not refuse a non-report command before process creation.'}
    }finally{$process.Dispose()}
}
Write-Host 'VTune routing passed: exact report arguments, collection/finalization/control unchanged, native non-report refusal.'
