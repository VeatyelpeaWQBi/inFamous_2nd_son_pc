# SPDX-License-Identifier: GPL-2.0-or-later
# Verify total accounting, disappearing cache files, and low-free-space protection.
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot '../../scripts/tools.ps1')
. (Join-Path $PSScriptRoot '../../scripts/diagnostic_tools.ps1')
$projectRoot=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
$testRoot=Resolve-ProjectChild $projectRoot ('state/offline-tests/'+[Guid]::NewGuid().ToString('N'))
$rotating=Join-Path $testRoot 'rotating-cache'
$logs=Join-Path $testRoot 'log'
foreach($path in @($rotating,$logs,(Join-Path $testRoot 'diagnostics/temp'),(Join-Path $testRoot 'home'))){
    $null=[IO.Directory]::CreateDirectory($path)
}
[IO.File]::WriteAllBytes((Join-Path $logs 'engine.log'),[byte[]](1,2,3))
[IO.File]::WriteAllBytes((Join-Path $testRoot 'resource.bin'),[byte[]](1,2,3,4,5))
[IO.File]::WriteAllBytes((Join-Path $testRoot 'diagnostics/temp/trace.etl'),[byte[]](1,2,3,4,5,6,7))
[IO.File]::WriteAllBytes((Join-Path $testRoot 'home/save.bin'),[byte[]](1,2,3,4,5,6,7,8,9))
$script:usageRacePath=$rotating
$script:denyUsageScan=$false
$originalEntries=(Get-Command Get-DiagnosticDirectoryEntries).ScriptBlock
$child=$null;$monitor=$null
try{
    function Get-DiagnosticDirectoryEntries([string]$Directory) {
        if($script:denyUsageScan){throw [UnauthorizedAccessException]::new('Injected access rejection.')}
        if($Directory -eq $script:usageRacePath -and [IO.Directory]::Exists($Directory)){
            [IO.Directory]::Delete($Directory)
        }
        if($Directory -eq $logs){
            # Return a real entry that disappears before its attributes/length are read.
            $vanishing=Join-Path $Directory 'rotating.tmp'
            [IO.File]::WriteAllBytes($vanishing,[byte[]](1,2))
            $names=[IO.Directory]::GetFileSystemEntries($Directory)
            [IO.File]::Delete($vanishing)
            return $names
        }
        return [IO.Directory]::EnumerateFileSystemEntries($Directory)
    }
    $usage=Get-DiagnosticUsage $testRoot
    if($usage.total_bytes -ne 24){throw 'Total usage must include logs, resources, WPR temporary files and save copies.'}
    $rootFailure=$false
    try{Get-DiagnosticUsage (Join-Path $testRoot 'missing-root')|Out-Null}catch{$rootFailure=$true}
    if(-not $rootFailure){throw 'A missing session root was silently accepted.'}
    $script:denyUsageScan=$true
    $accessFailure=$false
    try{Get-DiagnosticUsage $testRoot|Out-Null}catch [UnauthorizedAccessException]{$accessFailure=$true}
    if(-not $accessFailure){throw 'An access failure was silently accepted.'}
    $script:denyUsageScan=$false

    $policy=Get-DiagnosticStoragePolicy
    foreach($total in @(1GB,20GB,100GB)){
        $sample=New-DiagnosticStorageSample $total 120GB 3GB $policy.minimum_free_bytes
        if($sample.low_space -or $sample.peak_total_bytes -ne 120GB){throw 'Session size must not trigger a stop or discard the observed peak.'}
    }
    if((New-DiagnosticStorageSample 0 0 2GB $policy.minimum_free_bytes).low_space){throw 'The free-space boundary is inclusive.'}
    if(-not (New-DiagnosticStorageSample 24 24 1GB $policy.minimum_free_bytes).low_space){throw 'Low free disk space must trigger protection even for a tiny session.'}

    # Exercise polling while an actual installed-tool child is awaited, without a game.
    $policy.sample_interval_seconds=0
    $textWriter=[IO.StringWriter]::new([Globalization.CultureInfo]::InvariantCulture)
    $monitor=[pscustomobject]@{
        directory=$testRoot;root=$projectRoot;session='offline-test';state=[ordered]@{errors=@();status='waiting'}
        status_path=(Join-Path $testRoot 'status.json');clock=[Diagnostics.Stopwatch]::StartNew()
        last_sample_s=-10.0;policy=$policy;peak_total_bytes=0L;low_space=$false;failed=$false
        volume=[pscustomobject]@{AvailableFreeSpace=5GB};writer=$textWriter
    }
    $start=[Diagnostics.ProcessStartInfo]::new()
    $start.FileName=Join-Path $env:WINDIR 'System32/WindowsPowerShell/v1.0/powershell.exe'
    $start.Arguments='-NoLogo -NoProfile -Command "Start-Sleep -Milliseconds 1100"'
    $start.UseShellExecute=$false;$start.CreateNoWindow=$true
    $script:DiagnosticStorageMonitor=$monitor
    $child=[Diagnostics.Process]::Start($start)
    $null=$child.Handle
    if(-not (Wait-DiagnosticHelper $child 10000)){throw 'Offline child did not finish.'}
    $rows=@($textWriter.ToString().Split([char]10)|Where-Object {$_})
    $status=Read-JsonUtf8 $monitor.status_path
    if($rows.Count -lt 2 -or $status.usage.total_bytes -lt 24 -or $status.usage.available_free_bytes -ne 5GB){
        throw 'Aggregate monitoring stopped while a collector helper was awaited.'
    }
    Write-Host 'Aggregate usage, helper-wait monitoring, cache rotation and low-disk protection checks passed.'
}finally{
    $script:DiagnosticStorageMonitor=$null
    if($child){if(-not $child.HasExited){$child.Kill();$child.WaitForExit()};$child.Dispose()}
    if($monitor){$monitor.writer.Dispose()}
    Set-Item Function:\Get-DiagnosticDirectoryEntries -Value $originalEntries
    Assert-ProjectPath $projectRoot $testRoot
    foreach($name in @('log/engine.log','resource.bin','diagnostics/temp/trace.etl','home/save.bin','status.json')){
        $path=Join-Path $testRoot $name
        if(Test-Path -LiteralPath $path){Remove-Item -LiteralPath $path -Force}
    }
    foreach($path in @($rotating,(Join-Path $testRoot 'diagnostics/temp'),(Join-Path $testRoot 'diagnostics'),(Join-Path $testRoot 'home'),$logs,$testRoot)){
        Assert-ProjectPath $projectRoot $path
        if([IO.Directory]::Exists($path)){[IO.Directory]::Delete($path)}
    }
}
