# SPDX-License-Identifier: GPL-2.0-or-later
[CmdletBinding()]
param([Parameter(Mandatory)][string]$Session,
      [ValidateRange(30,600)][int]$WarmupSeconds=90,
      [ValidateRange(20,300)][int]$MeasureSeconds=60,
      [string]$SceneKey='r15-fixed-save-crossroad-v1',
      [ValidateSet('automatic','manual')][string]$SceneGate='automatic',
      [string]$ReferenceImage='',
      [ValidateRange(0.01,0.5)][double]$MaximumImageDistance=.15,
      [switch]$EvidenceOnly)
Set-StrictMode -Version Latest
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot 'tools.ps1')
. (Join-Path $PSScriptRoot 'diagnostic_tools.ps1')
. (Join-Path $PSScriptRoot 'ordinary_scene.ps1')
. (Join-Path $PSScriptRoot 'automatic_scene.ps1')
$taskRoot=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$taskRecord=Read-ProjectSession $taskRoot $Session
$taskLaunch=$taskRecord.Launch
if($taskLaunch.complete -or -not $taskLaunch.audio_muted -or $taskLaunch.diagnostic -or
   (-not $EvidenceOnly -and $taskLaunch.optimization_v3 -notin @('control','all','custom')) -or
   ($EvidenceOnly -and $taskLaunch.timing_profile -ne 'auxiliary')){
    throw 'Observer requires a running, muted, isolated ordinary V3 session.'
}
foreach($key in @('gpu_profile','perf_log','profile_stacks')+$(if(-not $EvidenceOnly){@('path_trace')}else{@()})){
    $taskPolicy=if($EvidenceOnly){$taskLaunch.timing_overrides}else{$taskLaunch.v3_overrides}
    if([int]$taskPolicy.$key -ne 0){throw ('Heavy instrumentation must be disabled: '+$key)}
}
$taskTarget=Get-DiagnosticTarget $taskLaunch $taskRoot
if(-not $taskTarget){throw 'The pinned game process is not running.'}
$taskNvidia=Find-DiagnosticExecutable 'nvidia-smi.exe' @((Join-Path $env:WINDIR 'System32/nvidia-smi.exe'))
if(-not $taskNvidia){throw 'The existing NVIDIA sensor tool is unavailable; nothing is installed.'}
$taskDirectory=Resolve-ProjectChild $taskRoot ($Session+'/ordinary-observer')
if(Test-Path -LiteralPath $taskDirectory){throw 'Observer output already exists; do not overwrite a previous observation.'}
$null=[IO.Directory]::CreateDirectory($taskDirectory)
$taskFields=@('timestamp','index','name','utilization.gpu','temperature.gpu','power.draw','clocks.sm','clocks.mem','pstate','clocks_event_reasons.active')
$taskSensor=New-DiagnosticSensors $taskNvidia $taskDirectory $taskFields
$taskWriter=[IO.StreamWriter]::new((Join-Path $taskDirectory 'cpu.csv'),$false,[Text.UTF8Encoding]::new($false))
$taskWriter.AutoFlush=$true
$taskWriter.WriteLine('utc,qpc_begin,qpc_end,game_cpu_seconds,observer_cpu_seconds,other_accessible_cpu_seconds,processes_compared,processes_unreadable,top_other_processes')
$taskPrevious=@{};$taskPreviousQpc=0L;$taskLastCpu=-2.0
$taskClock=[Diagnostics.Stopwatch]::StartNew()
$taskDeadline=[DateTime]::UtcNow.AddSeconds($WarmupSeconds+$MeasureSeconds+300)
$taskMetadata=[ordered]@{
    schema=1;observer_revision=3;kind='ordinary-low-rate-observation';session=$Session;runtime_sha256=$taskLaunch.runtime_sha256
    started_utc=[DateTime]::UtcNow.ToString('o');observer_pid=$PID;qpc_frequency=[Diagnostics.Stopwatch]::Frequency
    warmup_seconds=$WarmupSeconds;measure_seconds=$MeasureSeconds;stop_after_elapsed_seconds=($WarmupSeconds+$MeasureSeconds+3)
    scene_key=$SceneKey;scene_confirmed=$false;scene_receipt=$null;window_start_seconds=$null;window_end_seconds=$null
    gpu_fields=$taskFields;game_input='none';gpu_timestamp_queries=$false;audio_muted=$true
    cpu_scope='Process CPU deltas for accessible surviving PIDs; excludes unreadable/new/exited processes. Not total system CPU.'
    complete=$false;quit_requested=$false;failure=$null;finished_utc=$null;performance_acceptance=$false
}
try{
    Write-JsonUtf8 (Join-Path $taskDirectory 'observation.json') $taskMetadata
    $taskReceipt=if($SceneGate -eq 'automatic'){
        Wait-AutomaticOrdinaryScene $taskRoot $taskRecord $taskTarget $SceneKey $ReferenceImage $MaximumImageDistance
    }else{Wait-OrdinaryScene $taskRoot $taskRecord $taskTarget $SceneKey}
    $taskMetadata.scene_confirmed=$true;$taskMetadata.scene_receipt=$taskReceipt
    $taskMetadata.window_start_seconds=[Math]::Ceiling((Get-OrdinaryElapsed $taskRecord.Path)+$WarmupSeconds)
    $taskMetadata.window_end_seconds=$taskMetadata.window_start_seconds+$MeasureSeconds
    $taskMetadata.stop_after_elapsed_seconds=$taskMetadata.window_end_seconds+3
    Write-JsonUtf8 (Join-Path $taskDirectory 'observation.json') $taskMetadata
    Write-Host ('Scene verified; ordinary window '+$taskMetadata.window_start_seconds+'-'+$taskMetadata.window_end_seconds+' s.')
    while($true){
        if(Test-Path -LiteralPath (Join-Path $taskRoot 'state/runtime/v3-suite.stop')){throw 'Local stop request received.'}
        if([IO.DriveInfo]::new([IO.Path]::GetPathRoot($taskRoot)).AvailableFreeSpace -lt 3GB){throw 'Low free space; stopping this owned test session.'}
        if($taskTarget.HasExited){throw 'Game exited before the observation window completed.'}
        Update-DiagnosticSensors $taskSensor
        if($taskClock.Elapsed.TotalSeconds-$taskLastCpu -ge 1){
            $taskLastCpu=$taskClock.Elapsed.TotalSeconds
            $taskNow=[Diagnostics.Stopwatch]::GetTimestamp();$taskCurrent=@{};$taskUnreadable=0
            foreach($taskProcess in @(Get-Process)){
                try{
                    if($taskProcess.Id -eq 0){continue} # Idle time is not background CPU work.
                    $taskCurrent[[int]$taskProcess.Id]=@{name=$taskProcess.ProcessName;cpu=$taskProcess.TotalProcessorTime.TotalSeconds}
                }catch{$taskUnreadable++}
                finally{$taskProcess.Dispose()}
            }
            $taskGame=0.0;$taskOwn=0.0;$taskOther=0.0;$taskCompared=0;$taskTop=@()
            foreach($taskId in $taskCurrent.Keys){
                if(-not $taskPrevious.ContainsKey($taskId) -or $taskPrevious[$taskId].name -ne $taskCurrent[$taskId].name){continue}
                $taskDelta=$taskCurrent[$taskId].cpu-$taskPrevious[$taskId].cpu
                if($taskDelta -lt 0){continue}
                $taskCompared++
                if($taskId -eq $taskTarget.Id){$taskGame=$taskDelta}
                elseif($taskId -eq $PID){$taskOwn=$taskDelta}
                else{
                    $taskOther+=$taskDelta
                    if($taskDelta -gt 0){$taskTop+=@{name=$taskCurrent[$taskId].name;pid=$taskId;cpu_seconds=$taskDelta}}
                }
            }
            if($taskPreviousQpc){
                $taskRow=[pscustomobject][ordered]@{
                    utc=[DateTime]::UtcNow.ToString('o');qpc_begin=$taskPreviousQpc;qpc_end=$taskNow
                    game_cpu_seconds=$taskGame;observer_cpu_seconds=$taskOwn;other_accessible_cpu_seconds=$taskOther
                    processes_compared=$taskCompared;processes_unreadable=$taskUnreadable
                    top_other_processes=(@($taskTop|Sort-Object { $_.cpu_seconds } -Descending|Select-Object -First 3)|ConvertTo-Json -Compress -Depth 3)
                }
                $taskWriter.WriteLine(($taskRow|ConvertTo-Csv -NoTypeInformation)[1])
            }
            $taskPrevious=$taskCurrent;$taskPreviousQpc=$taskNow
        }
        $taskElapsed=Get-OrdinaryElapsed $taskRecord.Path
        if($taskElapsed -ge $taskMetadata.stop_after_elapsed_seconds){break}
        if([DateTime]::UtcNow -gt $taskDeadline){throw 'First flip or observation deadline exceeded.'}
        Start-Sleep -Milliseconds 250
    }
    $taskMetadata.complete=$true
}catch{
    $taskMetadata.failure=$_.Exception.Message
    throw
}finally{
    try{
        Stop-DiagnosticSensors $taskSensor
    }finally{
        $taskWriter.Dispose()
        try{
            if(-not $taskTarget.HasExited){
                try{
                    if($taskMetadata.complete){
                        & (Join-Path $PSScriptRoot 'control.ps1') -Action screenshot -Session $Session
                        Start-Sleep -Seconds 2
                    }
                }finally{
                    & (Join-Path $PSScriptRoot 'control.ps1') -Action quit -Session $Session
                    $taskMetadata.quit_requested=$true
                }
            }
        }finally{
            $taskTarget.Dispose()
            $taskMetadata.finished_utc=[DateTime]::UtcNow.ToString('o')
            Write-JsonUtf8 (Join-Path $taskDirectory 'observation.json') $taskMetadata
        }
    }
}
Write-Host ('Ordinary observation completed; scene-relative window '+$taskMetadata.window_start_seconds+'-'+$taskMetadata.window_end_seconds+'s; normal quit requested.')
