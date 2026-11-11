# SPDX-License-Identifier: GPL-2.0-or-later
$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest
. (Join-Path $PSScriptRoot '../../scripts/super_capture_tools.ps1')
foreach($candidate in @('feedback-checkpoint','feedback-writers','feedback-word','feedback-async','gds-counter','writer-footprint')){
    $a=Get-SuperFeedbackVariant $candidate -Baseline
    $b=Get-SuperFeedbackVariant $candidate
    if($a.tuning.Count -ne 9 -or $b.tuning.Count -ne 9){throw 'Unexpected candidate flags.'}
    foreach($key in $a.tuning.Keys){if($a.tuning[$key] -ne 0){throw 'Baseline must disable all experimental candidates.'}}
    $selected=@{'feedback-checkpoint'='readback_feedback_checkpoint';'feedback-writers'='readback_feedback_writers';'feedback-word'='readback_word_load';'feedback-async'='asc_queue';'gds-counter'='gds_counter_scope';'writer-footprint'='readback_writer_footprint'}[$candidate]
    foreach($key in $b.tuning.Keys){
        $expected=if($key -eq $selected){1}else{0}
        if($b.tuning[$key] -ne $expected){throw 'Candidate must enable exactly one mechanism.'}
    }
    if($a.name -ne 'A-'+$candidate -or $b.name -ne 'B-'+$candidate){throw 'Variant identity mismatch.'}
}
$rejected=$false
try{$null=Get-SuperFeedbackVariant 'unknown'}catch{$rejected=$true}
if(-not $rejected){throw 'Unknown candidate must be rejected.'}
Write-Host 'Feedback variant isolation checks passed.'
foreach($baseline in @($true,$false)){
    $variant=Get-SuperFeedbackVariant 'feedback-arena-pool' -Baseline:$baseline
    if($variant.tuning.Count -ne 15){throw 'Unexpected feedback pool isolation.'}
    foreach($key in $variant.tuning.Keys){
        $wanted=if($key -eq 'readback_window_kb'){512}else{0}
        if($variant.tuning[$key] -ne $wanted){throw 'Feedback pool changed another renderer setting.'}
    }
    $startup=@(Get-SuperStartupArguments 'feedback-arena-pool' $baseline)
    if(($baseline -and $startup.Count) -or
       (-not $baseline -and ($startup.Count -ne 1 -or $startup[0] -ne '-FeedbackArenaPoolCandidate'))){
        throw 'Feedback pool startup isolation differs.'
    }
}
$before=[pscustomobject]@{readback_window_kb=512;feedback_arena_pool_capacity=2048;feedback_arena_pool_stride=256
    feedback_arena_pool_group_records=2048
    feedback_arena_pool_moves=@(100,100)
    feedback_arena_pool_used=@(200,200);feedback_arena_pool_busy=@(0,0);feedback_arena_pool_overflows=@(0,0)
    feedback_arena_pool_resets=@(10,10)}
$after=[pscustomobject]@{readback_window_kb=512;feedback_arena_pool_capacity=2048;feedback_arena_pool_stride=256
    feedback_arena_pool_group_records=2048
    feedback_arena_pool_moves=@(200,200)
    feedback_arena_pool_used=@(300,300);feedback_arena_pool_busy=@(0,0);feedback_arena_pool_overflows=@(0,0)
    feedback_arena_pool_resets=@(20,20)}
Assert-SuperFeedbackArenaPoolReceipt $before $false
Assert-SuperFeedbackArenaPoolReceipt $after $false $before
$after.feedback_arena_pool_overflows=@(0,1)
$rejected=$false
try{Assert-SuperFeedbackArenaPoolReceipt $after $false $before}catch{$rejected=$true}
if(-not $rejected){throw 'Feedback pool capacity fallback must exclude the trial.'}
Write-Host 'Feedback pool isolation and capacity qualification passed.'
foreach($baseline in @($true,$false)){
    $variant=Get-SuperFeedbackVariant 'feedback-arena-tiled' -Baseline:$baseline
    if($variant.tuning.Count -ne 15){throw 'Tiled pool changed the renderer baseline.'}
    foreach($key in $variant.tuning.Keys){
        $wanted=if($key -eq 'readback_window_kb'){512}else{0}
        if($variant.tuning[$key] -ne $wanted){throw 'Tiled pool enabled another renderer setting.'}
    }
    $startup=@(Get-SuperStartupArguments 'feedback-arena-tiled' $baseline)
    $wanted=if($baseline){'-FeedbackArenaPoolCandidate'}else{'-FeedbackArenaTiledCandidate'}
    if($startup.Count -ne 1 -or $startup[0] -ne $wanted){throw 'Tiled reference must retain the compact pool.'}
}
$after.feedback_arena_pool_overflows=@(0,0)
$before.feedback_arena_pool_group_records=64
$after.feedback_arena_pool_group_records=64
Assert-SuperFeedbackArenaPoolReceipt $before $false $null 64
Assert-SuperFeedbackArenaPoolReceipt $after $false $before 64
$rejected=$false
try{Assert-SuperFeedbackArenaPoolReceipt $after $false $before 2048}catch{$rejected=$true}
if(-not $rejected){throw 'Tiled and compact layouts cannot be confused.'}
Write-Host 'Tiled pool preserves the active compact reference and verifies group geometry.'
foreach($baseline in @($true,$false)){
    $variant=Get-SuperFeedbackVariant 'feedback-arena-pool' -Baseline:$baseline -NormalRendering
    if($variant.tuning.Count -ne 15){throw 'Normal rendering combination changed unrelated settings.'}
    foreach($key in $variant.tuning.Keys){
        $wanted=if($key -eq 'readback_window_kb'){512}elseif($key -eq 'compute_barrier_scope'){1}else{0}
        if($variant.tuning[$key] -ne $wanted){throw 'Normal rendering must keep the same barrier scope in A and B.'}
    }
}
Write-Host 'Normal rendering comparison keeps the ordinary barrier setting in both trials.'
foreach($item in @(@('access-union','-AccessUnionCandidate'),
                   @('buffer-prep-union','-AccessUnionCandidate'),
                   @('readback-copy-stage','-ReadbackCopyStageCandidate'),
                   @('image-read-transition','-ImageReadTransitionCandidate'))){
    foreach($baseline in @($true,$false)){
        $variant=Get-SuperFeedbackVariant $item[0] -Baseline:$baseline
        if($variant.tuning.Count -ne 14){throw 'Unexpected startup candidate flags.'}
        foreach($key in $variant.tuning.Keys){
            $wanted=[int]($item[0] -eq 'buffer-prep-union' -and $key -eq 'written_sync_fastpath' -and -not $baseline)
            if($variant.tuning[$key] -ne $wanted){throw ('Startup candidate flag mismatch: '+$key)}
        }
        $startup=@(Get-SuperStartupArguments $item[0] $baseline)
        if($baseline -and $startup.Count){throw 'Baseline received a candidate startup flag.'}
        if(-not $baseline -and ($startup.Count -ne 1 -or $startup[0] -ne $item[1])){throw 'Candidate startup flag was not forwarded exactly once.'}
    }
}
foreach($baseline in @($true,$false)){
    $variant=Get-SuperFeedbackVariant 'feedback-page-words' -Baseline:$baseline
    if($variant.tuning.Count -ne 12){throw 'Page words must isolate the three needed flags and keep bulk off.'}
    foreach($key in $variant.tuning.Keys){
        $wanted=if($key -in @('readback_word_load','readback_word_prefetch','readback_word_page_cluster')){[int](-not $baseline)}else{0}
        if($variant.tuning[$key] -ne $wanted){throw ('Page words enabled an unrelated setting: '+$key)}
    }
}
foreach($baseline in @($true,$false)){
    $variant=Get-SuperFeedbackVariant 'image-sync-once' -Baseline:$baseline
    if($variant.tuning.Count -ne 11){throw 'Image sync must explicitly disable the written-sync candidate.'}
    foreach($key in $variant.tuning.Keys){
        $wanted=if($key -eq 'image_sync_once'){[int](-not $baseline)}else{0}
        if($variant.tuning[$key] -ne $wanted){throw ('Image sync enabled an unrelated setting: '+$key)}
    }
}
foreach($baseline in @($true,$false)){
    $variant=Get-SuperFeedbackVariant 'written-sync' -Baseline:$baseline
    if($variant.tuning.Count -ne 10){throw 'Written sync candidate must have one additional isolated flag.'}
    foreach($key in $variant.tuning.Keys){
        $wanted=if($key -eq 'written_sync_fastpath'){[int](-not $baseline)}else{0}
        if($variant.tuning[$key] -ne $wanted){throw ('Written sync enabled an unrelated setting: '+$key)}
    }
}
foreach($baseline in @($true,$false)){
    $variant=Get-SuperFeedbackVariant 'compute-recorder' -Baseline:$baseline
    if($variant.tuning.Count -ne 10 -or $variant.tuning.threaded_recording -ne 1){throw 'Unexpected compute recorder isolation.'}
    foreach($key in $variant.tuning.Keys){
        $expected=if($key -eq 'threaded_recording'){1}elseif($key -in @('asc_queue','compute_threaded_recording')){[int](-not $baseline)}else{0}
        if($variant.tuning[$key] -ne $expected){throw ('Unexpected recorder flag: '+$key)}
    }
}
Write-Host 'Compute recorder strategy is isolated against normal baseline.'

$topology=[pscustomobject]@{cores=@(
    [pscustomobject]@{group=0;efficiency_class=1;mask_hex='3'},
    [pscustomobject]@{group=0;efficiency_class=1;mask_hex='c'},
    [pscustomobject]@{group=0;efficiency_class=0;mask_hex='30'})}
$performanceMask=Get-SuperPerformanceCoreMask $topology
if($performanceMask -ne 'f'){throw 'Select the actual highest OS class, not logical numbering or SMT.'}
foreach($baseline in @($true,$false)){
    $variant=Get-SuperFeedbackVariant 'cpu-placement' -Baseline:$baseline -PerformanceCoreMask $performanceMask
    $expected=Get-SuperExpectedTuning $variant.tuning
    if($variant.tuning.Count -ne 12 -or $expected.cpu_affinity -ne 0 -or $expected.other_threads_affinity -ne 0){throw 'CPU candidate must isolate renderer threads.'}
    $affinity=if($baseline){'auto'}else{15}
    if($expected.gpu_threads_affinity -ne $affinity){throw 'INI hex must be normalized to numeric receipt.'}
    foreach($key in @('command_buffer_sharps','compute_barrier_scope','readback_feedback_checkpoint','readback_feedback_writers','readback_word_load','asc_queue','gds_counter_scope')){
        if($expected[$key] -ne 0){throw 'CPU candidate enabled another rendering mechanism.'}
    }
}
$hex=Get-SuperExpectedTuning @{gpu_threads_affinity='ffff'}
if($hex.gpu_threads_affinity -ne 65535){throw 'Never write decimal mask and reparse it as hexadecimal.'}
foreach($invalid in @(
    [pscustomobject]@{cores=@([pscustomobject]@{group=0;efficiency_class=1;mask_hex='f'})},
    [pscustomobject]@{cores=@([pscustomobject]@{group=1;efficiency_class=1;mask_hex='f'},[pscustomobject]@{group=0;efficiency_class=0;mask_hex='f0'})},
    [pscustomobject]@{cores=@([pscustomobject]@{group=0;efficiency_class=1;mask_hex='f'},[pscustomobject]@{group=0;efficiency_class=0;mask_hex='3'})}
)){
    $rejected=$false;try{$null=Get-SuperPerformanceCoreMask $invalid}catch{$rejected=$true}
    if(-not $rejected){throw 'Unsupported or inconsistent topology must fail before launching.'}
}
Write-Host 'CPU placement topology, isolation and hexadecimal receipt contracts passed.'

# Replay both production handoff loops with the real validated script parameters.
# The first poll has no receipt, the next supplies a session object. Neither may
# overwrite the candidate name. No child process, game, files or sleeps are used.
$root=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
$tokens=$null;$errors=$null
$entry=[Management.Automation.Language.Parser]::ParseFile((Join-Path $root 'scripts/collect_super.ps1'),[ref]$tokens,[ref]$errors)
if($errors.Count){throw ($errors.Message -join '; ')}
$startupLoop=$entry.Find({param($node)
    $node -is [Management.Automation.Language.DoWhileStatementAst] -and
    $node.Condition.Extent.Text -eq '-not $session'
},$true)
$recoveryLoop=$entry.Find({param($node)
    $node -is [Management.Automation.Language.WhileStatementAst] -and
    $node.Condition.Extent.Text -match '\$recoveryWait\.Elapsed' -and
    $node.Body.Extent.Text -match 'Read-SuperCaptureSession'
},$true)
if(-not $startupLoop -or -not $recoveryLoop){throw 'Production session handoff loops are missing.'}
$setup=@'
Set-StrictMode -Version Latest
$originalCandidate=$Candidate;$originalBaseline=[bool]$CandidateBaseline
$root='unused';$batchRelative='unused';$session=$null;$record=$null
$batch=@{session=$null};$launcher=[pscustomobject]@{HasExited=$false}
$expected=[pscustomobject]@{Path='unused';Launch=[pscustomobject]@{session='state/sessions/offline-handoff'}}
$poll=@{count=0}
function Assert-SuperLive {}
function Start-Sleep {param([int]$Milliseconds)}
function Read-SuperCaptureSession {
    param([string]$Root,[string]$Batch)
    $poll.count++
    if($poll.count -gt 2){throw 'Handoff did not adopt the published receipt.'}
    if($poll.count -eq 1){return $null}
    return $expected
}
$startup=[Diagnostics.Stopwatch]::StartNew()
$recoveryWait=[Diagnostics.Stopwatch]::StartNew()
'@
$checks=@'
if($poll.count -ne 2 -or $session -ne $expected.Launch.session -or
   -not [object]::ReferenceEquals($record,$expected)){throw 'Handoff did not wait for and adopt the exact session.'}
if($Candidate -ne $originalCandidate -or [bool]$CandidateBaseline -ne $originalBaseline){
    throw 'Session handoff overwrote the validated candidate selection.'
}
'@
foreach($loop in @($startupLoop,$recoveryLoop)){
    $replay=[scriptblock]::Create($entry.ParamBlock.Extent.Text+"`r`n"+$setup+"`r`n"+$loop.Extent.Text+"`r`n"+$checks)
    foreach($choice in @('none','feedback-checkpoint','feedback-writers','feedback-word','feedback-async','gds-counter','cpu-placement','compute-recorder')){
        foreach($isBaseline in @($false,$true)){
            & $replay -Candidate $choice -CandidateBaseline:$isBaseline
        }
    }
}
Write-Host 'Validated candidate survives pending and published session receipts in startup and failure recovery. No game started.'
$tokens=$null;$errors=$null
$launchAst=[Management.Automation.Language.Parser]::ParseFile((Join-Path $PSScriptRoot '../../scripts/launch.ps1'),[ref]$tokens,[ref]$errors)
if($errors.Count){throw ($errors.Message -join '; ')}
$manualGate=$launchAst.Find({param($n) $n -is [Management.Automation.Language.IfStatementAst] -and
    $n.Extent.Text.StartsWith('if($SuperPool -and')},$true)
$selector=$launchAst.Find({param($n) $n -is [Management.Automation.Language.AssignmentStatementAst] -and
    $n.Left.Extent.Text -eq '$useFeedbackPool'},$true)
if(-not $manualGate -or -not $selector){throw 'Missing real launcher manual-test policy.'}
$gateBlock=[scriptblock]::Create($manualGate.Extent.Text)
$selectBlock=[scriptblock]::Create($selector.Extent.Text)
# manual, benchmark, capture, diagnostic, timing, CPU tool, native pool, rejected, enabled
foreach($case in @(@(0,0,0,0,0,0,0,0,0),@(1,0,0,0,0,0,0,0,1),@(0,0,1,0,0,0,1,0,1),
                  @(1,1,0,0,0,0,0,1,1),@(1,0,1,0,0,0,0,1,1),@(1,0,0,1,0,0,0,1,1),
                  @(1,0,0,0,1,0,0,1,1),@(1,0,0,0,0,1,0,1,1))){
    $SuperPool=[bool]$case[0];$Benchmark=[bool]$case[1];$SuperCapture=[bool]$case[2]
    $isDiagnostic=[bool]$case[3];$isTiming=[bool]$case[4];$CpuHotspots=[bool]$case[5]
    $FeedbackArenaPoolCandidate=[bool]$case[6]
    $rejected=$false
    try{& $gateBlock}catch{$rejected=$true}
    if($rejected -ne [bool]$case[7]){throw 'Manual compact-pool option changed a capture or benchmark boundary.'}
    . $selectBlock
    if($useFeedbackPool -ne [bool]$case[8]){throw 'Manual and native compact-pool selection differ.'}
}
Write-Host 'Actual launcher policy keeps ordinary defaults and rejects mixed manual/capture modes. No game started.'
