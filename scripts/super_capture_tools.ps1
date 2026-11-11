# SPDX-License-Identifier: GPL-2.0-or-later
# Qualification and batch handoff helpers. Importing starts no game, tool or input.
function Write-SuperRounds([string]$Path,[object[]]$Cycles) {
    # Explicit input preserves [] and [one round] for the Python content checker.
    # Keep this capture-only change outside the engine's build-input fingerprint.
    $root=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
    Assert-ProjectPath $root $Path
    $pending=$Path+'.'+[Guid]::NewGuid().ToString('N')+'.pending'
    try{
        [IO.File]::WriteAllText($pending,(ConvertTo-Json -InputObject @($Cycles) -Depth 18),[Text.UTF8Encoding]::new($false))
        Publish-ProjectFile $pending $Path
    }finally{
        if(Test-Path -LiteralPath $pending){Remove-Item -LiteralPath $pending -Force}
    }
}
function Get-SuperElevatedArguments([string]$Entry,[Collections.IDictionary]$BoundParameters) {
    $arguments=[Collections.Generic.List[string]]::new()
    foreach($value in @('-NoLogo','-NoProfile','-ExecutionPolicy','Bypass','-File',$Entry,'-FullCapture')){$arguments.Add($value)}
    foreach($key in $BoundParameters.Keys){
        if($key -eq 'FullCapture'){continue}
        if($key -notin @('Mode','Profile','Rounds','MinimumWarmup','MaximumWarmup','BaselineSeconds','ReleaseSeconds','PeakSeconds','VtuneCpuMode')){
            throw ('Unsupported Full elevation parameter: '+$key)
        }
        # Keep name and value as two arguments. In Windows PowerShell the comma
        # expression @('-'+$key,$value) concatenates the array into one string.
        $arguments.Add('-'+$key)
        $arguments.Add([string]$BoundParameters[$key])
    }
    return $arguments.ToArray()
}
function Read-SuperCollectorStatus([string]$SessionPath) {
    $path=Join-Path $SessionPath 'diagnostics/status.json'
    # File.Replace can briefly make an independently opened path unavailable on
    # Windows. Missing startup/replace snapshots are pending, not collector failure.
    try{return Read-JsonUtf8 $path}catch{
        $cause=$_.Exception
        while($cause.InnerException){$cause=$cause.InnerException}
        if($cause -is [IO.IOException] -and ($cause.HResult -band 0xffff) -in @(2,3,32,33)){return $null}
        throw
    }
}
function Get-SuperCriticalTraceErrors($Metadata,[switch]$RequireDrawPrep) {
    $errors=[Collections.Generic.List[string]]::new()
    if(-not $Metadata){$errors.Add('Critical trace metadata missing.');return $errors.ToArray()}
    if(-not $Metadata.complete){$errors.Add('Critical trace is incomplete.')}
    foreach($key in @('dropped','cutoff_publishers','io_failed')){
        if(-not $Metadata.PSObject.Properties[$key] -or $Metadata.$key){$errors.Add('Critical trace integrity failure: '+$key)}
    }
    if(-not $Metadata.PSObject.Properties['events'] -or $Metadata.events -le 0){$errors.Add('Critical trace has no events.')}
    if($RequireDrawPrep){
        if(-not $Metadata.PSObject.Properties['draw_prep_audit'] -or -not $Metadata.draw_prep_audit -or
           -not $Metadata.PSObject.Properties['draw_prep_protocol'] -or $Metadata.draw_prep_protocol -ne 2 -or
           -not $Metadata.PSObject.Properties['draw_prep_sample_period'] -or $Metadata.draw_prep_sample_period -ne 256 -or
           -not $Metadata.PSObject.Properties['draw_prep_sample_cap'] -or $Metadata.draw_prep_sample_cap -ne 65536 -or
           -not $Metadata.PSObject.Properties['draw_prep_cap_hit'] -or $Metadata.draw_prep_cap_hit){
            $errors.Add('Draw preparation audit is missing or its sample budget was exhausted.')
        }
    }
    return $errors.ToArray()
}
function Get-SuperFeedbackVariant([string]$Candidate,[switch]$Baseline,[string]$PerformanceCoreMask='',
                                 [switch]$NormalRendering) {
    if($NormalRendering -and $Candidate -ne 'feedback-arena-pool'){throw 'Normal rendering comparison is restricted to the compact pool.'}
    if($Candidate -notin @('feedback-checkpoint','feedback-writers','feedback-word','feedback-async','gds-counter','cpu-placement','compute-recorder','recorder-spin','reg-dump-fast','feedback-word-prefetch','feedback-word-bulk','particle-poll-pause','c73-scan','written-sync','image-sync-once','feedback-page-words','access-union','readback-copy-stage','image-read-transition','buffer-prep-union','feedback-arena','vertex-input-key','feedback-arena-group','feedback-arena-pool','feedback-arena-tiled','early-complete','writer-footprint')){throw 'Unknown feedback candidate.'}
    $tuning=@{command_buffer_sharps=0;compute_barrier_scope=0
        readback_feedback_checkpoint=0;readback_feedback_writers=0;readback_word_load=0;readback_early_complete=0;readback_writer_footprint=0;asc_queue=0;gds_counter_scope=0;compute_threaded_recording=0}
    if($Candidate -eq 'cpu-placement'){
        if($PerformanceCoreMask -notmatch '^[0-9a-fA-F]{1,16}$'){throw 'CPU placement requires an enumerated performance-core mask.'}
        $mask=[Convert]::ToUInt64($PerformanceCoreMask,16)
        if($mask -eq 0 -or $mask -eq [UInt64]::MaxValue){throw 'Invalid performance-core mask.'}
        # INI affinity is hexadecimal, while the status receipt contains a number.
        # Preserve the exact INI spelling separately from normalized receipt values.
        $tuning.gpu_threads_affinity=if($Baseline){'auto'}else{$PerformanceCoreMask}
        $tuning.cpu_affinity='0';$tuning.other_threads_affinity='0'
    }elseif($Candidate -in @('feedback-word-prefetch','feedback-word-bulk','feedback-page-words')){
        $tuning.readback_word_load=[int](-not $Baseline)
        $tuning.readback_word_prefetch=[int](-not $Baseline)
        if($Candidate -eq 'feedback-word-bulk'){$tuning.readback_word_prefetch_bulk=[int](-not $Baseline)}
        if($Candidate -eq 'feedback-page-words'){
            $tuning.readback_word_prefetch_bulk=0
            $tuning.readback_word_page_cluster=[int](-not $Baseline)
        }
    }elseif($Candidate -in @('access-union','readback-copy-stage','image-read-transition','buffer-prep-union','feedback-arena','vertex-input-key','feedback-arena-group','feedback-arena-pool','feedback-arena-tiled')){
        $tuning.written_sync_fastpath=[int]($Candidate -eq 'buffer-prep-union' -and -not $Baseline)
        $tuning.image_sync_once=0
        $tuning.readback_word_prefetch=0;$tuning.readback_word_prefetch_bulk=0;$tuning.readback_word_page_cluster=0
        if($Candidate -in @('feedback-arena','feedback-arena-group','feedback-arena-pool','feedback-arena-tiled')){$tuning.readback_window_kb=512}
    }elseif($Candidate -eq 'image-sync-once'){
        $tuning.written_sync_fastpath=0
        $tuning.image_sync_once=[int](-not $Baseline)
    }elseif($Candidate -eq 'written-sync'){
        $tuning.written_sync_fastpath=[int](-not $Baseline)
    }elseif($Candidate -eq 'early-complete'){
        $tuning.readback_early_complete=[int](-not $Baseline)
    }elseif($Candidate -eq 'writer-footprint'){
        # O02 switch only; default-off. Does not enable feedback-writers or Draft A/B logic.
        $tuning.readback_writer_footprint=[int](-not $Baseline)
    }elseif($Candidate -eq 'particle-poll-pause'){
        # Selected at process launch, no live tuning.
    }elseif($Candidate -eq 'reg-dump-fast'){
        $tuning.reg_dump_fast_empty=[int](-not $Baseline)
    }elseif($Candidate -eq 'recorder-spin'){
        $tuning.threaded_recording=1
        $tuning.recorder_spin_us=if($Baseline){200}else{0}
    }elseif($Candidate -eq 'compute-recorder'){
        # Compare the complete new strategy with the actual normal baseline, not the failed
        # bare async-queue candidate. No other rendering candidates or affinity changes.
        $tuning.threaded_recording=1
        if(-not $Baseline){$tuning.asc_queue=1;$tuning.compute_threaded_recording=1}
    }elseif(-not $Baseline -and $Candidate -ne 'c73-scan'){
        if($Candidate -eq 'feedback-checkpoint'){$tuning.readback_feedback_checkpoint=1}
        elseif($Candidate -eq 'feedback-writers'){$tuning.readback_feedback_writers=1}
        elseif($Candidate -eq 'feedback-word'){$tuning.readback_word_load=1}
        elseif($Candidate -eq 'feedback-async'){$tuning.asc_queue=1}
        else{$tuning.gds_counter_scope=1}
    }
    if($NormalRendering){$tuning.compute_barrier_scope=1}
    return @{name=$(if($Baseline){'A-'}else{'B-'})+$Candidate;tuning=$tuning}
}
function Get-SuperStartupArguments([string]$Candidate,[bool]$Baseline) {
    if($Candidate -eq 'feedback-arena-tiled'){
        if($Baseline){return @('-FeedbackArenaPoolCandidate')}else{return @('-FeedbackArenaTiledCandidate')}
    }
    if($Baseline){return @()}
    if($Candidate -eq 'c73-scan'){return @('-C73ScanCandidate')}
    if($Candidate -eq 'particle-poll-pause'){return @('-ParticlePollPauseCandidate')}
    if($Candidate -in @('access-union','buffer-prep-union')){return @('-AccessUnionCandidate')}
    if($Candidate -eq 'readback-copy-stage'){return @('-ReadbackCopyStageCandidate')}
    if($Candidate -eq 'image-read-transition'){return @('-ImageReadTransitionCandidate')}
    if($Candidate -eq 'feedback-arena'){return @('-FeedbackArenaCandidate')}
    if($Candidate -eq 'feedback-arena-group'){return @('-FeedbackArenaGroupCandidate')}
    if($Candidate -eq 'feedback-arena-pool'){return @('-FeedbackArenaPoolCandidate')}
    if($Candidate -eq 'vertex-input-key'){return @('-VertexInputKeyCandidate')}
    return @()
}
function Assert-SuperFeedbackArenaPoolReceipt($Snapshot,[bool]$Baseline,$Before=$null,[int]$GroupRecords=2048) {
    if($Snapshot.readback_window_kb -ne 512 -or $Snapshot.feedback_arena_pool_capacity -ne 2048 -or
       $Snapshot.feedback_arena_pool_stride -ne 256 -or
       ($Snapshot.feedback_arena_pool_group_records -ne $(if($Baseline){0}else{$GroupRecords}))){
        throw 'Feedback pool capacity, stride or actual group layout differs.'
    }
    foreach($name in @('moves','overflows','busy','used','resets')){
        $values=@($Snapshot.('feedback_arena_pool_'+$name))
        if($values.Count -ne 2){throw 'Feedback pool requires counters for both original sides.'}
        foreach($value in $values){
            if($value -lt 0 -or ($Baseline -and $value -ne 0)){throw 'Invalid feedback pool receipt.'}
        }
    }
    if(-not $Baseline){
        for($side=0;$side -lt 2;$side++){
            if($Snapshot.feedback_arena_pool_overflows[$side] -ne 0 -or
               $Snapshot.feedback_arena_pool_used[$side] -notin 1..2048 -or
               $Snapshot.feedback_arena_pool_moves[$side] -le 0 -or
               $Snapshot.feedback_arena_pool_resets[$side] -le 0 -or
               ($Before -and ($Snapshot.feedback_arena_pool_moves[$side] -le $Before.feedback_arena_pool_moves[$side] -or
                             $Snapshot.feedback_arena_pool_resets[$side] -le $Before.feedback_arena_pool_resets[$side]))){
                throw 'Feedback pool did not cover both sides without capacity fallback.'
            }
        }
    }
}
function Assert-SuperFeedbackArenaGroupReceipt($Snapshot,[bool]$Baseline,$Before=$null) {
    $moves=@($Snapshot.feedback_arena_group_moves)
    if($Snapshot.readback_window_kb -ne 512 -or $moves.Count -ne 32){
        throw 'Feedback group requires 32 counters and a 512 KiB readback window.'
    }
    foreach($value in $moves){
        if($value -lt 0 -or ($Baseline -and $value -ne 0)){throw 'Invalid feedback group counters.'}
    }
    if(-not $Baseline){
        foreach($slot in @(4,6,7,20,22,23)){
            if($moves[$slot] -le 0 -or ($Before -and $moves[$slot] -le $Before.feedback_arena_group_moves[$slot])){
                throw 'The three observed records on each side did not enter feedback group isolation.'
            }
        }
    }
}
function Assert-SuperFeedbackArenaReceipt($Snapshot,[bool]$Baseline,$Before=$null) {
    $moves=$Snapshot.feedback_arena_moves
    if($Snapshot.readback_window_kb -ne 512 -or @($moves).Count -ne 2){
        throw 'The feedback arena receipt or 512 KiB readback configuration is missing.'
    }
    for($side=0;$side -lt 2;$side++){
        if($Baseline){
            if($moves[$side] -ne 0){throw 'The baseline moved a feedback record.'}
        }elseif($moves[$side] -le 0 -or ($null -ne $Before -and $moves[$side] -le $Before.feedback_arena_moves[$side])){
            throw "Feedback record relocation did not cover render side $side."
        }
    }
}
function Get-SuperExpectedTuning([Collections.IDictionary]$Tuning) {
    $expected=@{}
    foreach($key in $Tuning.Keys){
        $value=$Tuning[$key]
        if($key -in @('gpu_threads_affinity','cpu_affinity','other_threads_affinity')){
            $expected[$key]=if($key -eq 'gpu_threads_affinity' -and $value -eq 'auto'){'auto'}else{[Convert]::ToUInt64([string]$value,16)}
        }else{$expected[$key]=$value}
    }
    return $expected
}
function Get-SuperPerformanceCoreMask($Topology) {
    $cores=@($Topology.cores)
    if(-not $cores.Count -or @($cores|Where-Object {$_.group -ne 0}).Count){throw 'CPU placement supports one processor group only.'}
    $classes=@($cores.efficiency_class|Sort-Object -Unique)
    if($classes.Count -lt 2){throw 'No distinct performance/efficiency core classes were reported.'}
    $highest=($classes|Measure-Object -Maximum).Maximum
    [UInt64]$all=0;[UInt64]$performance=0
    foreach($core in $cores){
        [UInt64]$mask=[Convert]::ToUInt64($core.mask_hex,16)
        if($mask -eq 0 -or ($all -band $mask) -ne 0){throw 'Invalid or overlapping core topology masks.'}
        $all=$all -bor $mask
        if($core.efficiency_class -eq $highest){$performance=$performance -bor $mask}
    }
    if($performance -eq 0 -or $performance -eq $all){throw 'Performance cores must form a proper nonempty subset.'}
    return ('{0:x}' -f $performance)
}
function Get-SuperExperimentVariant([string]$Experiment,[int]$Accepted) {
    # One pair is A off, then B on. Extra pairs require explicit round counts.
    $candidate=($Accepted % 2) -eq 1
    if($Experiment -eq 'prefetch-off'){
        return @{name=if($candidate){'B-prefetch-off'}else{'A-prefetch-on'};tuning=@{readback_prefetch=[int](-not $candidate)}}
    }
    if($Experiment -eq 'window64'){
        return @{name=if($candidate){'B-window64'}else{'A-window512'};tuning=@{readback_window_kb=if($candidate){64}else{512}}}
    }
    if($Experiment -eq 'window2048'){
        return @{name=if($candidate){'B-window2048'}else{'A-window512'};tuning=@{readback_window_kb=if($candidate){2048}else{512}}}
    }
    if($Experiment -eq 'compute-barriers'){
        return @{name=if($candidate){'B-compute-barriers'}else{'A-barrier-baseline'};tuning=@{compute_barrier_scope=[int]$candidate}}
    }
    if($Experiment -eq 'buffer-sharps'){
        return @{name=if($candidate){'B-buffer-sharps'}else{'A-buffer-baseline'};tuning=@{command_buffer_sharps=[int]$candidate}}
    }
    if($Experiment -eq 'sampler-off'){
        return @{name=if($candidate){'B-sampler-off'}else{'A-sampler-on'};tuning=@{perf_log=[int](-not $candidate)}}
    }
    return @{name='baseline';tuning=@{}}
}
function Reset-SuperExperimentGroup($Cycles,[string]$Profile) {
    foreach($cycle in $Cycles){
        if($cycle.profile -eq $Profile -and $cycle.status -eq 'collected'){
            $cycle | Add-Member NoteProperty prior_status 'collected' -Force
            $cycle | Add-Member NoteProperty exclusion_reason 'A later trial compiled new shaders; restart the paired experiment after warmup.' -Force
            $cycle.status='excluded-abba-rewarm'
        }
    }
}
function Get-SuperFullEntryErrors($ExitCode,$Index,[DateTime]$StartedUtc) {
    $errors=[Collections.Generic.List[string]]::new()
    if($null -eq $ExitCode){$errors.Add('Full elevated process exit code is unavailable.')}
    elseif($ExitCode -ne 0){$errors.Add('Full elevated process returned exit code '+$ExitCode+'.')}
    if($null -eq $Index){$errors.Add('Full did not publish a collection batch; startup did not complete.');return $errors.ToArray()}
    foreach($field in @('updated_utc','batch','status','error')){
        if(-not $Index.PSObject.Properties[$field]){$errors.Add('Full result index is missing '+$field+'.');return $errors.ToArray()}
    }
    if([DateTime]::Parse($Index.updated_utc).ToUniversalTime() -lt $StartedUtc.ToUniversalTime()){
        $errors.Add('No new Full collection batch was published; the existing index belongs to an earlier run.')
    }elseif($Index.status -ne 'collected'){
        $errors.Add('Full collection status: '+$Index.status+'. '+$Index.error)
    }
    return $errors.ToArray()
}
function Resolve-SuperCaptureBatch([string]$Root,[string]$Batch) {
    if($Batch -notmatch '^state/super-captures/[a-zA-Z0-9-]+$'){throw 'Untrusted super capture batch path.'}
    return Resolve-ProjectChild $Root $Batch
}
function Assert-SuperCaptureStarting([string]$Root,[string]$Batch) {
    $path=Resolve-SuperCaptureBatch $Root $Batch
    $request=Read-JsonUtf8 (Join-Path $path 'capture.json')
    if($request.schema -ne 1 -or $request.batch -ne $Batch -or $request.status -ne 'starting'){
        throw 'Super capture batch is no longer waiting for game startup; launch cancelled.'
    }
}
function Publish-SuperCaptureSession([string]$Root,[string]$Batch,$Launch) {
    $path=Resolve-SuperCaptureBatch $Root $Batch
    # Publish even if cancellation raced with Process.Start, so the owner can quit
    # this exact process. The full launch record has already been published.
    Write-JsonUtf8 (Join-Path $path 'session.json') @{
        schema=1;batch=$Batch;session=$Launch.session;pid=$Launch.pid
        process_started=$Launch.process_started
    }
}
function Read-SuperCaptureSession([string]$Root,[string]$Batch) {
    $path=Join-Path (Resolve-SuperCaptureBatch $Root $Batch) 'session.json'
    if(-not(Test-Path -LiteralPath $path -PathType Leaf)){return $null}
    $receipt=Read-JsonUtf8 $path
    if($receipt.schema -ne 1 -or $receipt.batch -ne $Batch -or
        $receipt.session -notmatch '^state/sessions/[a-zA-Z0-9-]+$'){
        throw 'Super capture session receipt does not belong to this batch.'
    }
    # Do not consult state/active.json: it can still name any older build/run.
    # Preserve the pinned-engine check for the batch's actual new session.
    $candidate=Read-ProjectSession $Root $receipt.session
    $launch=$candidate.Launch
    if(-not $launch.PSObject.Properties['super_capture_batch'] -or
        -not $launch.super_capture -or $launch.super_capture_batch -ne $Batch -or
        -not $launch.game_launched -or $launch.pid -ne $receipt.pid -or
        $launch.process_started -ne $receipt.process_started){
        throw 'Super capture launch identity does not match its batch receipt.'
    }
    if($launch.complete){return $null}
    return $candidate
}
function Get-SuperFinalizationErrors($LauncherExitCode,$Launch,[string]$Batch,[string]$Session,$Collector=$null) {
    $errors=[Collections.Generic.List[string]]::new()
    $collectorErrors=[Collections.Generic.List[string]]::new()
    if($Collector -and $Collector.PSObject.Properties['errors']){
        foreach($detail in @($Collector.errors)){ if($detail){ $collectorErrors.Add([string]$detail) } }
    }
    if($null -eq $LauncherExitCode){$errors.Add('Diagnostic host exit code is unavailable; completion cannot be confirmed from its process handle.')}
    elseif($LauncherExitCode -ne 0){
        if($collectorErrors.Count){
            # Host exit 1 after artifact/VTune failure is cascade, not a game/launcher crash.
            $errors.Add('Diagnostic host exited '+$LauncherExitCode+' after collector/artifact failure (cascade; not a game crash). See collector errors.')
        }else{
            $errors.Add('Launcher returned exit code '+$LauncherExitCode+'; see launcher-error.log.')
        }
    }
    foreach($detail in $collectorErrors){ $errors.Add('Full collector: '+$detail) }
    if($null -eq $Launch){$errors.Add('The final launch record is missing.');return $errors.ToArray()}
    foreach($name in @('schema','path_base','session','super_capture','super_capture_batch','complete','exit_code','progress_published','source_game_written')){
        if(-not $Launch.PSObject.Properties[$name]){$errors.Add('Final launch record is missing '+$name+'.')}
    }
    if($errors.Count -and @($errors|Where-Object {$_ -like 'Final launch record is missing *'}).Count){return $errors.ToArray()}
    if($Launch.schema -ne 3 -or $Launch.path_base -ne 'project' -or $Launch.session -ne $Session -or
        $Launch.super_capture -ne $true -or $Launch.super_capture_batch -ne $Batch){
        $errors.Add('The final launch record does not belong to this collection-only batch.')
    }
    if($Launch.complete -ne $true){$errors.Add('The launcher has not published a completed launch record.')}
    if($null -eq $Launch.exit_code){$errors.Add('Game exit code is unavailable.')}
    elseif($Launch.exit_code -ne 0){$errors.Add('Game returned exit code '+$Launch.exit_code+'.')}
    if($Launch.progress_published -ne $false){$errors.Add('Save isolation failed: session progress was published.')}
    if($Launch.source_game_written -ne $false){$errors.Add('Game source preservation was not confirmed.')}
    return $errors.ToArray()
}
function Test-SuperCompileSnapshot($Snapshot) {
    foreach($name in @('shader_compile_starts','pipeline_compile_starts','compilations_active')){
        if($null -eq $Snapshot -or -not $Snapshot.PSObject.Properties[$name]){return $false}
        $number=0L
        if(-not [long]::TryParse([string]$Snapshot.$name,[ref]$number) -or $number -lt 0){return $false}
    }
    return $Snapshot.compilations_active -eq 0
}
function Test-SuperWarmCycle($Before,[object[]]$Snapshots,$After) {
    if(-not(Test-SuperCompileSnapshot $Before) -or -not(Test-SuperCompileSnapshot $After) -or -not $Snapshots.Count){return $false}
    foreach($sample in @($Snapshots)+@($After)){
        if(-not(Test-SuperCompileSnapshot $sample) -or
            $sample.shader_compile_starts -ne $Before.shader_compile_starts -or
            $sample.pipeline_compile_starts -ne $Before.pipeline_compile_starts){return $false}
    }
    return $true
}
function Test-SuperMeterConsumed([long]$Filled,[object[]]$Snapshots) {
    if($Filled -le 0 -or $Filled -gt 100000){return $false}
    foreach($sample in $Snapshots){
        if($sample.PSObject.Properties['karma_fill_count'] -and
            $sample.karma_fill_completed -eq $sample.karma_fill_requested -and
            $sample.karma_fill_count -lt $Filled){return $true}
    }
    return $false
}
function Get-SuperFrameCount($Snapshot) {
    $count=0L
    if($null -eq $Snapshot -or -not $Snapshot.PSObject.Properties['frame_count'] -or
       -not [long]::TryParse([string]$Snapshot.frame_count,[ref]$count) -or $count -lt 0){
        throw 'Full super capture requires native guest-flip progress telemetry; rebuild the current engine.'
    }
    return $count
}
function Test-SuperFrameProgress($Anchor,$Snapshot,[long]$Required) {
    $first=Get-SuperFrameCount $Anchor;$current=Get-SuperFrameCount $Snapshot
    if($Required -lt 0 -or $current -lt $first){throw 'Invalid or regressed Full guest-flip progress.'}
    return ($current-$first) -ge $Required
}
function Test-SuperRecoverySnapshot($Before,$Snapshot) {
    # A recovery heuristic, not a guest animation-state flag. Combine it with a
    # complete frame budget, and retain the screenshots and counters for review.
    if(-not(Test-SuperCompileSnapshot $Snapshot)){return $false}
    foreach($sample in @($Before,$Snapshot)){
        if(-not $sample.PSObject.Properties['dispatches_per_frame'] -or
           -not $sample.PSObject.Properties['draws_per_frame'] -or
           $sample.dispatches_per_frame -le 0 -or $sample.draws_per_frame -le 0){return $false}
    }
    return $Snapshot.dispatches_per_frame -le [Math]::Max($Before.dispatches_per_frame*1.25,($Before.dispatches_per_frame+50))
}
function Test-SuperReuseCachePlan($Plan) {
    if($Plan.super_capture -is [Collections.IDictionary]){return $Plan.super_capture.Contains('warmup_policy') -and $Plan.super_capture.warmup_policy -eq 'reuse-existing-cache'}
    return $null -ne $Plan.super_capture.PSObject.Properties['warmup_policy'] -and $Plan.super_capture.warmup_policy -eq 'reuse-existing-cache'
}
function Get-SuperQualifiedWindows([object[]]$Cycles,[switch]$IncludeUnqualified) {
    foreach($cycle in $Cycles){
        if($cycle.kind -ne 'measurement' -or $cycle.status -ne 'collected'){continue}
        if(-not $IncludeUnqualified -and (-not $cycle.warm -or -not $cycle.meter_consumed -or -not $cycle.recovery.completed -or
           -not(Test-SuperWarmCycle $cycle.filled @($cycle.before,$cycle.after) $cycle.after))){
            throw 'A qualified Full interval contains invalid compilation, consumption or recovery evidence.'
        }
        $start=[DateTimeOffset]::FromUnixTimeMilliseconds([long]$cycle.filled.observed_unix_ms)
        $end=[DateTimeOffset]::FromUnixTimeMilliseconds([long]$cycle.after.observed_unix_ms)
        if($end -le $start){throw 'Invalid qualified Full interval.'}
        [pscustomobject]@{cycle=$cycle.cycle;release_sequence=$cycle.release.sequence
            warm=[bool]$cycle.warm;window_started=$start.UtcDateTime.ToString('o');window_finished=$end.UtcDateTime.ToString('o')}
    }
}

# Full reuses the existing Full tools/configuration/artifact validation. Only its
# input owner and capture anchors change; normal Full plans never enter this path.
function Set-SuperFullPlan($Plan,$Request) {
    if($Plan.profile -ne 'full' -or $Request.collector_mode -ne 'full'){throw 'Full super capture requires a Full plan and batch.'}
    if($Request.PSObject.Properties['vtune_cpu_mode'] -and $Request.vtune_cpu_mode -ne 'hardware-memory'){
        throw 'Full requires hardware-memory; software sampling is not a Full replacement.'
    }
    $reuse=$null -ne $Request.PSObject.Properties['warmup_policy'] -and $Request.warmup_policy -eq 'reuse-existing-cache'
    foreach($rule in @(@('rounds_per_profile',1,20),@('minimum_warmup',0,20),@('maximum_warmup',0,40),
                       @('baseline_seconds',5,30),@('release_seconds',25,90),@('peak_seconds',1,20))){
        $value=$Request.($rule[0])
        if($null -eq $value -or $value -lt $rule[1] -or $value -gt $rule[2]){throw ('Invalid Full super setting: '+$rule[0])}
    }
    if($Request.maximum_warmup -lt $Request.minimum_warmup){throw 'Invalid Full warmup bounds.'}
    $Plan.super_capture=@{batch=$Request.batch;controller='super-batch-driver';rounds=$Request.rounds_per_profile
        minimum_warmup=$Request.minimum_warmup;maximum_warmup=$Request.maximum_warmup
        baseline_seconds=$Request.baseline_seconds;release_seconds=$Request.release_seconds;peak_seconds=$Request.peak_seconds
        progress_clock='native-guest-flips';reference_fps=60;cycle_timeout_seconds=1200
        recovery_samples=3;recovery_sample_frames=60;finalization_timeout_seconds=28800}
    if($reuse){
        if($Request.minimum_warmup -ne 0 -or $Request.maximum_warmup -ne 0){throw 'Cache-reuse Full does not run super warmup cycles.'}
        $Plan.super_capture.warmup_policy='reuse-existing-cache'
    }elseif($Request.minimum_warmup -lt 2 -or $Request.maximum_warmup -lt 5){throw 'Legacy Full requires warmup bounds.'}
    $Plan.delay_seconds=-1
    # Frame-driven workload can take much longer under Full instrumentation.
    # These are watchdog budgets only; the controller ends a completed workload.
    $Plan.seconds=[int](($Request.rounds_per_profile+10)*($Plan.super_capture.cycle_timeout_seconds+12)+60)
    $Plan.automation.enabled=$false
    $Plan.automation.boot_timeout_seconds=360
    $Plan.automation.warmup_seconds=0
    $gameplayTimeout=[int](360+20+($Request.maximum_warmup+$Request.rounds_per_profile+10)*($Plan.super_capture.cycle_timeout_seconds+12)+180)
    $Plan.super_capture.gameplay_timeout_seconds=$gameplayTimeout
    # The existing worker waits for offline Nsight export before orderly shutdown.
    $Plan.automation.session_timeout_seconds=$gameplayTimeout+$Plan.super_capture.finalization_timeout_seconds
    $Plan.collector_timing.vtune_seconds=$gameplayTimeout
    $Plan.collector_timing.nsight_timeout_seconds=$gameplayTimeout+30
    $Plan.collector_timing.nsight_anchor='acknowledged-super-release-plus-guest-flips'
    $Plan.capture_schedule=@{anchor='native-super-release-ack-and-guest-flips'
        nsight_frames=($Request.peak_seconds*60);renderdoc_frames=($Request.peak_seconds*60)
        guest_trace_frames=(($Request.peak_seconds+5)*60)}
    $Plan.cache_policy='session-private-cold-then-qualified-super-warmup'
    if($reuse){
        $Plan.seconds=[int]($Request.rounds_per_profile*($Plan.super_capture.cycle_timeout_seconds+12)+60)
        $gameplayTimeout=[int](360+20+$Plan.seconds+180)
        $Plan.super_capture.gameplay_timeout_seconds=$gameplayTimeout
        $Plan.automation.session_timeout_seconds=$gameplayTimeout+$Plan.super_capture.finalization_timeout_seconds
        $Plan.collector_timing.vtune_seconds=$Plan.seconds+180
        $Plan.collector_timing.nsight_timeout_seconds=$Plan.seconds+30
        $Plan.collector_timing.phase='gameplay'
        $Plan.collector_timing.anchor='scene-ready-then-sampling-start-receipt'
        $Plan.cache_policy='reuse-existing-state-runtime-cache'
    }
    # Full must retain the original Memory Access hardware scope. Software
    # sampling passing a micro test is not evidence that this path was fixed.
    if($Plan.cpu_sampling_backend -ne 'vtune-hardware' -or $Plan.collectors.vtune_analysis -ne 'memory-access' -or
       -not $Plan.bandwidth_calibration_requested){throw 'Full hardware Memory Access scope was changed.'}
    $Plan.storage_policy='no quotas or automatic retention; stop for low free disk space'
}
function Assert-SuperFullWarmup([object[]]$Cycles,[int]$Minimum) {
    $warmup=@($Cycles|Where-Object {$_.kind -eq 'warmup'})
    if($warmup.Count -lt $Minimum){throw 'Full super capture cannot start before its minimum warmup.'}
    foreach($cycle in @($warmup[-2],$warmup[-1])){
        if(-not $cycle.warm -or -not $cycle.meter_consumed){throw 'Full super capture requires two consecutive compilation-free super releases.'}
        if(-not $cycle.PSObject.Properties['recovery'] -or -not $cycle.recovery.completed){throw 'Full super capture requires recovered warmup releases.'}
    }
}
function Read-SuperFullAck([string]$SessionPath,[long]$Sequence,[string]$Action) {
    if($Sequence -le 0){throw 'Invalid Full super command sequence.'}
    $ack=Read-JsonUtf8 (Join-Path $SessionPath ('control-acks/'+$Sequence.ToString('D16')+'.json'))
    if($ack.sequence -ne $Sequence -or $ack.action -ne $Action -or -not $ack.accepted){throw 'Full super capture acknowledgement mismatch.'}
    return $ack
}
function Update-SuperFullAutomation([string]$Root,[string]$Session,$Plan,$Auto,$State,[double]$Elapsed) {
    if($Elapsed -gt $Plan.super_capture.gameplay_timeout_seconds){throw 'Full super batch exceeded its bounded gameplay timeout.'}
    $path=Resolve-ProjectChild $Root ($Session+'/diagnostics/super-controller.json')
    if(-not(Test-Path -LiteralPath $path -PathType Leaf)){return}
    $control=Read-JsonUtf8 $path
    if($control.schema -ne 1 -or $control.session -ne $Session -or $control.batch -ne $Plan.super_capture.batch -or
       $control.pid -ne $State.target_pid -or $control.process_started -ne $State.target_started_utc){throw 'Full super controller belongs to another process or batch.'}
    if($control.phase -notin @('warming','rewarming','recording','complete')){throw 'Invalid Full super controller phase.'}
    $Auto.phase=$control.phase;$Auto.scene_status='batch-driver-scene-evidence'
    $Auto.scene_evidence=$control.scene_evidence
    if($control.phase -in @('warming','rewarming')){return}
    $sessionPath=Resolve-ProjectChild $Root $Session
    $cycles=@(Read-JsonUtf8 (Join-Path $sessionPath 'super-rounds.json'))
    $reuse=Test-SuperReuseCachePlan $Plan
    if(-not $reuse){Assert-SuperFullWarmup $cycles $Plan.super_capture.minimum_warmup}
    $Auto.ready=$true;$Auto.ready_at=0
    $State.scene_confirmed=$false # OCR/pause evidence remains heuristic.
    $Auto.inputs=@()
    foreach($sequence in $control.releases){
        $ack=Read-SuperFullAck $sessionPath $sequence 'button'
        # Check the immutable command as well: a different button is not a release.
        $command=Read-JsonUtf8 (Join-Path $sessionPath ('control-queue/'+([long]$sequence).ToString('D16')+'.json'))
        if($command.button -ne 'down'){throw 'Full super workload contains a non-release button.'}
        $Auto.inputs+=@{sequence=$ack.sequence;action='button';button='down';acknowledged=$true;applied_unix_ms=$ack.applied_unix_ms}
    }
    if($control.gpu_sequence){
        if(-not $control.releases.Count){throw 'GPU detail capture has no acknowledged super release.'}
        $action=if($Plan.gpu_capture_backend -eq 'nsight'){'nsight'}else{'capture'}
        $ack=Read-SuperFullAck $sessionPath $control.gpu_sequence $action
        $release=$Auto.inputs[0]
        $releaseAck=Read-SuperFullAck $sessionPath $release.sequence 'button'
        if($ack.applied_unix_ms -lt $release.applied_unix_ms){throw 'GPU detail capture preceded the super release.'}
        $State.super_detail_release_sequence=$release.sequence
        $State.super_detail_delay_ms=([long]$ack.applied_unix_ms-[long]$release.applied_unix_ms)
        $State.super_detail_progress_frames=(Get-SuperFrameCount $ack)-(Get-SuperFrameCount $releaseAck)
        if($State.super_detail_progress_frames -lt $Plan.super_capture.peak_seconds*$Plan.super_capture.reference_fps -or
           $State.super_detail_progress_frames -gt $Plan.super_capture.release_seconds*$Plan.super_capture.reference_fps){throw 'GPU detail capture did not land inside the scheduled super guest-frame window.'}
        if($action -eq 'nsight'){
            $State.nsight_trigger_sequence=$ack.sequence;$State.nsight_sdk_accepted=$true
            $State.nsight_scene_anchor_utc=[DateTimeOffset]::FromUnixTimeMilliseconds($ack.applied_unix_ms).UtcDateTime.ToString('o')
        }else{
            if(-not $ack.renderdoc_ready){throw 'RenderDoc did not acknowledge the super frame request.'}
            $Auto.capture_sequence=$ack.sequence;$Auto.capture_trigger_attempted=$true;$Auto.capture_triggered=$true
            $Auto.capture_completed=@(Get-ChildItem -LiteralPath (Join-Path $sessionPath 'user/captures') -Filter '*.rdc' -File -Recurse -ErrorAction SilentlyContinue|Where-Object {$_.Length -gt 0 -and (Test-DiagnosticCaptureClosed $_.FullName)}).Count -gt 0
        }
    }
    $Auto.xray_triggered=[bool]$control.guest_trace_triggered
    if($control.phase -eq 'complete'){
        $measured=@($cycles|Where-Object {$_.kind -eq 'measurement'})
        if($measured.Count -ne $Plan.super_capture.rounds -or $Auto.inputs.Count -ne $measured.Count -or
           @($measured|Where-Object {$_.status -ne 'collected' -or (-not $reuse -and -not $_.warm) -or -not $_.meter_consumed -or
               -not $_.PSObject.Properties['recovery'] -or -not $_.recovery.completed}).Count){throw 'Full super workload has incomplete or unqualified measured releases.'}
        if(@($control.releases|Select-Object -Unique).Count -ne $measured.Count){throw 'Full super workload reused a release acknowledgement.'}
        for($index=0;$index -lt $measured.Count;$index++){
            if($measured[$index].release.sequence -ne $Auto.inputs[$index].sequence){throw 'Full super round and applied release acknowledgement differ.'}
        }
        if(-not $control.gpu_sequence -or -not $Auto.xray_triggered){throw 'Full super detail capture was not triggered.'}
        $State.super_qualified_windows=@(Get-SuperQualifiedWindows $cycles -IncludeUnqualified:$reuse)
        $State.super_excluded_cycles=@($cycles|Where-Object {$_.status -ne 'collected' -or -not $_.warm}|ForEach-Object {$_.cycle})
        $Auto.workload_completed=$true
    }
    Write-JsonUtf8 (Join-Path $sessionPath 'diagnostics/automation.json') $Auto
}
