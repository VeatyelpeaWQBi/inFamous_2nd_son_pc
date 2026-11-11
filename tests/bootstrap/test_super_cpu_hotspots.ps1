# SPDX-License-Identifier: GPL-2.0-or-later
# Offline replay of the actual exporter. No target, profiler, game or input is started.
Set-StrictMode -Version Latest
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot '../../scripts/tools.ps1')
. (Join-Path $PSScriptRoot '../../scripts/diagnostic_artifacts.ps1')
. (Join-Path $PSScriptRoot '../../scripts/diagnostic_tools.ps1')
. (Join-Path $PSScriptRoot '../../scripts/super_cpu_hotspots.ps1')
$projectRoot=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
$fixture=Resolve-ProjectChild $projectRoot ('state/test-super-cpu-'+[Guid]::NewGuid().ToString('N'))
$null=[IO.Directory]::CreateDirectory($fixture)
$script:requests=@();$script:wrongPid=$false;$script:wrongThread=$false
function Invoke-DiagnosticNative($Executable,$Arguments,$Log,$TimeoutMilliseconds){
    $script:requests+=,@($Arguments)
    $index=[array]::IndexOf($Arguments,'-report-output')
    if($index -lt 0){return}
    $file=$Arguments[$index+1]
    if($Arguments -contains 'summary'){
        [IO.File]::WriteAllText($file,"Metric Name,Metric Value`nCollection start time,12:00:00 02/11/2026 UTC`n")
    }else{
        $testPid=if($script:wrongPid){88}else{77}
        $testThread=if($script:wrongThread){'OtherWorker'}else{'shadPS4:GpuCommandProcessor'}
        [IO.File]::WriteAllText($file,"Process,Thread,Function,CPU Time,PID`nGame,$testThread,KnownFunction,2.5,$testPid`nOther,shadPS4:GpuCommandProcessor,OtherFunction,99,88`n")
    }
}
function New-Owner {
    return [pscustomobject]@{Directory=$fixture;Tools=@{vtune='not-started'};Exported=$false
        Process=[pscustomobject]@{HasExited=$true}
        State=[ordered]@{status='detached';vtune_exit_code=0;target_pid=77;errors=@();phases=@()}}
}
$release=[DateTimeOffset]::Parse('2026-11-02T12:00:12Z').ToUnixTimeMilliseconds()*1000000L
$cycle=[pscustomobject]@{release=[pscustomobject]@{applied_unix_ns=$release};warm=$true;meter_consumed=$true}
try{
    $owner=New-Owner
    Complete-SuperCpuHotspots $owner $cycle
    if(-not $owner.Exported -or $owner.State.status -ne 'collected' -or $owner.State.phases.Count -ne 3){throw 'Exporter did not complete the three intended CPU phases.'}
    foreach($phase in $owner.State.phases){
        if($phase.target_rows -ne 1 -or $phase.cp_cpu_seconds -ne 2.5){throw 'Exporter admitted another PID or failed to resolve the CP samples.'}
    }
    $reports=@($script:requests|Where-Object {$_ -contains '-time-filter'})
    if($reports.Count -ne 3){throw 'Unexpected CPU report count.'}
    $filters=@($reports|ForEach-Object {$_[[array]::IndexOf($_,'-time-filter')+1]})
    if(($filters -join ',') -ne '5.000000:10.000000,13.000000:20.000000,22.000000:29.000000'){throw ('Unexpected conservative phase filters: '+($filters -join ','))}
    $script:wrongPid=$true
    try{Complete-SuperCpuHotspots (New-Owner) $cycle;throw 'Wrong PID was accepted.'}catch{if($_.Exception.Message -notlike 'No explicitly matching target PID*'){throw}}
    $script:wrongPid=$false;$script:wrongThread=$true
    try{Complete-SuperCpuHotspots (New-Owner) $cycle;throw 'Missing CP name was accepted.'}catch{if($_.Exception.Message -notlike 'Command processor thread was not resolved*'){throw}}
    # Replay the real attachment boundary, checking required directories before the
    # mocked collector is invoked. No profiler/game process is created.
    function Test-DiagnosticAdministrator {return $true}
    function Read-ProjectSession {return [pscustomobject]@{Path=$fixture;Launch=[pscustomobject]@{
        super_capture=$true;super_capture_batch='state/super-captures/test';cpu_hotspots=$true
        runtime_sha256=('a'*64);process_started='2026-11-02T12:00:00Z'}}}
    function Get-DiagnosticTarget {
        return ([pscustomobject]@{Id=77;HasExited=$false}|Add-Member ScriptMethod Dispose {} -PassThru)
    }
    function Get-DiagnosticTools {return @{vtune=(Join-Path $env:WINDIR 'System32/WindowsPowerShell/v1.0/powershell.exe')}}
    function Quote-WindowsArgument([string]$Value){return '"'+$Value+'"'}
    $script:expectedMode='hw'
    function Start-DiagnosticProcess($Options){
        if(-not(Test-Path -LiteralPath (Join-Path $fixture 'diagnostics/cpu-hotspots/vtune-user') -PathType Container)){
            throw 'VTune user directory was not created before attachment.'
        }
        if($Options.ArgumentList -notlike ('*sampling-mode='+$script:expectedMode+'*') -or $Options.ArgumentList -notlike '*-target-pid*77*'){
            throw 'Unexpected CPU attachment configuration.'
        }
        if($script:expectedMode -eq 'hw' -and $Options.ArgumentList -notlike '*sampling-interval=5*'){throw 'Hardware interval changed.'}
        if($script:expectedMode -eq 'sw' -and ($Options.ArgumentList -like '*sampling-interval*' -or $Options.ArgumentList -like '*enable-stack-collection*' -or $Options.ArgumentList -notlike '*-data-limit*64*')){throw 'Software mode received hardware knobs or lacks its raw data bound.'}
        [IO.File]::WriteAllText($Options.RedirectStandardError,'Collection started.')
        return [pscustomobject]@{Id=90;HasExited=$false}
    }
    function Read-DiagnosticSharedText($Path){return [IO.File]::ReadAllText($Path)}
    $attached=$null
    Start-SuperCpuHotspots $projectRoot 'synthetic' ([ref]$attached)
    if($attached.State.status -ne 'sampling' -or -not $attached.State.sampling_ack_utc){throw 'Actual collection acknowledgement was not required.'}
    function Test-DiagnosticAdministrator {return $false}
    try{Start-SuperCpuHotspots $projectRoot 'synthetic' ([ref]$attached);throw 'Hardware mode bypassed elevation.'}catch{if($_.Exception.Message -notlike 'CPU hardware sampling requires*'){throw}}
    $script:expectedMode='sw';$software=$null
    Start-SuperCpuHotspots $projectRoot 'synthetic' ([ref]$software) 'sw'
    if($software.State.hardware_metrics -or $software.State.sampling_mode -ne 'sw' -or $software.State.sampling_interval_ms -ne 10){throw 'Standalone software diagnostic was mislabeled as hardware.'}
    Write-Host 'Super CPU replay passed: attachment directories/arguments/receipt, phase bounds, target PID, CP identity, failure preservation.'
}finally{
    # Only this explicitly checked synthetic fixture is removed; captures are never touched.
    $resolved=[IO.Path]::GetFullPath($fixture)
    if($resolved.StartsWith((Join-Path $projectRoot 'state/test-super-cpu-'),[StringComparison]::OrdinalIgnoreCase)){
        Remove-Item -LiteralPath $resolved -Recurse -Force
    }
}
