# SPDX-License-Identifier: GPL-2.0-or-later
# Offline Full/super integration replay. No game, profiler or controller input.
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot '../../scripts/tools.ps1')
. (Join-Path $PSScriptRoot '../../scripts/diagnostic_tools.ps1')
$root=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
$fixture=Resolve-ProjectChild $root ('state/test-super-full-'+[Guid]::NewGuid().ToString('N'))
$relative=Get-RelativePath $root $fixture
function Expect-Rejection([scriptblock]$Action,[string]$Message){
    $rejected=$false
    try{& $Action}catch{if($_.Exception.Message -notlike ('*'+$Message+'*')){throw};$rejected=$true}
    if(-not $rejected){throw ('Expected rejection: '+$Message)}
}
function New-FullPlan {
    return [ordered]@{profile='full';delay_seconds=90;seconds=60;session=$relative
        gpu_capture_backend='nsight';cache_policy='session-private-cold';storage_policy='monitor-only'
        wpr_profile='IFSSConcurrent';cpu_sampling_backend='vtune-hardware';bandwidth_calibration_requested=$true
        collectors=@{wpr=$true;nsight=$true;vtune_analysis='memory-access'}
        automation=@{enabled=$true;boot_timeout_seconds=240;warmup_seconds=20;session_timeout_seconds=900}
        collector_timing=(Get-DiagnosticCollectorTiming 'full' 90 60 240 20 900)
        capture_schedule=(Get-DiagnosticCaptureSchedule 'full' 60)}
}
function Write-Control {Write-JsonUtf8 (Join-Path $fixture 'diagnostics/super-controller.json') $script:control}
function Replay {Update-SuperFullAutomation $root $relative $script:plan $script:auto $script:state 450}
try{
    foreach($directory in @('diagnostics','control-acks','control-queue','user/captures')){$null=[IO.Directory]::CreateDirectory((Join-Path $fixture $directory))}
    if($null -ne (Read-SuperCollectorStatus $fixture)){throw 'A not-yet-published collector status must remain pending.'}
    Write-JsonUtf8 (Join-Path $fixture 'diagnostics/status.json') @{status='waiting';complete=$false}
    if((Read-SuperCollectorStatus $fixture).status -ne 'waiting'){throw 'Published collector status was lost.'}
    $reader=(Get-Item Function:Read-JsonUtf8).ScriptBlock
    try{
        function Read-JsonUtf8 {throw [IO.FileNotFoundException]::new('A concurrent status replacement is pending.')}
        if($null -ne (Read-SuperCollectorStatus $fixture)){throw 'A concurrent replacement gap must remain pending.'}
    }finally{Set-Item Function:Read-JsonUtf8 $reader}
    foreach($experiment in @('prefetch-off','window64','window2048','sampler-off','buffer-sharps','compute-barriers')){
        $names=@(0..3|ForEach-Object{(Get-SuperExperimentVariant $experiment $_).name})
        if($names[0] -ne $names[2] -or $names[1] -ne $names[3] -or $names[0] -eq $names[1]){throw 'Experiment must retain AB ordering.'}
    }
    $expectedSharps=@(0,1,0,1)
    foreach($index in 0..3){
        $variant=Get-SuperExperimentVariant 'compute-barriers' $index
        if($variant.tuning.Count -ne 1 -or $variant.tuning.compute_barrier_scope -ne $expectedSharps[$index]){throw 'Compute candidate must toggle only its own flag in AB order.'}
    }
    foreach($index in 0..3){
        $variant=Get-SuperExperimentVariant 'buffer-sharps' $index
        if($variant.tuning.Count -ne 1 -or $variant.tuning.command_buffer_sharps -ne $expectedSharps[$index]){throw 'Descriptor candidate must toggle only its own flag in AB order.'}
    }
    $paired=@([pscustomobject]@{profile='lightweight';status='collected'},
        [pscustomobject]@{profile='lightweight';status='warmup-new-compilation'},
        [pscustomobject]@{profile='counters';status='collected'})
    Reset-SuperExperimentGroup $paired 'lightweight'
    if($paired[0].status -ne 'excluded-abba-rewarm' -or $paired[0].prior_status -ne 'collected' -or
       $paired[1].status -ne 'warmup-new-compilation' -or $paired[2].status -ne 'collected'){
        throw 'A new compile must invalidate only the active paired experiment, preserving raw qualification.'
    }
    $request=[pscustomobject]@{collector_mode='full';batch='state/super-captures/offline-full-test'
        rounds_per_profile=3;minimum_warmup=5;maximum_warmup=15;baseline_seconds=10;release_seconds=30;peak_seconds=5}
    $plan=New-FullPlan
    Set-SuperFullPlan $plan $request
    if($plan.delay_seconds -ne -1 -or $plan.automation.enabled -or $plan.automation.warmup_seconds -ne 0 -or
       $plan.capture_schedule.anchor -ne 'native-super-release-ack-and-guest-flips' -or $plan.capture_schedule.nsight_frames -ne 300 -or
       $plan.capture_schedule.guest_trace_frames -ne 600 -or $plan.collector_timing.vtune_seconds -le 900 -or
       $plan.collector_timing.nsight_timeout_seconds -le 1000 -or $plan.automation.session_timeout_seconds -le $plan.super_capture.gameplay_timeout_seconds+1800){throw 'Full super plan lost bounded warmup/export coverage or release-based scheduling.'}
    if($plan.wpr_profile -ne 'IFSSConcurrent' -or $plan.collectors.vtune_analysis -ne 'memory-access'){throw 'Super mode must preserve Full collectors.'}
    if($plan.seconds -lt ($request.rounds_per_profile+10)*$plan.super_capture.cycle_timeout_seconds){throw 'Full recording watchdog must cover excluded attempts and rewarm, not only accepted rounds.'}
    $ordinary=New-FullPlan
    if($ordinary.Contains('super_capture') -or $ordinary.capture_schedule.anchor -ne 'gameplay-window-start'){throw 'Ordinary Full changed implicitly.'}
    $plan=($plan|ConvertTo-Json -Depth 18)|ConvertFrom-Json
    $auto=New-DiagnosticAutomation $plan;$auto.events=@()
    $state=[ordered]@{target_pid=123;target_started_utc='2026-10-30T00:00:00Z';scene_confirmed=$false}
    Replay
    if($auto.ready){throw 'Absent controller must not start Full gameplay capture.'}
    $control=[ordered]@{schema=1;session=$relative;batch=$request.batch;pid=123;process_started=$state.target_started_utc
        phase='warming';scene_evidence=@{pause_verified=$true};releases=@();gpu_sequence=$null;guest_trace_triggered=$false}
    Write-Control;Replay
    if($auto.ready){throw 'Warmup must not start Full GPU detail capture.'}
    $cycles=@(1..5|ForEach-Object{[pscustomobject]@{cycle=$_;kind='warmup';warm=$true;meter_consumed=$true;status='warmup-stable';recovery=@{completed=$true}}})
    Write-JsonUtf8 (Join-Path $fixture 'super-rounds.json') $cycles
    $control.phase='recording';Write-Control;Replay
    if(-not $auto.ready -or $auto.workload_completed -or $state.Contains('nsight_trigger_sequence')){throw 'Ready window must start without an idle Nsight trigger or premature completion.'}
    $cycles[-1].warm=$false;Write-JsonUtf8 (Join-Path $fixture 'super-rounds.json') $cycles
    Expect-Rejection {Replay} 'two consecutive'
    $cycles[-1].warm=$true;Write-JsonUtf8 (Join-Path $fixture 'super-rounds.json') $cycles
    $control.phase='rewarming';Write-Control;Replay
    if($auto.phase -ne 'rewarming' -or $auto.workload_completed){throw 'Excluded attempts must return to rewarm without completing or stopping the worker.'}
    $excluded=[pscustomobject]@{cycle=6;kind='warmup';warm=$false;meter_consumed=$true;status='warmup-new-compilation';recovery=@{completed=$true}}
    Write-JsonUtf8 (Join-Path $fixture 'super-rounds.json') @($cycles+$excluded)
    Replay
    $control.phase='recording';Write-Control;Expect-Rejection {Replay} 'two consecutive'
    Write-JsonUtf8 (Join-Path $fixture 'super-rounds.json') @($cycles+$excluded+$cycles[0])
    Expect-Rejection {Replay} 'two consecutive'
    Write-JsonUtf8 (Join-Path $fixture 'super-rounds.json') @($cycles+$excluded+$cycles[0]+$cycles[1])
    Replay
    Write-JsonUtf8 (Join-Path $fixture 'super-rounds.json') $cycles
    $control.phase='recording';Write-Control;Replay
    Expect-Rejection {Assert-SuperFullWarmup $cycles[0..3] 5} 'minimum warmup'
    $control.pid=124;Write-Control;Expect-Rejection {Replay} 'another process'
    $control.pid=123
    foreach($sequence in @(10,20,30)){
        Write-JsonUtf8 (Join-Path $fixture ('control-acks/'+$sequence.ToString('D16')+'.json')) @{
            sequence=$sequence;action='button';accepted=$true;applied_unix_ms=(100000+$sequence*1000);frame_count=($sequence*100)}
        Write-JsonUtf8 (Join-Path $fixture ('control-queue/'+$sequence.ToString('D16')+'.json')) @{
            sequence=$sequence;action='button';button='down'}
    }
    $control.releases=@(10);Write-Control;Replay
    if($auto.inputs.Count -ne 1 -or $auto.inputs[0].button -ne 'down'){throw 'Full workload must record acknowledged super releases.'}
    $gpu=@{sequence=11;action='nsight';accepted=$true;applied_unix_ms=115000;frame_count=1300}
    $gpuPath=Join-Path $fixture 'control-acks/0000000000000011.json'
    Write-JsonUtf8 $gpuPath $gpu
    $control.gpu_sequence=11;Write-Control;Replay
    if(-not $state.nsight_sdk_accepted -or $state.super_detail_progress_frames -ne 300 -or $state.super_detail_release_sequence -ne 10){throw 'Nsight detail must use native release and frame progress receipts.'}
    # The user's failure: long wall time with insufficient game progress must not
    # trigger detail capture; sufficient guest progress can take over 30 seconds.
    $gpu.applied_unix_ms=180000;$gpu.frame_count=1092;Write-JsonUtf8 $gpuPath $gpu;Expect-Rejection {Replay} 'scheduled super guest-frame'
    $gpu.frame_count=1300;Write-JsonUtf8 $gpuPath $gpu;Replay
    if($state.super_detail_delay_ms -ne 70000){throw 'Slow Full progress must not be rejected by the former 30-second wall clock.'}
    $gpu.applied_unix_ms=115000
    $gpu.applied_unix_ms=109000;Write-JsonUtf8 $gpuPath $gpu;Expect-Rejection {Replay} 'preceded'
    $gpu.applied_unix_ms=115000;$gpu.frame_count=2801;Write-JsonUtf8 $gpuPath $gpu;Expect-Rejection {Replay} 'scheduled super'
    $gpu.frame_count=1300
    $gpu.applied_unix_ms=115000;$gpu.accepted=$false;Write-JsonUtf8 $gpuPath $gpu;Expect-Rejection {Replay} 'acknowledgement mismatch'
    $gpu.accepted=$true;Write-JsonUtf8 $gpuPath $gpu
    $control.phase='complete';Write-Control;Expect-Rejection {Replay} 'incomplete or unqualified'
    $control.releases=@(10,20,30);$control.guest_trace_triggered=$true
    $measured=@(1..3|ForEach-Object{
        $begin=[pscustomobject]@{shader_compile_starts=10;pipeline_compile_starts=20;compilations_active=0;observed_unix_ms=(100000+$_*10000)}
        $end=[pscustomobject]@{shader_compile_starts=10;pipeline_compile_starts=20;compilations_active=0;observed_unix_ms=(105000+$_*10000)}
        [pscustomobject]@{cycle=(5+$_);kind='measurement';warm=$true;meter_consumed=$true;status='collected';release=@{sequence=($_*10)}
            recovery=@{completed=$true};filled=$begin;before=$begin;after=$end}
    })
    Write-JsonUtf8 (Join-Path $fixture 'super-rounds.json') @($cycles+$measured)
    Write-Control;Replay
    if(-not $auto.workload_completed -or $auto.inputs.Count -ne 3 -or -not $auto.xray_triggered){throw 'Complete, warm, applied Full super workload failed to finish.'}
    if($state.super_qualified_windows.Count -ne 3 -or $state.super_qualified_windows[0].release_sequence -ne 10){throw 'Accepted CPU report intervals were lost.'}
    $measured[-1].recovery.completed=$false;Write-JsonUtf8 (Join-Path $fixture 'super-rounds.json') @($cycles+$measured)
    Expect-Rejection {Replay} 'incomplete or unqualified'
    $measured[-1].recovery.completed=$true;Write-JsonUtf8 (Join-Path $fixture 'super-rounds.json') @($cycles+$measured)
    $control.releases=@(10,20,20);Write-Control;Expect-Rejection {Replay} 'reused a release'
    $control.releases=@(10,30,20);Write-Control;Expect-Rejection {Replay} 'acknowledgement differ'
    $control.releases=@(10,20,30);Write-Control
    $measured[-1].warm=$false;Write-JsonUtf8 (Join-Path $fixture 'super-rounds.json') @($cycles+$measured)
    Expect-Rejection {Replay} 'incomplete or unqualified'
    # Replay the alternate GPU backend against the same real receipt checks.
    $control.phase='recording';$control.releases=@(10);$plan.gpu_capture_backend='renderdoc'
    $gpu.action='capture';$gpu.renderdoc_ready=$true;Write-JsonUtf8 $gpuPath $gpu
    Write-Control;Replay
    if(-not $auto.capture_triggered -or $auto.capture_completed){throw 'RenderDoc trigger acceptance must not be confused with a completed frame file.'}
    # New policy has no warmup releases; cold trials remain captured, not warm-qualified.
    $request|Add-Member NoteProperty warmup_policy 'reuse-existing-cache'
    $request.minimum_warmup=0;$request.maximum_warmup=0
    $newPlan=New-FullPlan;Set-SuperFullPlan $newPlan $request
    if(-not(Test-SuperReuseCachePlan $newPlan) -or $newPlan.seconds -ge 4*$newPlan.super_capture.cycle_timeout_seconds){throw 'Cache-reuse plan still budgets extra warmup releases.'}
    $plan=($newPlan|ConvertTo-Json -Depth 18)|ConvertFrom-Json
    $auto=New-DiagnosticAutomation $plan;$auto.events=@()
    $control.phase='recording';$control.releases=@();$control.gpu_sequence=$null
    Write-JsonUtf8 (Join-Path $fixture 'super-rounds.json') @()
    Write-Control;Replay
    if(-not $auto.ready){throw 'Cache-reuse Full must become ready without a warmup release.'}
    $control.phase='complete';$control.releases=@(10,20,30);$control.gpu_sequence=11
    $gpu.action='nsight';Write-JsonUtf8 $gpuPath $gpu
    $measured[-1].after.pipeline_compile_starts++
    Write-JsonUtf8 (Join-Path $fixture 'super-rounds.json') @($measured)
    Write-Control;Replay
    if(-not $auto.workload_completed -or $state.super_qualified_windows.Count -ne 3 -or $state.super_qualified_windows[-1].warm -or $state.super_excluded_cycles -notcontains $measured[-1].cycle){throw 'Cache-reuse completion must preserve all three CPU windows and mark the cold trial.'}
    $request|Add-Member NoteProperty vtune_cpu_mode 'stable'
    Expect-Rejection {Set-SuperFullPlan (New-FullPlan) $request} 'Full requires hardware-memory'
    $request.vtune_cpu_mode='hardware-memory'
    $hardwarePlan=New-FullPlan;Set-SuperFullPlan $hardwarePlan $request
    $hardwareSpec=Get-DiagnosticPlanSpec $hardwarePlan
    $knobs=@(Get-DiagnosticVtuneKnobs $hardwarePlan $hardwareSpec.VtuneAnalysis)
    if($hardwareSpec.VtuneAnalysis -ne 'memory-access' -or $hardwarePlan.cpu_sampling_backend -ne 'vtune-hardware' -or
       $knobs -notcontains 'dram-bandwidth-limits=true' -or -not $hardwarePlan.bandwidth_calibration_requested){throw 'Full must retain hardware events and DRAM calibration.'}
    if((Get-DiagnosticPlanSpec (New-FullPlan)).VtuneAnalysis -ne 'memory-access'){throw 'Original Full hardware memory analysis must remain unchanged.'}
    # A single round is one release and one CPU interval, with a JSON array even
    # when empty or containing only one record (PowerShell 5 pipeline edge case).
    $request.rounds_per_profile=1
    $single=New-FullPlan;Set-SuperFullPlan $single $request
    if($single.super_capture.rounds -ne 1 -or $single.seconds -ge 2*$single.super_capture.cycle_timeout_seconds){throw 'A single Full round was expanded.'}
    $plan=($single|ConvertTo-Json -Depth 18)|ConvertFrom-Json
    $auto=New-DiagnosticAutomation $plan;$auto.events=@()
    Write-SuperRounds (Join-Path $fixture 'super-rounds.json') @()
    if([IO.File]::ReadAllText((Join-Path $fixture 'super-rounds.json')) -notmatch '^\s*\[\s*\]\s*$'){throw 'Empty rounds must serialize as an array.'}
    Write-SuperRounds (Join-Path $fixture 'super-rounds.json') @($measured[0])
    if(-not [IO.File]::ReadAllText((Join-Path $fixture 'super-rounds.json')).TrimStart().StartsWith('[')){throw 'A single round must remain a JSON array for the offline parser.'}
    $control.phase='complete';$control.releases=@(10);Write-Control;Replay
    if(-not $auto.workload_completed -or $auto.inputs.Count -ne 1 -or $state.super_qualified_windows.Count -ne 1){throw 'Single Full release did not produce exactly one completed CPU window.'}
    foreach($name in @('collect_super.ps1','super_capture_tools.ps1','diagnostic_tools.ps1','launch.ps1')){
        $tokens=$null;$errors=$null
        $null=[Management.Automation.Language.Parser]::ParseFile((Join-Path $root ('scripts/'+$name)),[ref]$tokens,[ref]$errors)
        if($errors.Count){throw ($name+': '+($errors.Message -join '; '))}
    }
    Write-Host 'PASS: Full plan preservation; warmup/identity gates; release-anchored Nsight and RenderDoc receipts; bounded timing; no cold/partial success. No game started.'
}finally{
    $absolute=[IO.Path]::GetFullPath($fixture)
    $allowed=[IO.Path]::GetFullPath((Join-Path $root 'state')).TrimEnd('\')+'\'
    if(-not $absolute.StartsWith($allowed,[StringComparison]::OrdinalIgnoreCase) -or [IO.Path]::GetFileName($absolute) -notmatch '^test-super-full-[a-f0-9]{32}$'){throw 'Refusing cleanup outside the Full super fixture.'}
    if(Test-Path -LiteralPath $absolute){Remove-Item -LiteralPath $absolute -Recurse -Force}
}
