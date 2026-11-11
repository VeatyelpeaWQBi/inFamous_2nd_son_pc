# SPDX-License-Identifier: GPL-2.0-or-later
[CmdletBinding()]
param(
    [ValidateSet('1080p','1440p','pro4k')][string]$Mode='pro4k',
    [ValidateSet('suite','lightweight','counters','compute','graphics')][string]$Profile='suite',
    [ValidateRange(1,20)][int]$Rounds=2,
    [ValidateRange(0,20)][int]$MinimumWarmup=0,
    [ValidateRange(0,40)][int]$MaximumWarmup=0,
    [ValidateRange(5,30)][int]$BaselineSeconds=10,
    [ValidateRange(25,90)][int]$ReleaseSeconds=30,
    [switch]$FullCapture,
    [switch]$CriticalCapture,
    [switch]$ReadbackBarrierAudit,
    [switch]$PageProtectAudit,
    [switch]$ComputeScopeAudit,
    [switch]$ReadbackWaitScopeAudit,
    [switch]$ComputeProtectAudit,
    [switch]$DrawPrepAudit,
    [switch]$ObserveEnvironment,
    [ValidateRange(0,30)][int]$EnvironmentProbeSeconds=0,
    [switch]$CpuHotspots,
    [ValidateSet('Off','Train','Use')][string]$RendererPgoTrial='Off',
    [ValidateSet('hw','sw')][string]$CpuHotspotsSamplingMode='hw',
    [ValidatePattern('^(?:[0-9a-fA-F]{1,10})?$')][string]$FeedbackTraceAddress='',
    [ValidatePattern('^(?:[0-9a-fA-F]{1,10})?$')][string]$SecondFeedbackTraceAddress='',
    [switch]$ProbeCandidate,
    [ValidateSet('none','feedback-checkpoint','feedback-writers','feedback-word','feedback-async','gds-counter','cpu-placement','compute-recorder','recorder-spin','reg-dump-fast','feedback-word-prefetch','feedback-word-bulk','particle-poll-pause','c73-scan','written-sync','image-sync-once','feedback-page-words','access-union','readback-copy-stage','image-read-transition','buffer-prep-union','feedback-arena','vertex-input-key','feedback-arena-group','feedback-arena-pool','feedback-arena-tiled','early-complete','writer-footprint')][string]$Candidate='none',
    [switch]$CandidateBaseline,
    [switch]$NormalRendering,
    [ValidatePattern('^(?:[0-9a-fA-F]{1,16})?$')][string]$PerformanceCoreMask='',
    [ValidatePattern('^(?:[0-9a-fA-F]{1,16})?$')][string]$ComputeTimingHash='',
    [ValidateRange(0,20)][int]$ComputeTimingStartSeconds=0,
    [ValidatePattern('^(?:[0-9a-fA-F]{1,16})?$')][string]$TraceShaderHash='',
    [ValidateSet('hardware-memory')][string]$VtuneCpuMode='hardware-memory',
    [ValidateRange(1,20)][int]$PeakSeconds=5,
    [ValidateSet('none','prefetch-off','window64','window2048','sampler-off','buffer-sharps','compute-barriers')][string]$Experiment='none'
)
Set-StrictMode -Version Latest
$ErrorActionPreference='Stop'
if($EnvironmentProbeSeconds){
    if(-not $ObserveEnvironment -or $FullCapture -or $CriticalCapture -or $CpuHotspots -or
       $Candidate -ne 'none' -or $ProbeCandidate -or $Experiment -ne 'none' -or $Profile -ne 'lightweight'){
        throw 'Environment probe requires native lightweight observation without a performance candidate or release.'
    }
    if(-not $PSBoundParameters.ContainsKey('Rounds')){$Rounds=1}
    if($Rounds -ne 1){throw 'Environment probe is one no-release session.'}
}
if($ObserveEnvironment -and ($FullCapture -or $CpuHotspots)){throw 'External environment observation is restricted to native collection.'}
if($CpuHotspotsSamplingMode -ne 'hw' -and -not $CpuHotspots){throw 'Software CPU hotspots is an explicit standalone CPU diagnostic, not a Full fallback.'}
# Native defaults to one A/B pair, reusing the existing cache without extra releases.
$simpleNative=-not ($FullCapture -or $CriticalCapture)
if($NormalRendering -and ((-not $simpleNative -and -not $DrawPrepAudit) -or $CpuHotspots -or $ProbeCandidate -or
    $Candidate -ne 'feedback-arena-pool' -or $Profile -ne 'lightweight' -or $Experiment -ne 'none')){
    throw 'Normal rendering comparison requires one compact-pool lightweight trial.'
}
$auditCount=[int][bool]$ReadbackBarrierAudit+[int][bool]$PageProtectAudit+[int][bool]$ComputeScopeAudit+[int][bool]$ReadbackWaitScopeAudit+[int][bool]$ComputeProtectAudit
if($DrawPrepAudit -and ($auditCount -ne 0 -or -not $CriticalCapture -or $FullCapture -or
    $CpuHotspots -or $ProbeCandidate -or $Profile -ne 'lightweight' -or
    $Candidate -ne 'feedback-arena-pool' -or $CandidateBaseline -or $Rounds -ne 1 -or
    $Experiment -ne 'none' -or -not $NormalRendering -or $ComputeTimingHash -or $TraceShaderHash)){
    throw 'Draw preparation audit requires one compact-pool critical release with normal rendering.'
}
if($auditCount -and
    (-not $CriticalCapture -or $FullCapture -or $CpuHotspots -or $Profile -ne 'lightweight' -or
     $Candidate -ne 'none' -or $ProbeCandidate -or $ComputeTimingHash -or
     ($FeedbackTraceAddress -and -not $ReadbackWaitScopeAudit) -or
     $auditCount -gt 1)){
    throw 'Each audit requires an isolated lightweight critical capture.'
}
if($ReadbackWaitScopeAudit -and ($Experiment -ne 'none' -or $Rounds -ne 1)){
    throw 'Readback wait audit requires one baseline release.'
}
if($ComputeProtectAudit -and ($Experiment -ne 'none' -or $Rounds -ne 1)){
    throw 'Compute protection audit requires one baseline release.'
}
if($RendererPgoTrial -ne 'Off' -and
    (-not $simpleNative -or $CpuHotspots -or $ProbeCandidate -or $Candidate -ne 'none' -or
     $Experiment -ne 'none' -or $Rounds -ne 1 -or $Profile -ne 'lightweight')){
    throw 'A renderer PGO trial requires exactly one independent lightweight release.'
}
if($CpuHotspots){
    if($FullCapture -or $CriticalCapture -or $ProbeCandidate -or $Candidate -ne 'none' -or $Experiment -ne 'none'){
        throw 'CPU hotspots is one isolated native diagnostic, without Full, critical trace or candidates.'
    }
    if(-not $PSBoundParameters.ContainsKey('Rounds')){$Rounds=1}
    if($Rounds -ne 1 -or $Profile -ne 'lightweight'){throw 'CPU hotspots requires one lightweight release.'}
}
if($ComputeTimingHash -and -not $CriticalCapture){throw 'ComputeTimingHash requires bounded native CriticalCapture.'}
if($ComputeTimingStartSeconds -and (-not $ComputeTimingHash -or -not $CriticalCapture -or $FullCapture)){
    throw 'ComputeTimingStartSeconds requires a bounded native compute timing capture.'
}
if($TraceShaderHash -and -not $ComputeTimingStartSeconds){
    throw 'TraceShaderHash requires a delayed bounded native compute timing capture.'
}
$traceWord=0L;$secondWord=0L
if($FeedbackTraceAddress){
    if(-not $CriticalCapture){throw 'FeedbackTraceAddress requires bounded native CriticalCapture.'}
    $traceWord=[Convert]::ToUInt64($FeedbackTraceAddress,16)
    if(-not $traceWord -or ($traceWord -band 3)){throw 'FeedbackTraceAddress must be a nonzero aligned word.'}
}
if($SecondFeedbackTraceAddress){
    if(-not $FeedbackTraceAddress -or $FullCapture){
        throw 'SecondFeedbackTraceAddress requires a primary address and bounded native CriticalCapture.'
    }
    $secondWord=[Convert]::ToUInt64($SecondFeedbackTraceAddress,16)
    if(-not $secondWord -or ($secondWord -band 3)){throw 'SecondFeedbackTraceAddress must be a nonzero aligned word.'}
    if($secondWord -eq $traceWord){throw 'SecondFeedbackTraceAddress must differ from the primary address.'}
}
if($CandidateBaseline -and $Candidate -eq 'none'){throw 'CandidateBaseline requires Candidate.'}
if($Candidate -in @('readback-copy-stage','image-read-transition','feedback-arena','vertex-input-key','feedback-arena-group','feedback-arena-pool','feedback-arena-tiled') -and
   (($CriticalCapture -and $Candidate -notin @('feedback-arena','feedback-arena-group','feedback-arena-pool','feedback-arena-tiled')) -or $Profile -notin @('suite','lightweight'))){
    throw 'The requested candidate requires a lightweight release.'
}
if($Candidate -ne 'none'){
    if($FullCapture -or $ProbeCandidate -or $Experiment -ne 'none'){
        throw 'Feedback candidates use independent native single-release trials, not Full or another experiment.'
    }
    if(-not $PSBoundParameters.ContainsKey('Rounds')){$Rounds=1}
    if($Rounds -ne 1){throw 'Feedback candidate trials require Rounds 1. Use super_feedback.bat for a fresh-process pair.'}
    if($Profile -eq 'suite'){$Profile='lightweight'}
    if($Profile -notin @('lightweight','counters')){throw 'Feedback candidates support lightweight or counters.'}
}
if($simpleNative){
    $MinimumWarmup=0;$MaximumWarmup=0
    if(-not $EnvironmentProbeSeconds -and -not $CpuHotspots -and $RendererPgoTrial -eq 'Off' -and $Candidate -eq 'none' -and
       -not $PSBoundParameters.ContainsKey('Experiment') -and $Profile -in @('suite','lightweight','counters')){
        $Experiment='compute-barriers'
    }
    if($Experiment -ne 'none' -and $Rounds % 2 -ne 0){throw 'Native comparison uses A then B: use -Rounds 2 (or an explicitly requested even count).'}
}
# Full reuses the persistent cache; one round means one actual super release.
if($FullCapture){if(-not $PSBoundParameters.ContainsKey('Rounds')){$Rounds=1};$MinimumWarmup=0;$MaximumWarmup=0}
if($ProbeCandidate -and -not $CriticalCapture){throw 'ProbeCandidate requires CriticalCapture.'}
if($CriticalCapture){
    if($FullCapture -or $Experiment -ne 'none' -or $Profile -notin @('lightweight','counters')){throw 'Critical capture requires native lightweight/counters, no experiment.'}
    $Rounds=1;$MinimumWarmup=0;$MaximumWarmup=0
}
if($PSVersionTable.PSVersion.Major -ne 5){throw 'Use diagnostics/super.bat (the installed Windows PowerShell 5.1 entry).'}
if($MaximumWarmup -lt $MinimumWarmup){throw 'MaximumWarmup must be at least MinimumWarmup.'}
if($FullCapture -and $Profile -ne 'suite'){throw 'The Full entry selects the existing Full collector; use super.bat for individual native profiles.'}
if($Experiment -ne 'none' -and ($FullCapture -or $Profile -notin @('suite','lightweight','counters'))){throw 'Experiments require native lightweight/counter collection.'}
. (Join-Path $PSScriptRoot 'tools.ps1')
. (Join-Path $PSScriptRoot 'diagnostic_tools.ps1')
. (Join-Path $PSScriptRoot 'super_capture_tools.ps1')
if($ObserveEnvironment){. (Join-Path $PSScriptRoot 'super_environment.ps1')}
if($CpuHotspots){
    . (Join-Path $PSScriptRoot 'super_cpu_hotspots.ps1')
    if($CpuHotspotsSamplingMode -eq 'hw' -and -not(Test-DiagnosticAdministrator)){throw 'CPU hardware sampling requires an elevated Windows PowerShell. No game started.'}
}
$root=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$powerShell=Join-Path $PSHOME 'powershell.exe'
$fullEntry=$null;$fullEntryPath=$null
if($FullCapture){
    $null=[IO.Directory]::CreateDirectory((Join-Path $root 'state/super-captures'))
    $fullEntryPath=Join-Path $root 'state/super-captures/last-full-entry.json'
    $fullEntry=[ordered]@{schema=1;pid=$PID;started_utc=[DateTime]::UtcNow.ToString('o');updated_utc=$null
        status='starting';exit_code=$null;error=$null;batch=$null;session=$null;elevated_child_pid=$null}
    $fullEntry.updated_utc=$fullEntry.started_utc
    Write-JsonUtf8 $fullEntryPath $fullEntry
    $identity=[Security.Principal.WindowsIdentity]::GetCurrent()
    try{$administrator=([Security.Principal.WindowsPrincipal]::new($identity)).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)}finally{$identity.Dispose()}
    if(-not $administrator){
        $child=$null;$result=1
        try{
            # This console is the user's interactive Full entry, matching full.bat.
            $elevatedArguments=@(Get-SuperElevatedArguments $PSCommandPath $PSBoundParameters)
            Write-Host 'Full 采集需要管理员权限，正在打开采集窗口；此窗口将等待并显示最终结果。'
            $fullEntry.status='elevating';Write-JsonUtf8 $fullEntryPath $fullEntry
            $child=Start-Process -FilePath $powerShell -ArgumentList (($elevatedArguments|ForEach-Object{Quote-WindowsArgument $_}) -join ' ') -WorkingDirectory $root -Verb RunAs -WindowStyle Normal -PassThru
            $fullEntry.elevated_child_pid=$child.Id
            $null=$child.Handle
            while(-not $child.WaitForExit(1000)){}
            $indexPath=Join-Path $root 'state/super-captures/last-full-run.json'
            $index=if(Test-Path -LiteralPath $indexPath){Read-JsonUtf8 $indexPath}else{$null}
            $entryErrors=@(Get-SuperFullEntryErrors $child.ExitCode $index ([DateTime]::Parse($fullEntry.started_utc)))
            if($entryErrors.Count){throw ($entryErrors -join ' ')}
            $fullEntry.batch=$index.batch;$fullEntry.session=$index.session
            $fullEntry.status='collected';$result=0
            Write-Host ('Full 采集成功：'+$index.batch)
        }catch{
            $fullEntry.status='failed';$fullEntry.error=$_.Exception.Message
            # A pre-batch failure in the elevated script is recorded before its
            # window closes; surface that error in the original console too.
            try{
                $childEntry=Read-JsonUtf8 $fullEntryPath
                if($child -and $childEntry.pid -eq $child.Id){
                    if($childEntry.error){$fullEntry.error+=' '+$childEntry.error}
                    $fullEntry.batch=$childEntry.batch;$fullEntry.session=$childEntry.session
                }
            }catch{}
            Write-Host ('Full 执行失败：'+$fullEntry.error) -ForegroundColor Red
        }finally{
            $fullEntry.exit_code=$result;$fullEntry.updated_utc=[DateTime]::UtcNow.ToString('o')
            Write-JsonUtf8 $fullEntryPath $fullEntry
            if($child){$child.Dispose()}
        }
        exit $result
    }
}
$session=$null;$record=$null;$launcher=$null;$batchLock=$null;$batchPath=$null;$batchRelative=$null;$batch=$null
$exitCode=1;$events=@();$cycleNumber=0
$fullControl=$null;$fullPlan=$null
$cpuHotspotsOwner=$null;$cpuHoldOpen=$false
$environmentOwner=$null
$currentVariant=@{name='baseline';tuning=@{}}
function Save-SuperBatch {
    $batch.updated_utc=[DateTime]::UtcNow.ToString('o')
    Write-JsonUtf8 (Join-Path $batchPath 'capture.json') $batch
    Write-JsonUtf8 (Join-Path $root 'state/super-captures/last-run.json') @{
        batch=(Get-RelativePath $root $batchPath);session=$session;status=$batch.status
        updated_utc=$batch.updated_utc;error=$batch.error
    }
    $modeIndex=if($FullCapture){'last-full-run.json'}else{'last-native-run.json'}
    Write-JsonUtf8 (Join-Path $root ('state/super-captures/'+$modeIndex)) @{
        batch=$batchRelative;session=$session;status=$batch.status;updated_utc=$batch.updated_utc;error=$batch.error
    }
}
function Save-SuperFullController {
    if($fullControl){Write-JsonUtf8 (Join-Path $record.Path 'diagnostics/super-controller.json') $fullControl}
}
function Assert-SuperLive {
    if($environmentOwner){$null=Read-SuperEnvironment $environmentOwner}
    if($launcher -and $launcher.HasExited){throw 'The game launcher exited before collection finished; see launcher logs.'}
    if($CriticalCapture -and $record){
        $trace=Join-Path $record.Path 'path-trace.csv'
        if((Test-Path -LiteralPath $trace) -and (Get-Item -LiteralPath $trace).Length -gt 64MB){throw 'Critical trace exceeded 64 MiB; raw data preserved.'}
    }
    $volume=[IO.DriveInfo]::new([IO.Path]::GetPathRoot($root))
    if($volume.AvailableFreeSpace -lt 2GB){throw 'Free disk space is below 2 GiB; collection stopped.'}
    if($FullCapture -and $record){
        $collectorState=Read-SuperCollectorStatus $record.Path
        if($collectorState){
            if($collectorState.complete -or $collectorState.status -in @('failed','partial','disk-space-stop','interrupted')){
                throw ('Full collector ended before the workload: '+$collectorState.status+'; '+($collectorState.errors -join '; '))
            }
        }
    }
}
function Send-SuperCommand([string]$Action,[string]$Button='down') {
    Assert-SuperLive
    $parameters=@{Action=$Action;Session=$session}
    if($Action -eq 'button'){$parameters.Button=$Button;$parameters.DurationMs=200}
    if($Action -eq 'button' -and $FullCapture){$parameters.MinimumFrames=2}
    & (Join-Path $PSScriptRoot 'control.ps1') @parameters | Out-Null
    $command=Read-JsonUtf8 (Join-Path $record.Path 'command.json')
    $ackPath=Join-Path $record.Path ('control-acks/'+([long]$command.sequence).ToString('D16')+'.json')
    $wait=[Diagnostics.Stopwatch]::StartNew()
    while(-not(Test-Path -LiteralPath $ackPath)){
        Assert-SuperLive
        $timeout=if($batch.status -eq 'loading'){180}elseif($FullCapture){60}else{15}
        if($wait.Elapsed.TotalSeconds -gt $timeout){throw ('No native acknowledgement for '+$Action)}
        Start-Sleep -Milliseconds 50
    }
    $ack=Read-JsonUtf8 $ackPath
    if($ack.sequence -ne $command.sequence -or $ack.action -ne $Action -or -not $ack.accepted){throw ('Native command rejected: '+$ack.error)}
    $script:events+=@($ack)
    return $ack
}
function Get-SuperSnapshot {
    $ack=Send-SuperCommand 'status'
    $snapshot=Read-JsonUtf8 (Join-Path $record.Path 'status.json')
    if($snapshot.sequence -ne $ack.sequence){throw 'Native status sequence mismatch.'}
    $script:events+=@($snapshot)
    return $snapshot
}
function Wait-SuperSeconds([double]$Seconds) {
    $wait=[Diagnostics.Stopwatch]::StartNew()
    while($wait.Elapsed.TotalSeconds -lt $Seconds){Assert-SuperLive;Start-Sleep -Milliseconds 200}
}
function Wait-SuperFramePoint($Anchor,[long]$Frames,$CycleClock) {
    while($true){
        Assert-SuperLive
        if($CycleClock.Elapsed.TotalSeconds -gt $fullPlan.super_capture.cycle_timeout_seconds){
            throw 'Full super cycle timed out waiting for guest progress or recovery; no next release was sent.'
        }
        $snapshot=Get-SuperSnapshot
        if(Test-SuperFrameProgress $Anchor $snapshot $Frames){return $snapshot}
        if($CycleClock.Elapsed.TotalSeconds-$script:superProgressNotice -ge 30){
            Write-Host ('Full 等待游戏推进：本轮 '+[int]$CycleClock.Elapsed.TotalSeconds+' 秒，当前节点 '+((Get-SuperFrameCount $snapshot)-(Get-SuperFrameCount $Anchor))+'/'+$Frames+' 帧。')
            $script:superProgressNotice=$CycleClock.Elapsed.TotalSeconds
        }
        Wait-SuperSeconds 0.5
    }
}
function Set-SuperProfile([string]$Name) {
    $values=@{perf_log=0;profile_stacks=0;gpu_profile=0;path_trace=0;dump_frames=0
        gpu_profile_compute_hash='0';gpu_profile_compute_limit=0;gpu_profile_draw_hash='0';gpu_profile_draw_limit=0}
    if($Name -ne 'lightweight'){$values.perf_log=1}
    if($Name -eq 'compute'){$values.gpu_profile=2}
    if($Name -eq 'graphics'){$values.gpu_profile=3;$values.profile_stacks=1}
    if($Name -eq 'full'){$values.gpu_profile=2;$values.profile_stacks=1}
    if($ComputeTimingHash){
        $values.gpu_profile=if($ComputeTimingStartSeconds){1}else{2}
        $values.gpu_profile_compute_hash=$ComputeTimingHash
        $values.gpu_profile_compute_limit=8192
    }
    if($FeedbackTraceAddress){
        $values.trace_reads=('{0:x}-{1:x}' -f $traceWord,($traceWord+4))
    }
    $values.trace_writer_word=if($SecondFeedbackTraceAddress){$SecondFeedbackTraceAddress}else{'0'}
    Set-DiagnosticTuning $root $record.Path $values
    # The engine reloads tuning asynchronously; prove the requested mode reached it.
    $wait=[Diagnostics.Stopwatch]::StartNew()
    do{
        Wait-SuperSeconds 1
        $snapshot=Get-SuperSnapshot
        $traceReady=$true
        if($FeedbackTraceAddress){
            $traceReady=$null -ne $snapshot.PSObject.Properties['trace_reads_lo'] -and
                $null -ne $snapshot.PSObject.Properties['trace_reads_hi'] -and
                $null -ne $snapshot.PSObject.Properties['trace_writer_word'] -and
                [uint64]$snapshot.trace_reads_lo -eq $traceWord -and
                [uint64]$snapshot.trace_reads_hi -eq ($traceWord+4) -and
                [uint64]$snapshot.trace_writer_word -eq $secondWord
        }
        if($wait.Elapsed.TotalSeconds -gt 15){throw ('Tuning reload timeout: '+$Name)}
    }while(-not $traceReady -or [int]$snapshot.perf_log -ne $values.perf_log -or [int]$snapshot.gpu_profile -ne $values.gpu_profile)
}
function Set-SuperVariant($Variant) {
    if(-not $Variant.tuning.Count){return}
    Set-DiagnosticTuning $root $record.Path $Variant.tuning
    $expected=Get-SuperExpectedTuning $Variant.tuning
    $tuningWait=[Diagnostics.Stopwatch]::StartNew()
    do{
        Wait-SuperSeconds 1
        $snapshot=Get-SuperSnapshot
        if($tuningWait.Elapsed.TotalSeconds -gt 15){throw 'Experiment tuning reload timeout.'}
        $applied=$true
        foreach($key in $expected.Keys){
            # Isolated karma-fill builds may omit FullCapture-only receipt fields such as compute_barrier_scope.
            $actual=if($null -ne $snapshot.PSObject.Properties[$key]){$snapshot.$key}
                elseif($key -eq 'compute_barrier_scope' -and $expected[$key] -eq 0){0}
                else{$null}
            if($actual -ne $expected[$key]){$applied=$false}
        }
        if($Candidate -eq 'cpu-placement' -and -not $CandidateBaseline){
            [UInt64]$mask=$expected.gpu_threads_affinity
            if(([UInt64]$snapshot.process_affinity -band $mask) -ne $mask){throw 'The process does not allow the complete enumerated performance-core mask.'}
        }
    }while(-not $applied)
}
function Invoke-SuperEnvironmentProbe([int]$Seconds) {
    Set-SuperProfile 'lightweight'
    $environmentOwner.Phase='probe'
    $batch.environment_probe=[ordered]@{seconds=$Seconds;before=(Get-SuperSnapshot);after=$null;no_release=$true}
    for($elapsed=0;$elapsed -lt $Seconds;$elapsed+=5){
        Wait-SuperSeconds ([Math]::Min(5,$Seconds-$elapsed))
        $batch.environment_probe.after=Get-SuperSnapshot
    }
    Write-SuperRounds (Join-Path $record.Path 'super-rounds.json') @()
    $batch.comparison_qualified=$false
}
function Invoke-SuperCycle([string]$Kind,[string]$Collector,[int]$Ordinal,[switch]$Evidence) {
    $script:cycleNumber++
    $script:events=@()
    $cycle=[ordered]@{cycle=$cycleNumber;kind=$Kind;profile=$Collector;ordinal=$Ordinal
        variant=$currentVariant.name;requested_tuning=(Get-SuperExpectedTuning $currentVariant.tuning)
        requested_ini_tuning=$currentVariant.tuning
        status='running';warm=$false;meter_consumed=$false;error=$null
        before=$null;filled=$null;release=$null;after=$null;recovery=$null
        capture_suppressed_new_compilation=$false
        progress_clock=if($FullCapture){'native-guest-flips'}else{'wall-clock'}
        reference_fps=if($FullCapture){$fullPlan.super_capture.reference_fps}else{$null}
        elapsed_seconds=$null;events_file=('super-trial-'+$cycleNumber+'.json')}
    $cycleClock=[Diagnostics.Stopwatch]::StartNew()
    $script:superProgressNotice=0
    try{
        if($environmentOwner){Wait-SuperEnvironmentQuiet $environmentOwner -Preflight:$false}
        if($environmentOwner){$environmentOwner.Phase='measurement'}
        if($FullCapture){Set-SuperProfile 'full'}
        $null=Send-SuperCommand 'karma-fill'
        $fillWait=[Diagnostics.Stopwatch]::StartNew()
        do{
            Wait-SuperSeconds 0.3
            $filled=Get-SuperSnapshot
            if($fillWait.Elapsed.TotalSeconds -gt 15){throw 'The guest did not complete the fill request.'}
        }while($filled.karma_fill_completed -ne $filled.karma_fill_requested)
        if($filled.karma_fill_count -le 0 -or $filled.karma_fill_count -gt 100000){throw 'The native setter did not fill an unlocked super move.'}
        $cycle.filled=$filled
        if($CpuHotspots){Start-SuperCpuHotspots $root $session ([ref]$script:cpuHotspotsOwner) $CpuHotspotsSamplingMode}
        if($CriticalCapture){$null=Send-SuperCommand 'trace-start'}
        # Evidence screenshots occur only in an excluded warmup cycle.
        if($Evidence){$null=Send-SuperCommand 'screenshot'}
        if($FullCapture){$before=Wait-SuperFramePoint $filled ($BaselineSeconds*$fullPlan.super_capture.reference_fps) $cycleClock}
        else{Wait-SuperSeconds $BaselineSeconds;$before=Get-SuperSnapshot}
        $cycle.before=$before
        if($environmentOwner){Assert-SuperEnvironmentBeforeRelease $environmentOwner}
        if($Candidate -eq 'feedback-arena'){Assert-SuperFeedbackArenaReceipt $before ([bool]$CandidateBaseline)}
        if($Candidate -eq 'feedback-arena-group'){Assert-SuperFeedbackArenaGroupReceipt $before ([bool]$CandidateBaseline)}
        if($Candidate -in @('feedback-arena-pool','feedback-arena-tiled')){
            $poolOff=[bool]($CandidateBaseline -and $Candidate -eq 'feedback-arena-pool')
            $poolGroup=if($Candidate -eq 'feedback-arena-tiled' -and -not $CandidateBaseline){64}else{2048}
            Assert-SuperFeedbackArenaPoolReceipt $before $poolOff $null $poolGroup
        }
        if($Candidate -eq 'vertex-input-key' -and
            (($before.vertex_input_key_hits -gt 0) -eq [bool]$CandidateBaseline)){
            throw 'The vertex input candidate did not match its pre-release use receipt.'
        }
        if($before.karma_fill_count -ne $filled.karma_fill_count -or $before.karma_fill_count -le 0){throw 'The super meter changed before D-pad down; release consumption cannot be attributed to this trial.'}
        $cycle.release=Send-SuperCommand 'button'
        $clock=[Diagnostics.Stopwatch]::StartNew();$snapshots=@()
        if($FullCapture -and $Kind -eq 'measurement'){
            $fullControl.releases+=@($cycle.release.sequence);Save-SuperFullController
        }
        $schedule=@(0.5,1.5,3,5,8,12,18,25)
        if($ReleaseSeconds -gt 25){$schedule+=@($ReleaseSeconds)}
        if($ComputeTimingStartSeconds){$schedule=@(($schedule+@($ComputeTimingStartSeconds))|Sort-Object -Unique)}
        if($FullCapture){$schedule+=@($PeakSeconds,($PeakSeconds+5),($ReleaseSeconds/2));$schedule=@($schedule|Sort-Object -Unique)}
        foreach($at in $schedule){
            if($FullCapture){$point=Wait-SuperFramePoint $cycle.release ([long][Math]::Ceiling($at*$fullPlan.super_capture.reference_fps)) $cycleClock}
            else{while($clock.Elapsed.TotalSeconds -lt $at){Assert-SuperLive;Start-Sleep -Milliseconds 50}}
            if($ComputeTimingStartSeconds -and $at -eq $ComputeTimingStartSeconds){
                $windowTuning=@{gpu_profile=2}
                if($TraceShaderHash){$windowTuning.trace_shader=$TraceShaderHash}
                Set-DiagnosticTuning $root $record.Path $windowTuning
            }
            if($Evidence -and $at -in @(1.5,5)){$null=Send-SuperCommand 'screenshot'}
            if($FullCapture){
                if($Kind -eq 'measurement' -and $at -eq $PeakSeconds -and -not $fullControl.gpu_sequence){
                    if(-not (Test-SuperReuseCachePlan $fullPlan) -and -not(Test-SuperWarmCycle $filled (@($before)+$snapshots+@($point)) $point)){
                        $cycle.capture_suppressed_new_compilation=$true
                        Write-Host '本轮抓帧前出现新编译；跳过 GPU 详细抓帧，完成恢复后重新预热。'
                    }else{
                        $gpuAction=if($fullPlan.gpu_capture_backend -eq 'nsight'){'nsight'}else{'capture'}
                        $gpuAck=Send-SuperCommand $gpuAction
                        if($gpuAction -eq 'capture' -and -not $gpuAck.renderdoc_ready){throw 'RenderDoc API did not accept the super capture.'}
                        $fullControl.gpu_sequence=$gpuAck.sequence;$fullControl.gpu_release_sequence=$cycle.release.sequence;Save-SuperFullController
                    }
                }
                if($Kind -eq 'measurement' -and -not $cycle.capture_suppressed_new_compilation -and $at -eq ($PeakSeconds+5)){
                    Set-DiagnosticTuning $root $record.Path @{dump_frames=2}
                    $fullControl.guest_trace_triggered=$true
                    $fullControl.guest_trace_requests+=@{release_sequence=$cycle.release.sequence;requested_utc=[DateTime]::UtcNow.ToString('o')
                        frame_count=(Get-SuperFrameCount $point)}
                    Save-SuperFullController
                }
                if($at -eq ($ReleaseSeconds/2)){Set-DiagnosticTuning $root $record.Path @{gpu_profile=3}}
            }
            $snapshots+=@(Get-SuperSnapshot)
        }
        $after=$snapshots[-1];$cycle.after=$after
        $cycle.meter_consumed=Test-SuperMeterConsumed $filled.karma_fill_count $snapshots
        if($Candidate -eq 'feedback-arena'){Assert-SuperFeedbackArenaReceipt $after ([bool]$CandidateBaseline) $before}
        if($Candidate -eq 'feedback-arena-group'){Assert-SuperFeedbackArenaGroupReceipt $after ([bool]$CandidateBaseline) $before}
        if($Candidate -in @('feedback-arena-pool','feedback-arena-tiled')){
            Assert-SuperFeedbackArenaPoolReceipt $after $poolOff $before $poolGroup
        }
        if($Candidate -eq 'vertex-input-key' -and
            (($after.vertex_input_key_hits -gt $before.vertex_input_key_hits) -eq [bool]$CandidateBaseline)){
            throw 'The vertex input candidate did not match its release use receipt.'
        }
        if(-not $cycle.meter_consumed){throw 'D-pad down was acknowledged, but no super-meter consumption was observed.'}
        if($FullCapture){
            # A frame budget alone is not an animation-state flag. Also require
            # three recovered samples separated by real guest flips before refill.
            $recovery=[ordered]@{completed=$false;criterion='frame-budget-and-dispatch-recovery';samples=@()}
            $cycle.recovery=$recovery;$streak=0;$last=$after
            while($streak -lt $fullPlan.super_capture.recovery_samples){
                $last=Wait-SuperFramePoint $last $fullPlan.super_capture.recovery_sample_frames $cycleClock
                $recovery.samples+=@($last);$snapshots+=@($last)
                $streak=if(Test-SuperRecoverySnapshot $before $last){$streak+1}else{0}
            }
            $recovery.completed=$true;$cycle.after=$last
            # Outside the recorded release window; evidence remains reviewable.
            $null=Send-SuperCommand 'screenshot'
        }
        $cycle.warm=Test-SuperWarmCycle $filled (@($before)+$snapshots) $cycle.after
        $cycle.status=if($Evidence){'warmup-evidence'}elseif(-not $cycle.warm){if($FullCapture){'warmup-new-compilation'}else{'captured-new-compilation'}}elseif($Kind -eq 'measurement'){'collected'}else{'warmup-stable'}
        if($FullCapture -and (Test-SuperReuseCachePlan $fullPlan) -and $Kind -eq 'measurement'){
            $cycle.status='collected'
            if(-not $cycle.warm){Write-Host '本次数据已保存，含新编译；不追加预热，不作为零编译性能样本。'}
        }
        if($FullCapture -and -not (Test-SuperReuseCachePlan $fullPlan) -and $Kind -eq 'measurement' -and -not $cycle.warm){
            if($fullControl.gpu_release_sequence -eq $cycle.release.sequence){
                throw 'New compilation invalidated the SDK-triggered detail trial; this one-shot GPU trace cannot be reused as warm. Raw data was preserved.'
            }
            $cycle.kind='warmup'
            $fullControl.releases=@($fullControl.releases|Where-Object {$_ -ne $cycle.release.sequence})
            $fullControl.phase='rewarming';Save-SuperFullController
        }
        # A screenshot/OCR operation is never part of a qualified measurement.
        if($Evidence){$cycle.warm=$false}
        return [pscustomobject]$cycle
    }catch{
        $cycle.status='failed';$cycle.error=$_.Exception.Message
        throw
    }finally{
        if($environmentOwner){$environmentOwner.Phase='loading'}
        $cycle.elapsed_seconds=$cycleClock.Elapsed.TotalSeconds
        Write-JsonUtf8 (Join-Path $record.Path $cycle.events_file) @($script:events)
        $batch.cycles+=@([pscustomobject]$cycle)
        Write-SuperRounds (Join-Path $record.Path 'super-rounds.json') @($batch.cycles)
        Save-SuperBatch
    }
}
try{
    if($Candidate -eq 'cpu-placement'){
        $topology=(& (Join-Path $PSScriptRoot 'read_cpu_topology.ps1'))|ConvertFrom-Json
        $enumeratedMask=Get-SuperPerformanceCoreMask $topology
        if(-not $PerformanceCoreMask -or [Convert]::ToUInt64($PerformanceCoreMask,16) -ne [Convert]::ToUInt64($enumeratedMask,16)){
            throw 'CPU placement mask differs from current OS topology; stopped before launching.'
        }
    }
    $manifest=Read-JsonUtf8 (Join-Path $root 'project.json')
    if(Get-Process -Name ([IO.Path]::GetFileNameWithoutExtension($manifest.runtime_path)) -ErrorAction SilentlyContinue){throw 'Close the running game before starting a collection batch.'}
    $area=Resolve-ProjectChild $root 'state/super-captures'
    $null=[IO.Directory]::CreateDirectory($area)
    $batchLock=[IO.File]::Open((Join-Path $area 'capture.lock'),[IO.FileMode]::OpenOrCreate,[IO.FileAccess]::ReadWrite,[IO.FileShare]::None)
    $batchPath=Join-Path $area ((Get-Date -Format 'yyyyMMdd-HHmmss')+'-'+$Mode+'-'+[Guid]::NewGuid().ToString('N').Substring(0,8))
    $null=[IO.Directory]::CreateDirectory($batchPath)
    $batchRelative=Get-RelativePath $root $batchPath
    $profiles=if($FullCapture){@('full')}elseif($Profile -eq 'suite'){@('lightweight')}else{@($Profile)}
    $batch=[ordered]@{schema=1;batch=$batchRelative;status='starting';error=$null;session=$null;mode=$Mode
        profiles=@($profiles);rounds_per_profile=$Rounds;minimum_warmup=$MinimumWarmup;maximum_warmup=$MaximumWarmup
        baseline_seconds=$BaselineSeconds;release_seconds=$ReleaseSeconds;consecutive_warm_required=0
        collector_mode=if($FullCapture){'full'}else{'native'};peak_seconds=$PeakSeconds
        warmup_policy='reuse-existing-cache'
        simple_native=[bool]$simpleNative;comparison_qualified=$true;comparison_warnings=@()
        critical_capture=[bool]$CriticalCapture
        cpu_hotspots=[bool]$CpuHotspots
        cpu_hotspots_sampling_mode=$(if($CpuHotspots){$CpuHotspotsSamplingMode}else{$null})
        compute_timing_hash=$ComputeTimingHash
        compute_timing_start_seconds=$ComputeTimingStartSeconds
        trace_shader_hash=$TraceShaderHash
        feedback_trace_address=$FeedbackTraceAddress
        second_feedback_trace_address=$SecondFeedbackTraceAddress
        vtune_cpu_mode=$(if($CpuHotspots){'hotspots-'+$CpuHotspotsSamplingMode}else{$VtuneCpuMode})
        progress_clock=if($FullCapture){'native-guest-flips'}else{'wall-clock'}
        experiment=$Experiment;variant_order=if($Candidate -ne 'none'){'independent single trial'}elseif($Experiment -eq 'none'){'baseline'}else{'AB repeated'}
        candidate=$Candidate;candidate_baseline=[bool]$CandidateBaseline;independent_trial=($Candidate -ne 'none')
        normal_rendering=[bool]$NormalRendering
        draw_prep_audit=[bool]$DrawPrepAudit
        observe_environment=[bool]$ObserveEnvironment
        environment_probe_seconds=$EnvironmentProbeSeconds;environment_probe=$null
        started_utc=[DateTime]::UtcNow.ToString('o');updated_utc=$null;finished_utc=$null
        scope=$(if($EnvironmentProbeSeconds){'standing-environment-probe-no-release'}else{'single-game-process'});automatic_analysis=$false;performance_acceptance=$false
        scene_evidence='heuristic OCR, pause response and native rendering counters; review scene evidence'
        state_replay='same loaded save and standing position; game-controlled cinematic camera; no save reload between cycles'
        cycles=@();launch_exit_code=$null}
    Save-SuperBatch
    Write-Host ('采集批次：'+(Get-RelativePath $root $batchPath))
    if($FullCapture){Write-Host ('复用已有缓存；自动读档、充能及释放 '+$Rounds+' 次，不追加大招预热；新编译单独标记。')}
    elseif($CriticalCapture){Write-Host 'Critical trace: reuse cache, one release, no additional warmup; stop above 64 MiB.'}
    elseif($Experiment -ne 'none'){Write-Host ('简易采集共 '+$Rounds+' 次：A 关闭优化，B 开启优化；单一采集档，复用缓存，不追加预热或自动重跑。')}
    else{Write-Host ('简易采集共 '+$Rounds+' 次；单一采集档，复用缓存，不追加预热或自动重跑。')}
    Write-Host '采集期间请勿移动人物、操作菜单或切换能力。脚本不分析结果，也不回存正式存档。'
    if($ObserveEnvironment){
        $environmentOwner=New-SuperEnvironment $batchPath
        Write-Host 'Checking external load before launching; no game input is sent while the desktop is active.'
        Wait-SuperEnvironmentQuiet $environmentOwner
        $environmentOwner.Phase='loading'
    }
    if($FullCapture){Write-Host ('Full：按实际游戏帧推进，60 帧为一个参考秒；首个正式大招推进 '+($PeakSeconds*60)+' 帧后触发 GPU 抓帧，每轮完整推进 '+($ReleaseSeconds*60)+' 帧并确认恢复后才再次充能。现实时间仅作超时保护。')}
    if($FullCapture){Write-Host 'VTune 完整硬件采集：Memory Access、DRAM 带宽校准、Full 全样本整理及逐轮 CPU 报告；失败时保留原始数据，不降级为软件采样。'}
    $arguments=@('-NoLogo','-NoProfile','-ExecutionPolicy','Bypass','-File',(Join-Path $PSScriptRoot 'launch.ps1'),'-Mode',$Mode,'-NoMenu','-SuperCapture','-SuperCaptureBatch',$batchRelative)
    if($FullCapture){$arguments+=@('-Diagnostic','full','-DiagnosticDelaySeconds','-1')}
    if($CriticalCapture){$arguments+='-CriticalCapture'}
    if($ReadbackBarrierAudit){$arguments+='-ReadbackBarrierAudit'}
    if($PageProtectAudit){$arguments+='-PageProtectAudit'}
    if($ComputeScopeAudit){$arguments+='-ComputeScopeAudit'}
    if($ReadbackWaitScopeAudit){$arguments+='-ReadbackWaitScopeAudit'}
    if($ComputeProtectAudit){$arguments+='-ComputeProtectAudit'}
    if($DrawPrepAudit){$arguments+='-DrawPrepAudit'}
    if($CpuHotspots){$arguments+='-CpuHotspots'}
    if($RendererPgoTrial -ne 'Off'){$arguments+=@('-RendererPgoTrial',$RendererPgoTrial)}
    $arguments+=@(Get-SuperStartupArguments $Candidate ([bool]$CandidateBaseline))
    $launcherOptions=@{
        FilePath=$powerShell;ArgumentList=(($arguments|ForEach-Object{Quote-WindowsArgument $_}) -join ' ')
        WorkingDirectory=$root;WindowStyle='Hidden';PassThru=$true
        RedirectStandardOutput=(Join-Path $batchPath 'launcher-output.log')
        RedirectStandardError=(Join-Path $batchPath 'launcher-error.log')
    }
    # Retain the helper's Windows handle from startup, as the other diagnostic
    # helpers do. Start-Process alone can lose ExitCode after a long redirected run.
    $launcher=Start-DiagnosticProcess $launcherOptions
    $startup=[Diagnostics.Stopwatch]::StartNew()
    do{
        Assert-SuperLive
        # PowerShell variable names are case-insensitive: keep the launch
        # receipt separate from the validated $Candidate selection.
        $pendingSessionRecord=Read-SuperCaptureSession $root $batchRelative
        if($pendingSessionRecord){$record=$pendingSessionRecord;$session=$pendingSessionRecord.Launch.session}
        if($startup.Elapsed.TotalSeconds -gt 120){throw 'No new collection-only game session appeared.'}
        if(-not $session){Start-Sleep -Milliseconds 500}
    }while(-not $session)
    $batch.session=$session;$batch.status='loading';Save-SuperBatch
    if($environmentOwner){
        $environmentOwner.TargetPid=[uint32]$record.Launch.pid
        $environmentOwner.TargetStartedUtc=$record.Launch.process_started
        $created=([DateTime]::Parse($record.Launch.process_started).ToUniversalTime()).ToFileTimeUtc()
        $environmentOwner.Observer.SetTarget($environmentOwner.TargetPid,$created)
    }
    $null=[IO.Directory]::CreateDirectory((Join-Path $record.Path 'diagnostics'))
    if($FullCapture){
        $fullPlan=Read-JsonUtf8 (Join-Path $record.Path 'diagnostics/plan.json')
        if(-not $fullPlan.PSObject.Properties['super_capture'] -or $fullPlan.super_capture.batch -ne $batchRelative){throw 'The Full launcher did not publish the super-specific diagnostic plan.'}
        # Keep [] and single-round [object] valid for both PS and Python readers.
        Write-SuperRounds (Join-Path $record.Path 'super-rounds.json') @()
        $fullControl=[ordered]@{schema=1;session=$session;batch=$batchRelative;pid=$record.Launch.pid
            process_started=$record.Launch.process_started;phase='warming';scene_evidence=$null
            releases=@();gpu_sequence=$null;gpu_release_sequence=$null;guest_trace_triggered=$false;guest_trace_requests=@()}
        Save-SuperFullController
    }
    $plan=@{profile='cpu';seconds=60;automation=@{boot_timeout_seconds=360}}
    $navigation=New-DiagnosticAutomation $plan
    $navigationClock=[Diagnostics.Stopwatch]::StartNew()
    while(-not $navigation.ready){
        $null=Get-SuperSnapshot
        Update-DiagnosticAutomation $root $session $plan $navigation $navigationClock.Elapsed.TotalSeconds
        Wait-SuperSeconds 2
    }
    if($Candidate -in @('c73-scan','particle-poll-pause')){
        $gameLog=Join-Path $record.Path 'log/CUSA00309.log'
        $receipt=if($Candidate -eq 'c73-scan'){'C73 scan experiment: verified SPIR-V replacement'}else{'Particle poll PAUSE experiment: verified call replacement'}
        $scanLoaded=Test-Path -LiteralPath $gameLog -PathType Leaf
        if($scanLoaded){
            $scanLoaded=[bool](Select-String -LiteralPath $gameLog -SimpleMatch $receipt -Quiet)
        }
        if($scanLoaded -eq [bool]$CandidateBaseline){throw 'Experimental replacement receipt does not match the trial.'}
    }
    if($FullCapture){
        $fullControl.scene_evidence=$navigation.scene_evidence;$fullControl.phase='recording';Save-SuperFullController
        $readyWait=[Diagnostics.Stopwatch]::StartNew()
        do{
            Wait-SuperSeconds 1
            $collectorState=Read-SuperCollectorStatus $record.Path
            if($readyWait.Elapsed.TotalSeconds -gt 240){throw 'Full collector did not acknowledge the measurement window.'}
        }while(-not $collectorState -or $collectorState.status -ne 'recording')
    }
    if($CriticalCapture){Wait-SuperSeconds 20}
    if($EnvironmentProbeSeconds){
        Write-Host 'Observing the standing scene only; no charge or super-release input.'
        Invoke-SuperEnvironmentProbe $EnvironmentProbeSeconds
    }else{
    foreach($collector in $profiles){
        Write-Host ('开始采集组：'+$collector)
        $batch.status='collecting-'+$collector;Save-SuperBatch
        Set-SuperProfile $collector
        $accepted=0;$attempt=0;$stable=2
        while($accepted -lt $Rounds){
            $attempt++
            if($attempt -gt $Rounds+10){throw ('Too many new-compilation rounds in '+$collector+'; incomplete batch preserved.')}
            $kind=if($FullCapture -or $stable -ge 2){'measurement'}else{'warmup'}
            if($FullCapture -and $kind -eq 'measurement'){$fullControl.phase='recording';Save-SuperFullController}
            $currentVariant=Get-SuperExperimentVariant $Experiment $accepted
            if($Candidate -ne 'none'){$currentVariant=Get-SuperFeedbackVariant $Candidate -Baseline:$CandidateBaseline -PerformanceCoreMask $PerformanceCoreMask -NormalRendering:$NormalRendering}
            if($ProbeCandidate -or $ComputeScopeAudit){$currentVariant=@{name='probe-compute-barriers';tuning=@{compute_barrier_scope=1}}}
            if($currentVariant.tuning.Count){
                Set-SuperVariant $currentVariant
                Write-Host ('对比方案：'+$currentVariant.name)
            }
            $cycle=Invoke-SuperCycle $kind $collector ($accepted+1)
            if($CriticalCapture -and -not $cycle.warm){throw 'Critical trace compiled new work; retained for diagnosis, not warm performance acceptance.'}
            if($simpleNative){
                # Each requested release consumes one slot, even if it compiled new work.
                # Keep cold evidence, mark the pair unqualified, never add a hidden retry.
                $accepted++
                if(-not $cycle.warm){
                    $batch.comparison_qualified=$false
                    $batch.comparison_warnings+=('Round '+$accepted+' compiled new shaders/pipelines; raw data retained, pair not warm-qualified.')
                    Save-SuperBatch
                    Write-Host '此轮含新编译，已标记；保留数据，不追加重跑。'
                }
                Write-Host ('已保存 '+$collector+' '+$accepted+'/'+$Rounds)
                if($accepted -lt $Rounds){Wait-SuperSeconds 5}
                continue
            }
            if($Experiment -ne 'none' -and -not $cycle.warm -and $accepted -gt 0){
                Reset-SuperExperimentGroup $batch.cycles $collector
                Write-SuperRounds (Join-Path $record.Path 'super-rounds.json') @($batch.cycles)
                Save-SuperBatch
                $accepted=0
                Write-Host '新编译使当前交叉对比失效；保留原记录，预热后从 A 重新开始。'
            }
            $stable=if($cycle.warm){$stable+1}else{0}
            if($cycle.status -eq 'collected'){$accepted++;Write-Host ('已保存 '+$collector+' '+$accepted+'/'+$Rounds)}
            else{Write-Host '此轮仅计入预热，继续等待稳定。'}
            Wait-SuperSeconds 5
        }
    }
    }
    $batch.status='finalizing';Save-SuperBatch
    if($FullCapture){
        $fullControl.phase='complete';Save-SuperFullController
        $finalWait=[Diagnostics.Stopwatch]::StartNew();$lastNotice=-60
        while(-not $launcher.WaitForExit(1000)){
            if($finalWait.Elapsed.TotalSeconds-$lastNotice -ge 60){Write-Host 'Full 工具正在保存追踪并校验采集完整性，请等待自动结束。';$lastNotice=$finalWait.Elapsed.TotalSeconds}
            if($finalWait.Elapsed.TotalSeconds -gt $fullPlan.super_capture.finalization_timeout_seconds){throw 'Full collector finalization exceeded its bounded export timeout; preserve the session and its logs.'}
        }
    }else{
        if($CpuHotspots){Stop-SuperCpuHotspots $cpuHotspotsOwner}
        $null=Send-SuperCommand 'screenshot'
        Wait-SuperSeconds 2
        $null=Send-SuperCommand 'quit'
        if(-not $launcher.WaitForExit(60000)){throw 'The launcher did not finish flushing; preserve this incomplete batch and close the game normally.'}
    }
    $batch.launch_exit_code=$launcher.ExitCode
    $completedLaunch=Read-JsonUtf8 (Join-Path $record.Path 'launch.json')
    if($CpuHotspots){Complete-SuperCpuHotspots $cpuHotspotsOwner $batch.cycles[-1]}
    $finalCollector=if($FullCapture){Read-SuperCollectorStatus $record.Path}else{$null}
    $finalizationErrors=@(Get-SuperFinalizationErrors $batch.launch_exit_code $completedLaunch $batchRelative $session $finalCollector)
    if($finalizationErrors.Count){throw ($finalizationErrors -join ' ')}
    if($CriticalCapture){
        $traceMetadata=Read-JsonUtf8 (Join-Path $record.Path 'path-trace.json')
        $traceErrors=@(Get-SuperCriticalTraceErrors $traceMetadata -RequireDrawPrep:$DrawPrepAudit)
        Write-JsonUtf8 (Join-Path $record.Path 'diagnostics/critical-quality.json') @{
            validated=($traceErrors.Count -eq 0);errors=$traceErrors;metadata=$traceMetadata
        }
        if($traceErrors.Count){throw ($traceErrors -join ' ')}
    }
    $batch.status='collected';$exitCode=0
    if($environmentOwner -and ($environmentOwner.MeasurementSamples -eq 0 -or
        $environmentOwner.SuspectMeasurements -ne 0)){
        $batch.comparison_qualified=$false
        $batch.comparison_warnings+=('External load or missing observation during measurement; retained for diagnosis, not candidate elimination or FPS acceptance.')
    }
}catch{
    if($fullEntry){$fullEntry.error=$_.Exception.Message}
    if($batch){$batch.status='failed';$batch.error=$_.Exception.Message;Save-SuperBatch}
    Write-Host $_.Exception.Message -ForegroundColor Red
}finally{
    if($batch -and $exitCode -ne 0 -and $batch.status -ne 'failed'){
        $batch.status='failed';$batch.error='Collection interrupted before completion.';Save-SuperBatch
    }
    # A launcher may still be preparing when the driver fails. Mark cancellation
    # before it can start the game, and recover a receipt published during that race.
    if(-not $session -and $batchRelative -and $launcher -and -not $launcher.HasExited){
        $recoveryWait=[Diagnostics.Stopwatch]::StartNew()
        while(-not $session -and -not $launcher.HasExited -and $recoveryWait.Elapsed.TotalSeconds -lt 10){
            try{
                $pendingSessionRecord=Read-SuperCaptureSession $root $batchRelative
                if($pendingSessionRecord){$record=$pendingSessionRecord;$session=$pendingSessionRecord.Launch.session;$batch.session=$session}
            }catch{Write-Warning $_.Exception.Message;break}
            if(-not $session){Start-Sleep -Milliseconds 200}
        }
    }
    if($CpuHotspots -and $cpuHotspotsOwner -and $cpuHotspotsOwner.Process -and -not $cpuHotspotsOwner.Process.HasExited){
        try{Stop-SuperCpuHotspots $cpuHotspotsOwner}catch{
            $cpuHoldOpen=$true
            Write-Warning ('CPU collector detach failed. Owned game kept alive to preserve traces: '+$_.Exception.Message)
        }
    }
    if(-not $cpuHoldOpen -and $session -and $launcher -and -not $launcher.HasExited){
        try{
            if($FullCapture){Send-DiagnosticControl $root $session 'stop'}
            else{& (Join-Path $PSScriptRoot 'control.ps1') -Action quit -Session $session | Out-Null}
            if(-not $launcher.WaitForExit(30000)){Write-Warning 'The owned game has not finished exiting; close it normally before another batch.'}
        }catch{Write-Warning $_.Exception.Message}
    }
    if($environmentOwner){Close-SuperEnvironment $environmentOwner ($exitCode -eq 0)}
    if($batch){$batch.finished_utc=[DateTime]::UtcNow.ToString('o');Save-SuperBatch;Write-Host ('采集状态：'+$batch.status+'；目录：'+(Get-RelativePath $root $batchPath))}
    if($fullEntry){
        $fullEntry.exit_code=$exitCode;$fullEntry.status=if($exitCode -eq 0){'collected'}else{'failed'}
        $fullEntry.updated_utc=[DateTime]::UtcNow.ToString('o')
        if($batch){$fullEntry.batch=$batchRelative;$fullEntry.session=$session;$fullEntry.error=$batch.error}
        elseif($exitCode -ne 0 -and -not $fullEntry.error){$fullEntry.error='Full startup stopped before creating a batch; see the elevated console error.'}
        Write-JsonUtf8 $fullEntryPath $fullEntry
    }
    if($batchLock){$batchLock.Dispose()}
    if($launcher){$launcher.Dispose()}
}
exit $exitCode
