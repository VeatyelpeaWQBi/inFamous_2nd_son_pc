# SPDX-License-Identifier: GPL-2.0-or-later
# Execute the real Full cycle and wait functions with slow guest-progress replay.
# No game, profiler, files, UAC or controller input.
Set-StrictMode -Version Latest
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot '../../scripts/super_capture_tools.ps1')
$tokens=$null;$errors=$null
$ast=[Management.Automation.Language.Parser]::ParseFile((Join-Path $PSScriptRoot '../../scripts/collect_super.ps1'),[ref]$tokens,[ref]$errors)
if($errors.Count){throw ($errors.Message -join '; ')}
foreach($name in @('Wait-SuperFramePoint','Invoke-SuperCycle')){
    $definition=$ast.Find({param($node) $node -is [Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -eq $name},$true)
    if(-not $definition){throw ('Missing real collector function: '+$name)}
    Invoke-Expression $definition.Extent.Text
}
$FullCapture=$true;$BaselineSeconds=10;$ReleaseSeconds=30;$PeakSeconds=5
$CpuHotspots=$false;$CriticalCapture=$false;$ComputeTimingStartSeconds=0
$Candidate='none';$CandidateBaseline=$false
$environmentOwner=$null
$fullPlan=@{gpu_capture_backend='nsight';super_capture=@{reference_fps=60;cycle_timeout_seconds=1200;recovery_samples=3;recovery_sample_frames=60}}
$fullControl=@{releases=@();gpu_sequence=$null;gpu_release_sequence=$null;guest_trace_triggered=$false;guest_trace_requests=@()}
$record=@{Path='unused'};$root='unused';$batch=@{cycles=@()};$cycleNumber=0;$currentVariant=@{name='baseline';tuning=@{}}
$script:fakeFrame=1000L;$script:fakeSequence=0;$script:releaseFrame=$null;$script:commands=@();$script:tuning=@();$script:consume=$true
$script:compileAt=$null;$script:compileIncrement=0
function Assert-SuperLive {}
function Set-SuperProfile {}
function Save-SuperFullController {}
function Save-SuperBatch {}
function Write-JsonUtf8 {}
function Write-SuperRounds {}
function Wait-SuperSeconds {}
function Set-DiagnosticTuning($Root,$Path,$Values){$script:tuning+=@{frame=$script:fakeFrame;values=$Values}}
function Send-SuperCommand([string]$Action){
    $script:fakeSequence++
    if($Action -eq 'button'){$script:releaseFrame=$script:fakeFrame}
    $script:commands+=@{action=$Action;frame=$script:fakeFrame}
    return [pscustomobject]@{sequence=$script:fakeSequence;action=$Action;accepted=$true;frame_count=$script:fakeFrame;renderdoc_ready=$false}
}
function Get-SuperSnapshot {
    # 92 frames over a former 30-second interval is still insufficient. The
    # real controller must keep polling rather than starting the next cycle.
    $step=if($null -ne $script:releaseFrame -and $script:fakeFrame -eq $script:releaseFrame){92}else{23}
    $script:fakeFrame+=$step
    $progress=if($null -eq $script:releaseFrame){-1}else{$script:fakeFrame-$script:releaseFrame}
    $dispatches=if($progress -ge 0 -and $progress -lt 2100){1245}else{440}
    if($null -ne $script:compileAt -and $progress -ge $script:compileAt){$script:compileIncrement=1}
    return [pscustomobject]@{frame_count=$script:fakeFrame;sequence=1;karma_fill_requested=1;karma_fill_completed=1
        karma_fill_count=if($progress -ge 0 -and $script:consume){0}else{7}
        shader_compile_starts=(10+$script:compileIncrement);pipeline_compile_starts=(20+$script:compileIncrement);compilations_active=0
        draws_per_frame=7600;dispatches_per_frame=$dispatches}
}
$cycle=Invoke-SuperCycle 'measurement' 'full' 1
if($cycle.status -ne 'collected' -or -not $cycle.meter_consumed -or -not $cycle.recovery.completed){throw 'Slow-frame replay did not complete and recover.'}
if((Get-SuperFrameCount $cycle.before)-(Get-SuperFrameCount $cycle.filled) -lt 600){throw 'Full baseline still advances on wall time.'}
$release=Get-SuperFrameCount $cycle.release
$gpu=@($script:commands|Where-Object {$_.action -eq 'nsight'})
if($gpu.Count -ne 1 -or $gpu[0].frame-$release -lt 300 -or $gpu[0].frame-$release -ge 600){throw 'GPU detail was not anchored to the 300-frame node.'}
if(@($script:tuning|Where-Object {$_.values.ContainsKey('dump_frames') -and $_.frame-$release -ge 600}).Count -ne 1){throw 'Guest dump must wait for its 600-frame node.'}
if((Get-SuperFrameCount $cycle.after)-$release -lt 1800 -or $cycle.recovery.samples.Count -lt 3){throw 'Next release was permitted before the complete frame budget and recovery.'}
if(@($script:commands|Where-Object {$_.action -eq 'karma-fill'}).Count -ne 1 -or @($script:commands|Where-Object {$_.action -eq 'button'}).Count -ne 1){throw 'Recovery must not refill or retry the release.'}
$rejected=$false
try{$null=Wait-SuperFramePoint $cycle.release 1800 ([pscustomobject]@{Elapsed=@{TotalSeconds=1201}})}catch{
    if($_.Exception.Message -notlike '*timed out waiting for guest progress*'){throw};$rejected=$true
}
if(-not $rejected){throw 'A stalled game must terminate at the watchdog, not advance.'}
$script:consume=$false;$script:releaseFrame=$null;$fullControl.gpu_sequence=$null
$rejected=$false
try{$null=Invoke-SuperCycle 'warmup' 'full' 2}catch{
    if($_.Exception.Message -notlike '*no super-meter consumption*'){throw};$rejected=$true
}
if(-not $rejected -or $batch.cycles[-1].status -ne 'failed'){throw 'Frame progression must not bypass actual meter consumption.'}
$script:consume=$true;$script:releaseFrame=$null;$script:compileAt=232;$script:commands=@()
$fullControl.gpu_sequence=$null;$fullControl.gpu_release_sequence=$null;$fullControl.releases=@()
$cycle=Invoke-SuperCycle 'measurement' 'full' 3
if($cycle.status -ne 'warmup-new-compilation' -or $cycle.kind -ne 'warmup' -or
   -not $cycle.capture_suppressed_new_compilation -or $fullControl.phase -ne 'rewarming' -or
   $fullControl.releases.Count -or @($script:commands|Where-Object {$_.action -eq 'nsight'}).Count){
    throw 'A new compile at frame 232 must suppress the one-shot GPU trace and rewarm, preserving the unqualified trial.'
}
$script:releaseFrame=$null;$script:compileAt=400;$script:compileIncrement=0
$fullControl.gpu_sequence=$null;$fullControl.gpu_release_sequence=$null;$fullControl.releases=@()
$rejected=$false
try{$null=Invoke-SuperCycle 'measurement' 'full' 4}catch{
    if($_.Exception.Message -notlike '*one-shot GPU trace cannot be reused as warm*'){throw};$rejected=$true
}
if(-not $rejected -or $batch.cycles[-1].status -ne 'failed'){throw 'New compilation after an SDK trigger must never produce a warm success.'}
$fullPlan.super_capture.warmup_policy='reuse-existing-cache'
$fullControl.gpu_sequence=$null;$fullControl.gpu_release_sequence=$null;$fullControl.releases=@()
$beforeCount=$batch.cycles.Count
foreach($round in 1..3){
    $script:releaseFrame=$null;$script:compileAt=232;$script:compileIncrement=0;$script:commands=@()
    $cycle=Invoke-SuperCycle 'measurement' 'full' $round
    if($cycle.status -ne 'collected' -or $cycle.kind -ne 'measurement' -or $cycle.warm -or $cycle.capture_suppressed_new_compilation){throw 'Cache reuse must preserve a new-compile trial with honest warm=false, without rewarming or suppressing capture.'}
    if(@($script:commands|Where-Object {$_.action -eq 'button'}).Count -ne 1){throw 'Cache reuse must not add release retries.'}
}
if($batch.cycles.Count-$beforeCount -ne 3 -or $fullControl.releases.Count -ne 3 -or -not $fullControl.gpu_sequence){throw 'Cache reuse must record exactly three trials and the GPU detail trigger.'}
Write-Host 'PASS: real Full cycle replay waits for baseline/release frames, 300/600-frame GPU/dump nodes, recovered samples; timeout and absent consumption fail closed. No game started.'
