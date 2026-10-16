# SPDX-License-Identifier: GPL-2.0-or-later
# Offline protocol regression. Uses a fake process and an isolated, disposable session.
# No engine, input consumer, screenshots or external profiler is started.
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot '../../scripts/tools.ps1')
. (Join-Path $PSScriptRoot '../../scripts/diagnostic_automation.ps1')
$projectRoot=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
$manifest=Read-JsonUtf8 (Join-Path $projectRoot 'project.json')
$relative='state/sessions/offline-control-'+[Guid]::NewGuid().ToString('N')
$testRoot=Resolve-ProjectChild $projectRoot $relative
$null=[IO.Directory]::CreateDirectory($testRoot)
$mockControlProcess=[pscustomobject]@{
    Id=2147483647;Path=(Resolve-ProjectPath $projectRoot $manifest.runtime_path)
    StartTime=[DateTime]::Parse('2026-01-01T00:00:00Z').ToUniversalTime()
}
$launch=@{
    schema=3;path_base='project';session=$relative;runtime=$manifest.runtime_path
    runtime_sha256=$manifest.runtime_sha256;game_launched=$true;complete=$false
    pid=$mockControlProcess.Id;process_started=$mockControlProcess.StartTime.ToUniversalTime().ToString('o')
}
function Invoke-OfflineControl($Parameters) {
    # Local function shadowing applies to the called script; a real process lookup is unnecessary.
    function Get-Process {
        [CmdletBinding()]param([int]$Id)
        if($Id -ne $mockControlProcess.Id){throw 'Unexpected offline target PID.'}
        return $mockControlProcess
    }
    function Get-ProjectProcessIdentity($Process) {
        return [pscustomobject]@{Path=$Process.Path;CreationFileTime=$Process.StartTime.ToFileTimeUtc()}
    }
    & (Join-Path $projectRoot 'scripts/control.ps1') @Parameters -Session $relative 6>$null
}
try{
    Write-JsonUtf8 (Join-Path $testRoot 'launch.json') $launch
    $requests=@(
        @{Action='status'},@{Action='nsight-stop'},@{Action='vtune-detach'},@{Action='screenshot'},@{Action='button';Button='square';DurationMs=200},
        @{Action='axis';Axis=2;Value=128},@{Action='quit'}
    )
    $sequence=0
    foreach($parameters in $requests){
        Invoke-OfflineControl $parameters
        $command=Read-JsonUtf8 (Join-Path $testRoot 'command.json')
        $sequence++
        if($command.sequence -ne $sequence -or $command.action -ne $parameters.Action){throw 'Control command sequencing failed.'}
        if($parameters.Action -eq 'button' -and ($command.button -ne 'square' -or $command.duration_ms -ne 200)){throw 'Button payload was lost.'}
        if($parameters.Action -eq 'axis' -and ($command.axis -ne 2 -or $command.value -ne 128)){throw 'Axis payload was lost.'}
    }
    $queued=@(Get-ChildItem -LiteralPath (Join-Path $testRoot 'control-queue') -File|Sort-Object Name)
    if($queued.Count -ne $requests.Count){throw 'Fast producers overwrote an unapplied control command.'}
    for($i=0;$i -lt $queued.Count;$i++){
        $entry=Read-JsonUtf8 $queued[$i].FullName
        if($entry.sequence -ne $i+1 -or $entry.action -ne $requests[$i].Action){throw 'FIFO command payload/order was lost.'}
    }
    $mockControlProcess.StartTime=$mockControlProcess.StartTime.AddSeconds(1)
    $rejected=$false
    try{Invoke-OfflineControl @{Action='screenshot'}}catch{
        if($_.Exception.Message -notmatch 'process identity'){throw}
        $rejected=$true
    }
    if(-not $rejected){throw 'A reused PID must be rejected before writing any command.'}
    $command=Read-JsonUtf8 (Join-Path $testRoot 'command.json')
    if($command.sequence -ne $sequence){throw 'Identity rejection overwrote the previous command.'}
    $logDirectory=Join-Path $testRoot 'log'
    $null=[IO.Directory]::CreateDirectory($logDirectory)
    $log=Join-Path $logDirectory 'shad_log.txt'
    [IO.File]::WriteAllText($log,'Perf: draws/frame 800 dispatches/frame 200'+[Environment]::NewLine+('validation chatter'+[Environment]::NewLine)*150,[Text.UTF8Encoding]::new($false))
    if(-not (Get-DiagnosticSceneTelemetry $testRoot)){throw 'Single-file telemetry was not found behind validation chatter.'}
    [IO.File]::WriteAllText($log,'Perf: draws/frame 1 dispatches/frame 0',[Text.UTF8Encoding]::new($false))
    if(Get-DiagnosticSceneTelemetry $testRoot){throw 'Menu-level rendering must not qualify as scene telemetry.'}
    Write-Host 'Offline control protocol, process identity rejection and log discovery checks passed.'
}finally{
    foreach($name in @('launch.json','command.json','control.lock','log/shad_log.txt')){
        $path=Resolve-ProjectChild $projectRoot ($relative+'/'+$name)
        if(Test-Path -LiteralPath $path){Remove-Item -LiteralPath $path -Force}
    }
    $directory=Resolve-ProjectChild $projectRoot ($relative+'/log')
    if(Test-Path -LiteralPath $directory){[IO.Directory]::Delete($directory)}
    $queue=Resolve-ProjectChild $projectRoot ($relative+'/control-queue')
    foreach($entry in @(Get-ChildItem -LiteralPath $queue -File)){
        Assert-ProjectPath $projectRoot $entry.FullName
        Remove-Item -LiteralPath $entry.FullName
    }
    [IO.Directory]::Delete($queue)
    Assert-ProjectPath $projectRoot $testRoot
    [IO.Directory]::Delete($testRoot)
}
