# SPDX-License-Identifier: GPL-2.0-or-later
[CmdletBinding()]
param([ValidateSet('1080p','pro4k')][string]$Mode='pro4k',
      [string[]]$Cases=@(),
      [ValidateRange(30,600)][int]$WarmupSeconds=90,
      [ValidateRange(20,300)][int]$MeasureSeconds=60,
      [ValidateRange(1,2)][int]$MaxAdditionalGiB=2,
      [string]$ResumePlan,
      [string]$PlanPath,
      [string]$SceneKey='r15-fixed-save-crossroad-v1',
      [ValidateSet('automatic','manual')][string]$SceneGate='automatic',
      [ValidateRange(0.01,0.5)][double]$MaximumImageDistance=.15,
      [switch]$Run)
Set-StrictMode -Version Latest
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot 'tools.ps1')
. (Join-Path $PSScriptRoot 'v3_profiles.ps1')
. (Join-Path $PSScriptRoot 'v3_matrix_policy.ps1')
$taskRoot=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
if($ResumePlan){
    if($Cases.Count){throw 'ResumePlan restores its frozen case list; do not also pass Cases.'}
    $taskOutput=Resolve-ProjectChild $taskRoot $ResumePlan
    if(-not $ResumePlan.StartsWith('state/runtime/v3-matrix-') -or -not $ResumePlan.EndsWith('.json')){throw 'Resume only a matrix plan under state/runtime.'}
    $taskState=ConvertTo-MatrixData (Read-JsonUtf8 $taskOutput)
    if($taskState.schema -ne 2 -or $taskState.kind -ne 'v3-ordinary-independent-and-combined'){throw 'Only scene-gated schema 2 plans can resume.'}
    if(-not $taskState.Contains('workflow_revision') -or $taskState.workflow_revision -lt 4){throw 'Pre-automation plans are retained as history; create a new automatic plan.'}
    Assert-MatrixFrozenFiles $taskRoot $taskState.frozen_files
    $Mode=$taskState.mode;$WarmupSeconds=$taskState.warmup_seconds;$MeasureSeconds=$taskState.measure_seconds
    $SceneKey=$taskState.scene_key;$taskRows=$taskState.rows;$taskSettings=$taskState.launcher_settings
    $SceneGate=$taskState.scene_gate;$MaximumImageDistance=$taskState.maximum_image_distance
    $taskSaveFiles=$taskState.save_source_files
}else{
$taskCatalog=Get-V3ExperimentCatalog
if(-not $Cases.Count){$Cases=@($taskCatalog.Keys)+@('combined-2','combined-4')}
if(@($Cases|Select-Object -Unique).Count -ne $Cases.Count){throw 'Repeated matrix cases are not allowed.'}
$taskRows=@();$taskIndex=0
foreach($taskCase in $Cases){
    $taskCandidates=@(if($taskCase -eq 'baseline-pilot'){}elseif($taskCase -eq 'combined-2'){Get-V3CombinedCandidates 2}elseif($taskCase -eq 'combined-4'){Get-V3CombinedCandidates 4}else{$taskCase})
    $taskExpected=Get-V3CandidateOverrides $taskCandidates
    foreach($taskRole in @('control-before','candidate','control-after')){
        $taskChosen=@(if($taskRole -eq 'candidate'){$taskCandidates})
        $taskRows+=@{index=$taskIndex;case=$taskCase;role=$taskRole;candidates=@($taskChosen)
                     expected_overrides=(Get-V3CandidateOverrides $taskChosen)
                     status='pending';session=$null;launcher_exit_code=$null;attempt=1}
        $taskIndex++
    }
}
$taskProject=Read-JsonUtf8 (Join-Path $taskRoot 'project.json')
$taskReceipt=Read-JsonUtf8 (Join-Path $taskRoot 'Build/RelWithDebInfo/build-receipt.json')
$taskSaveRoot=Resolve-ProjectChild $taskRoot 'state/play/home'
$taskSaveFiles=@(Get-ChildItem -LiteralPath $taskSaveRoot -Recurse -File|ForEach-Object {
    Assert-ProjectPath $taskRoot $_.FullName
    @{path=(Get-RelativePath $taskSaveRoot $_.FullName);bytes=$_.Length;sha256=(Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant()}
})
if(-not $taskSaveFiles.Count){throw 'The fixed gameplay save source is empty.'}
$taskVerifiedLines=@(Invoke-ProjectNativeTool 'preflight' @('--mode',$Mode))
if($LASTEXITCODE -ne 0){throw 'Matrix preflight failed; no game started.'}
$taskVerified=($taskVerifiedLines -join [Environment]::NewLine)|ConvertFrom-Json
$taskSettings=$taskVerified.launcher_settings
$taskPlanRelative=if($PlanPath){$PlanPath}else{'state/runtime/v3-matrix-'+(Get-Date).ToString('yyyyMMdd-HHmmss-fff')+'.json'}
if(-not $taskPlanRelative.StartsWith('state/runtime/v3-matrix-') -or -not $taskPlanRelative.EndsWith('.json')){throw 'PlanPath must name a matrix JSON under state/runtime.'}
$taskOutput=Resolve-ProjectChild $taskRoot $taskPlanRelative
if(Test-Path -LiteralPath $taskOutput){throw 'Do not overwrite an existing matrix; use ResumePlan.'}
$taskFreezePaths=@('scripts/test-v3-matrix.ps1','scripts/v3_matrix_policy.ps1','scripts/v3_profiles.ps1','scripts/observe-ordinary.ps1',
    'scripts/ordinary_scene.ps1','scripts/automatic_scene.ps1','scripts/confirm-ordinary-scene.ps1','scripts/launch.ps1','scripts/control.ps1','scripts/tools.ps1',
    'scripts/diagnostic_automation.ps1','scripts/diagnostic_tools.ps1','scripts/diagnostic_sensors.ps1',
    'scripts/run-v3-suite.ps1','scripts/summarize_v3_suite.py','diagnostics/v3-suite.json',
    'scripts/analyze_v3_matrix.py','scripts/analyze_ordinary.py','scripts/metrics.py','scripts/launcher_settings.py','scripts/project_paths.py',
    'state/play/launcher.json','state/play/user/config.json','state/play/user/custom_configs/CUSA00309.json','state/play/user/perf_tuning.ini',
    'profiles/1080p.json','profiles/pro4k.json','profiles/CUSA00309.xml')
$taskState=[ordered]@{schema=2;kind='v3-ordinary-independent-and-combined';mode=$Mode
    workflow_revision=4;scene_gate=$SceneGate;maximum_image_distance=$MaximumImageDistance;reference_images=[ordered]@{}
    runtime_sha256=$taskProject.runtime_sha256;game_sha256=$taskProject.game_sha256;source_sha256=$taskReceipt.source_sha256
    save_source_files=$taskSaveFiles
    launcher_settings=$taskSettings
    scene_key=$SceneKey;frozen_files=(Get-MatrixFrozenFiles $taskRoot $taskFreezePaths)
    history=@();repeat_required_cases=@();owned_sessions=@();cleanup_pending=$false
    warmup_seconds=$WarmupSeconds;measure_seconds=$MeasureSeconds;audio_muted=$true
    maximum_additional_bytes=([long]$MaxAdditionalGiB*1GB);additional_owned_bytes=0L
    estimated_gameplay_minutes=[Math]::Ceiling($taskRows.Count*($WarmupSeconds+$MeasureSeconds+3)/60)
    full_collectors=$false;automatic_performance_acceptance=$false;run_requested=[bool]$Run
    budget_check='between-runs; the final running session can exceed the threshold'
    analysis_status='pending';analysis_output=(Get-RelativePath $taskRoot (Join-Path ([IO.Path]::GetDirectoryName($taskOutput)) ([IO.Path]::GetFileNameWithoutExtension($taskOutput)+'-summary.json')))
    created_utc=[DateTime]::UtcNow.ToString('o');complete=$false;failure=$null;rows=$taskRows}
Write-JsonUtf8 $taskOutput $taskState
}
function Get-MatrixSaveSignature($Files){
    return (@($Files|Sort-Object {$_.path}|ForEach-Object {$_.path+'|'+$_.bytes+'|'+$_.sha256}) -join "`n")
}
$taskSaveSignature=Get-MatrixSaveSignature $taskSaveFiles
Write-Host ('Matrix plan: '+(Get-RelativePath $taskRoot $taskOutput))
Write-Host ('Runs: '+$taskRows.Count+'; gameplay at least '+$taskState.estimated_gameplay_minutes+' min plus loading; stop starting runs at '+$MaxAdditionalGiB+' GiB of owned session files.')
if(-not $Run){Write-Host 'Plan only. No game or performance test started. Run after the user releases system resources.';exit 0}

function Get-OwnedMatrixSessionBytes([string]$Path){
    $taskDirectories=[Collections.Generic.Stack[IO.DirectoryInfo]]::new()
    $taskDirectories.Push([IO.DirectoryInfo]::new($Path));$taskBytes=0L
    while($taskDirectories.Count){
        foreach($taskItem in $taskDirectories.Pop().EnumerateFileSystemInfos()){
            if($taskItem.Attributes -band [IO.FileAttributes]::ReparsePoint){continue}
            if($taskItem -is [IO.DirectoryInfo]){$taskDirectories.Push($taskItem)}else{$taskBytes+=$taskItem.Length}
        }
    }
    return $taskBytes
}
$taskLock=$null;$taskLauncher=$null;$taskSession=$null
$taskShell=Join-Path $env:WINDIR 'System32/WindowsPowerShell/v1.0/powershell.exe'
try{
    $taskLock=[IO.File]::Open((Join-Path $taskRoot 'state/runtime/v3-matrix.lock'),[IO.FileMode]::OpenOrCreate,[IO.FileAccess]::ReadWrite,[IO.FileShare]::None)
    if($ResumePlan){Reset-InterruptedMatrixGroups $taskState}
    $taskState.run_requested=$true
    $taskPython=Resolve-InstalledPython
    foreach($taskRow in $taskRows){
        if(Test-Path -LiteralPath (Join-Path $taskRoot 'state/runtime/v3-suite.stop')){throw 'Local stop request received.'}
        if($taskRow.status -eq 'validated'){continue}
        Assert-MatrixFrozenFiles $taskRoot $taskState.frozen_files
        if($taskState.additional_owned_bytes -ge $taskState.maximum_additional_bytes){throw 'Matrix storage budget reached; remaining runs were not started.'}
        $taskVolume=[IO.DriveInfo]::new([IO.Path]::GetPathRoot($taskRoot))
        if($taskVolume.AvailableFreeSpace -lt 3GB){throw 'Not enough free space for a safe next run.'}
        $taskCurrentReceipt=Read-JsonUtf8 (Join-Path $taskRoot 'Build/RelWithDebInfo/build-receipt.json')
        if($taskCurrentReceipt.source_sha256 -ne $taskState.source_sha256 -or $taskCurrentReceipt.runtime_sha256 -ne $taskState.runtime_sha256){throw 'The activated build changed during the matrix.'}
        $taskPrevious=Read-JsonUtf8 (Join-Path $taskRoot 'state/active.json')
        $taskArguments=@('-NoLogo','-NoProfile','-ExecutionPolicy','Bypass','-File',(Join-Path $taskRoot 'scripts/launch.ps1'),'-Mode',$Mode,'-NoMenu','-OptimizationV3')
        if($taskRow.candidates.Count){$taskArguments+=@('custom','-V3CandidateList',($taskRow.candidates -join ','))}else{$taskArguments+='control'}
        if($SceneGate -eq 'automatic'){$taskArguments+='-AutomatedComparison'}
        $taskPrefix=[IO.Path]::GetFileNameWithoutExtension($taskOutput)+'-'+$taskRow.index+'-try'+$taskRow.attempt
        $taskStdout=Join-Path $taskRoot ('state/runtime/'+$taskPrefix+'.log')
        $taskStderr=Join-Path $taskRoot ('state/runtime/'+$taskPrefix+'.stderr.log')
        $taskLauncher=Start-Process -FilePath $taskShell -ArgumentList (($taskArguments|ForEach-Object {Quote-WindowsArgument $_}) -join ' ') -WorkingDirectory $taskRoot -WindowStyle Hidden -PassThru -RedirectStandardOutput $taskStdout -RedirectStandardError $taskStderr
        $taskDeadline=[DateTime]::UtcNow.AddSeconds(120);$taskSession=$null
        while(-not $taskSession){
            $taskActive=Read-JsonUtf8 (Join-Path $taskRoot 'state/active.json')
            if($taskActive.session -ne $taskPrevious.session){$taskSession=$taskActive.session;break}
            if($taskLauncher.HasExited){throw ('Launcher exited before a new game session: '+$taskStdout)}
            if([DateTime]::UtcNow -gt $taskDeadline){throw 'Game startup deadline exceeded.'}
            Start-Sleep -Milliseconds 250
        }
        $taskRecord=Read-ProjectSession $taskRoot $taskSession
        if($taskState.owned_sessions -notcontains $taskSession){$taskState.owned_sessions+=,$taskSession}
        $taskLaunch=$taskRecord.Launch
        if($taskLaunch.runtime_sha256 -ne $taskState.runtime_sha256 -or $taskLaunch.game_sha256 -ne $taskState.game_sha256 -or -not $taskLaunch.audio_muted){throw 'Matrix identity or mute policy changed.'}
        if((Get-MatrixSaveSignature $taskLaunch.save_source_files) -ne $taskSaveSignature){throw 'Save source changed during the matrix.'}
        if((ConvertTo-Json -InputObject $taskLaunch.launcher_settings -Compress) -ne (ConvertTo-Json -InputObject $taskSettings -Compress)){throw 'Launcher/display settings changed during the matrix.'}
        foreach($taskKey in $taskRow.expected_overrides.Keys){
            if([int]$taskLaunch.v3_overrides.$taskKey -ne $taskRow.expected_overrides[$taskKey]){throw ('Candidate configuration mismatch: '+$taskKey)}
        }
        $taskRow.status='running';$taskRow.session=$taskSession
        Write-JsonUtf8 $taskOutput $taskState
        $taskReference=if($taskState.reference_images.Contains($taskRow.case)){$taskState.reference_images[$taskRow.case]}else{''}
        & (Join-Path $PSScriptRoot 'observe-ordinary.ps1') -Session $taskSession -WarmupSeconds $WarmupSeconds -MeasureSeconds $MeasureSeconds -SceneKey $SceneKey -SceneGate $SceneGate -ReferenceImage $taskReference -MaximumImageDistance $MaximumImageDistance
        if(-not $taskLauncher.WaitForExit(45000)){throw 'Normal exit remains pending; matrix stopped.'}
        $taskLauncher.Refresh();$taskRow.launcher_exit_code=$taskLauncher.ExitCode
        $taskFinal=Read-JsonUtf8 (Join-Path $taskRecord.Path 'launch.json')
        if(-not $taskFinal.complete -or $taskFinal.exit_code -ne 0){throw 'Game did not complete normally.'}
        if(-not $taskReference){
            $taskSceneReceipt=Read-JsonUtf8 (Join-Path $taskRecord.Path 'ordinary-observer/scene-confirmed.json')
            $taskState.reference_images[$taskRow.case]=$taskSceneReceipt.screenshot
        }
        $taskRow.status='collected';$taskSession=$null
        Write-JsonUtf8 $taskOutput $taskState
        Assert-MatrixFrozenFiles $taskRoot $taskState.frozen_files
        & $taskPython -B (Join-Path $PSScriptRoot 'analyze_v3_matrix.py') --plan (Get-RelativePath $taskRoot $taskOutput)
        if($LASTEXITCODE -ne 0){$taskRow.status='invalid';throw 'Immediate row validation failed; original evidence retained.'}
        $taskRow.status='validated'
        $taskSummary=Read-JsonUtf8 (Resolve-ProjectChild $taskRoot $taskState.analysis_output)
        if($taskRow.role -eq 'control-after'){
            $taskGroup=@($taskSummary.groups|Where-Object {$_.case -eq $taskRow.case})[0]
            if($taskGroup.complete -and $taskGroup.comparison.control_drift_over_3_percent){
                $taskState.repeat_required_cases+=,$taskRow.case
                throw 'Control drift exceeds the review threshold; matrix stopped for condition review.'
            }
        }
        $taskState.additional_owned_bytes=0L
        foreach($taskOwned in $taskState.owned_sessions){$taskState.additional_owned_bytes+=Get-OwnedMatrixSessionBytes (Resolve-ProjectChild $taskRoot $taskOwned)}
        $taskLauncher.Dispose();$taskLauncher=$null
    }
    $taskState.complete=$true
    Write-JsonUtf8 $taskOutput $taskState
    try{
        & $taskPython -B (Join-Path $PSScriptRoot 'analyze_v3_matrix.py') --plan (Get-RelativePath $taskRoot $taskOutput)
        if($LASTEXITCODE -ne 0){throw 'Offline matrix analysis failed; all collected evidence was retained.'}
        $taskState.analysis_status='complete'
    }catch{$taskState.analysis_status='failed';throw}
}catch{
    $taskState.failure=$_.Exception.Message
    throw
}finally{
    if($taskSession){
        try{& (Join-Path $PSScriptRoot 'control.ps1') -Action quit -Session $taskSession}catch{Write-Warning ('Normal game exit request failed: '+$_.Exception.Message)}
        if($taskLauncher -and -not $taskLauncher.WaitForExit(45000)){$taskState.cleanup_pending=$true}
    }
    Write-JsonUtf8 $taskOutput $taskState
    if($taskLauncher){$taskLauncher.Dispose()}
    if($taskLock){$taskLock.Dispose()}
}
Write-Host 'All matrix runs collected and summarized. Scene/image correctness and repeatable gains still require review.'
