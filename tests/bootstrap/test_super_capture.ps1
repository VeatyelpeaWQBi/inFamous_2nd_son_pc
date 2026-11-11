# SPDX-License-Identifier: GPL-2.0-or-later
# Offline qualification regression: no game, compiler, capture or controller input.
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot '../../scripts/super_capture_tools.ps1')
function Snapshot([long]$Shaders=10,[long]$Pipelines=20,[long]$Active=0,[long]$Meter=7){
    return [pscustomobject]@{shader_compile_starts=$Shaders;pipeline_compile_starts=$Pipelines
        compilations_active=$Active;karma_fill_requested=1;karma_fill_completed=1;karma_fill_count=$Meter}
}
$before=Snapshot
$drawMeta=[pscustomobject]@{complete=$true;dropped=0;cutoff_publishers=0;io_failed=$false;events=6
    draw_prep_audit=$true;draw_prep_protocol=2;draw_prep_sample_period=256;draw_prep_sample_cap=65536;draw_prep_cap_hit=$false}
if(@(Get-SuperCriticalTraceErrors $drawMeta -RequireDrawPrep).Count){throw 'Complete bounded draw audit must qualify.'}
$drawMeta.draw_prep_protocol=1
if(@(Get-SuperCriticalTraceErrors $drawMeta -RequireDrawPrep).Count -eq 0){throw 'Intrusive legacy draw audit cannot qualify.'}
$drawMeta.draw_prep_protocol=2
$drawMeta.draw_prep_cap_hit=$true
if(@(Get-SuperCriticalTraceErrors $drawMeta -RequireDrawPrep).Count -eq 0){throw 'Exhausted draw audit budget cannot qualify.'}
$drawMeta.draw_prep_cap_hit=$false;$drawMeta.draw_prep_audit=$false
if(@(Get-SuperCriticalTraceErrors $drawMeta -RequireDrawPrep).Count -eq 0){throw 'A trace without the requested draw audit cannot qualify.'}
$after=Snapshot
if(-not(Test-SuperWarmCycle $before @((Snapshot),(Snapshot)) $after)){throw 'Stable compile counters must qualify.'}
if(Test-SuperWarmCycle $before @((Snapshot -Shaders 11)) (Snapshot -Shaders 11)){throw 'A new shader compile must invalidate the round.'}
if(Test-SuperWarmCycle $before @((Snapshot -Pipelines 21)) (Snapshot -Pipelines 21)){throw 'A new pipeline compile must invalidate the round.'}
if(Test-SuperWarmCycle $before @((Snapshot -Active 1)) $after){throw 'An in-flight compile must invalidate the round.'}
if(Test-SuperWarmCycle (Snapshot -Active 1) @($after) $after){throw 'An unfinished baseline compile must invalidate the round.'}
if(Test-SuperWarmCycle $before @() $after){throw 'An empty record cannot prove warmup.'}
if(Test-SuperCompileSnapshot ([pscustomobject]@{shader_compile_starts=0})){throw 'Old engines without complete compile telemetry must fail closed.'}
if(Test-SuperCompileSnapshot (Snapshot -Shaders -1)){throw 'Invalid counter values must fail closed.'}
if(-not(Test-SuperMeterConsumed 7 @((Snapshot -Meter 0)))){throw 'Live meter consumption must be detected.'}
if(Test-SuperMeterConsumed 7 @((Snapshot -Meter 7))){throw 'Acknowledging down without consuming the meter cannot qualify.'}
if(Test-SuperMeterConsumed 0 @((Snapshot -Meter 0))){throw 'An empty fill cannot qualify.'}
if(Test-SuperFrameProgress ([pscustomobject]@{frame_count=1000}) ([pscustomobject]@{frame_count=1092}) 300){throw '92 slow Full frames cannot reach the 300-frame capture point.'}
if(-not(Test-SuperFrameProgress ([pscustomobject]@{frame_count=1000}) ([pscustomobject]@{frame_count=1300}) 300)){throw 'Real guest progress must advance Full capture.'}
foreach($sample in @([pscustomobject]@{},[pscustomobject]@{frame_count=-1},[pscustomobject]@{frame_count='bad'})){
    $rejected=$false;try{$null=Get-SuperFrameCount $sample}catch{$rejected=$true}
    if(-not $rejected){throw 'Unavailable guest progress must fail closed.'}
}
$rejected=$false;try{$null=Test-SuperFrameProgress ([pscustomobject]@{frame_count=1000}) ([pscustomobject]@{frame_count=999}) 300}catch{$rejected=$true}
if(-not $rejected){throw 'Guest progress must not regress.'}
$base=Snapshot;$base|Add-Member NoteProperty dispatches_per_frame 440;$base|Add-Member NoteProperty draws_per_frame 7600
$busy=Snapshot;$busy|Add-Member NoteProperty dispatches_per_frame 1245;$busy|Add-Member NoteProperty draws_per_frame 5247
if(Test-SuperRecoverySnapshot $base $busy){throw 'The previous super cinematic cannot pass the recovery gate.'}
$busy.dispatches_per_frame=450
if(-not(Test-SuperRecoverySnapshot $base $busy)){throw 'Recovered render counters must be recognized.'}
$busy.compilations_active=1
if(Test-SuperRecoverySnapshot $base $busy){throw 'In-flight compilation cannot pass recovery.'}
$root=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
foreach($relative in @('scripts/collect_super.ps1','scripts/super_capture_tools.ps1','scripts/launch.ps1','scripts/control.ps1')){
    $tokens=$null;$errors=$null
    $null=[Management.Automation.Language.Parser]::ParseFile((Join-Path $root $relative),[ref]$tokens,[ref]$errors)
    if($errors.Count){throw ($relative+': '+($errors.Message -join '; '))}
}
Write-Host 'PASS: warmup/new/in-flight compile gates, live meter consumption, collection script syntax. No game started.'

$validTrace=[pscustomobject]@{complete=$true;dropped=0;cutoff_publishers=0;io_failed=$false;events=500}
if(@(Get-SuperCriticalTraceErrors $validTrace).Count){throw 'Complete loss-free trace rejected.'}
foreach($key in @('dropped','cutoff_publishers','io_failed')){
    $badTrace=[pscustomobject]@{complete=$true;dropped=0;cutoff_publishers=0;io_failed=$false;events=500}
    $badTrace.$key=1
    if(-not @(Get-SuperCriticalTraceErrors $badTrace).Count){throw ('Trace integrity failure hidden: '+$key)}
}
if(-not @(Get-SuperCriticalTraceErrors $null).Count){throw 'Missing trace accepted.'}
if(-not @(Get-SuperCriticalTraceErrors ([pscustomobject]@{complete=$false})).Count){throw 'Incomplete trace accepted.'}
Write-Host 'PASS: critical trace losses/missing metadata fail closed after workload completion.'

# Replay the actual native driver loop with cold and warm snapshots. No game/files.
$entry=[Management.Automation.Language.Parser]::ParseFile((Join-Path $root 'scripts/collect_super.ps1'),[ref]$tokens,[ref]$errors)
$prefix=$entry.Extent.Text.Substring(0,$entry.Extent.Text.IndexOf('. (Join-Path $PSScriptRoot ''tools.ps1'')'))
$settings=[scriptblock]::Create($prefix+@'
return [pscustomobject]@{rounds=$Rounds;minimum=$MinimumWarmup;maximum=$MaximumWarmup
    simple=$simpleNative;experiment=$Experiment;full=[bool]$FullCapture}
'@)
$native=& $settings
if($native.rounds -ne 2 -or $native.minimum -ne 0 -or $native.maximum -ne 0 -or
   -not $native.simple -or $native.experiment -ne 'compute-barriers'){throw 'Native default must be exactly two A/B releases, no warmup.'}
$full=& $settings -FullCapture
if($full.rounds -ne 1 -or $full.minimum -ne 0 -or $full.maximum -ne 0 -or $full.simple){throw 'Full must remain one release.'}
$critical=& $settings -CriticalCapture -Profile counters
if($critical.rounds -ne 1 -or $critical.simple -or $critical.experiment -ne 'none'){throw 'Critical one-release probe changed.'}
$rejected=$false
try{$null=& $settings -Rounds 3}catch{$rejected=$true}
if(-not $rejected){throw 'An incomplete A/B count must be rejected before launching.'}
$loop=$entry.Find({param($node)$node -is [Management.Automation.Language.ForEachStatementAst] -and
    $node.Variable.VariablePath.UserPath -eq 'collector'},$true)
$profileStatement=$entry.Find({param($node)$node -is [Management.Automation.Language.AssignmentStatementAst] -and
    $node.Left.Extent.Text -eq '$profiles'},$true)
if(-not $loop -or -not $profileStatement){throw 'Native collection loop or profile selection missing.'}
$driver=[scriptblock]::Create($profileStatement.Extent.Text+"`r`n"+$loop.Extent.Text)
foreach($coldAt in @(-1,0,1,2)){
    & {
        param($Driver,$ColdAt)
        $FullCapture=$false;$CriticalCapture=$false;$ProbeCandidate=$false
        $Candidate='none';$CandidateBaseline=$false
        $simpleNative=$true;$Profile='suite';$Rounds=2;$Experiment='compute-barriers'
        $batch=@{status='starting';cycles=@();comparison_qualified=$true;comparison_warnings=@()}
        $record=@{Path='unused'};$observed=[Collections.Generic.List[object]]::new()
        function Save-SuperBatch {}
        function Set-SuperProfile([string]$Name){if($Name -ne 'lightweight'){throw 'Default added a second or heavy profile.'}}
        function Set-SuperVariant($Variant) {}
        function Wait-SuperSeconds([double]$Seconds) {}
        function Invoke-SuperCycle([string]$Kind,[string]$Collector,[int]$Ordinal){
            if($Kind -ne 'measurement'){throw 'Hidden warmup release.'}
            $observed.Add([pscustomobject]@{flag=$currentVariant.tuning.compute_barrier_scope;ordinal=$Ordinal})
            $warm=$ColdAt -ne 2 -and ($observed.Count-1) -ne $ColdAt
            return [pscustomobject]@{warm=$warm;status=if($warm){'collected'}else{'captured-new-compilation'}}
        }
        & $Driver
        if($observed.Count -ne 2 -or $observed[0].flag -ne 0 -or $observed[1].flag -ne 1 -or
           $observed[0].ordinal -ne 1 -or $observed[1].ordinal -ne 2){throw 'Native did not stop after exactly A then B.'}
        if($batch.comparison_qualified -ne ($ColdAt -eq -1)){throw 'Cold pair qualification was lost.'}
    } $driver $coldAt
}
Write-Host 'PASS: native A/B driver stops after two releases even with new compilation; Full and Critical remain one.'
