# SPDX-License-Identifier: GPL-2.0-or-later
[CmdletBinding()]
param()
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot '../../scripts/tools.ps1')
$root=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
$fixture=Resolve-ProjectChild $root ('state/offline-tests/native entry '+[Guid]::NewGuid().ToString('N'))
$null=[IO.Directory]::CreateDirectory($fixture)
function Invoke-OfflineNativeEntry([string]$Module,[string]$Json){
    $info=New-Object Diagnostics.ProcessStartInfo
    $info.FileName=[IO.Path]::Combine([Environment]::GetFolderPath('System'),'WindowsPowerShell/v1.0/powershell.exe')
    $info.WorkingDirectory=$root
    $info.UseShellExecute=$false;$info.CreateNoWindow=$true
    $info.RedirectStandardOutput=$true;$info.RedirectStandardError=$true
    $entryFile=if($Module -in @('metrics','analyze-game','diagnostic-quality','analyze-timing')){'diagnostic-native.ps1'}else{'native.ps1'}
    $arguments=@('-NoLogo','-NoProfile','-ExecutionPolicy','Bypass','-File',(Join-Path $root ('scripts/'+$entryFile)),'-Module',$Module,'-ArgumentsJson',$Json)
    $info.Arguments=($arguments|ForEach-Object{Quote-WindowsArgument $_}) -join ' '
    $child=[Diagnostics.Process]::Start($info)
    try{
        $output=$child.StandardOutput.ReadToEndAsync();$errors=$child.StandardError.ReadToEndAsync()
        if(-not $child.WaitForExit(30000)){$child.Kill();$child.WaitForExit();throw 'Offline native entry timed out.'}
        return [pscustomobject]@{Code=$child.ExitCode;Output=$output.GetAwaiter().GetResult();Error=$errors.GetAwaiter().GetResult()}
    }finally{$child.Dispose()}
}
try{
    $csv=Join-Path $fixture "frames ' test.csv"
    [IO.File]::WriteAllText($csv,"frame,elapsed_ns,interval_ns`n0,0,0`n1,16000000,16000000`n2,32000000,16000000`n3,48000000,16000000`n",[Text.UTF8Encoding]::new($true))
    $arguments=@((Get-RelativePath $root $csv),'--start','0','--end','0.048','--mode','1080p')
    $entry=Invoke-OfflineNativeEntry 'metrics' (ConvertTo-Json -InputObject $arguments -Compress)
    if($entry.Code -ne 0 -or $entry.Error){throw ('Metrics entry failed: '+$entry.Error)}
    $report=$entry.Output|ConvertFrom-Json
    if($report.schema -ne 2 -or $report.frames -ne 3 -or $report.fps -ne 62.5 -or $report.baseline_acceptance -ne 'not-verified'){throw 'Metrics entry returned an invalid report.'}
    foreach($case in @(
        @{module='preflight';json='["--mode","720p","--frozen"]';code=1;message='explicit 1080p or pro4k'},
        @{module='launch-menu';json='["--unknown"]';code=2;message='Unknown argument'},
        @{module='metrics';json='[]';code=1;message='Missing frame CSV'},
        @{module='metrics';json='"scalar-is-not-an-array"';code=1;message='JSON array of strings'},
        @{module='metrics';json='[["nested"]]';code=1;message='JSON array of strings'},
        @{module='metrics';json='[null]';code=1;message='JSON array of strings'}
    )){
        $entry=Invoke-OfflineNativeEntry $case.module $case.json
        if($entry.Code -ne $case.code -or $entry.Error -notmatch $case.message -or $entry.Output){throw ('Native entry rejection failed for '+$case.module+': '+$entry.Error)}
    }
    Write-Host 'Native entry regressions passed: metrics JSON, quoted paths, array validation and exit codes. No UI or game was started.'
}finally{
    Assert-ProjectPath $root $fixture
    foreach($file in Get-ChildItem -LiteralPath $fixture -File -Force){Assert-ProjectPath $root $file.FullName;Remove-Item -LiteralPath $file.FullName -Force}
    [IO.Directory]::Delete($fixture)
}
