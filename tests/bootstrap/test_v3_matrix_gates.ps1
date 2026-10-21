# SPDX-License-Identifier: GPL-2.0-or-later
$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest
$taskRoot=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
. (Join-Path $taskRoot 'scripts/tools.ps1')
. (Join-Path $taskRoot 'scripts/ordinary_scene.ps1')
. (Join-Path $taskRoot 'scripts/v3_matrix_policy.ps1')
. (Join-Path $taskRoot 'scripts/automatic_scene.ps1')
$taskStatusLaunch=@{resolution=@{width=3840;height=2160}}
$taskStatusJson='{"schema":2,"sequence":7,"game_serial":"CUSA00309","perf_log":false,"gpu_profile":0,"display_window_count":1,"display_width":3840,"display_height":2160,"observed_unix_ms":10000,"draws_per_frame":5500,"dispatches_per_frame":1500}'
$taskStatus=ConvertFrom-Json $taskStatusJson
if(-not(Test-OrdinaryStatusSnapshot $taskStatus 7 $taskStatusLaunch 10000)){throw 'Complete native status was rejected.'}
foreach($taskKey in @('sequence','schema','game_serial','observed_unix_ms','draws_per_frame','dispatches_per_frame','display_width')){
    $taskIncomplete=ConvertFrom-Json $taskStatusJson
    $taskIncomplete.PSObject.Properties.Remove($taskKey)
    if(Test-OrdinaryStatusSnapshot $taskIncomplete 7 $taskStatusLaunch 10000){throw ('Partial native status accepted: '+$taskKey)}
}
foreach($taskEmptyStatus in @($null,(ConvertFrom-Json '{}'),(ConvertFrom-Json '[]'))){
    if(Test-OrdinaryStatusSnapshot $taskEmptyStatus 7 $taskStatusLaunch 10000){throw 'Empty native status was accepted.'}
}
if((Test-OrdinaryStatusSnapshot $taskStatus 8 $taskStatusLaunch 10000) -or
   (Test-OrdinaryStatusSnapshot $taskStatus 7 $taskStatusLaunch 16000) -or
   (Test-OrdinaryStatusSnapshot $taskStatus 7 $taskStatusLaunch 8000)){throw 'Stale/wrong-sequence native status was accepted.'}
foreach($taskMutation in @(@{key='schema';value=1},@{key='game_serial';value='wrong'},@{key='perf_log';value=$true},@{key='display_width';value=1920})){
    $taskBadStatus=ConvertFrom-Json $taskStatusJson;$taskBadStatus.($taskMutation.key)=$taskMutation.value
    $taskRejected=$false;try{$null=Test-OrdinaryStatusSnapshot $taskBadStatus 7 $taskStatusLaunch 10000}catch{$taskRejected=$true}
    if(-not $taskRejected){throw ('Unsafe native status was accepted: '+$taskMutation.key)}
}
$taskLaunch=@{session='state/sessions/test';runtime_sha256='a';game_sha256='b';pid=42;process_started='time'}
$taskReceipt=@{schema=1;session='state/sessions/test';runtime_sha256='a';game_sha256='b';pid=42;process_started='time'
    scene_key='fixed';evidence='agent-reviewed-gameplay-image';gameplay_confirmed=$true;view_confirmed=$true
    elapsed_seconds=5;screenshot_sha256=('c'*64)}
Test-OrdinarySceneReceipt $taskLaunch $taskReceipt 'fixed'
foreach($taskField in @('pid','process_started','session','scene_key','evidence')){
    $taskBad=ConvertTo-MatrixData $taskReceipt;$taskBad[$taskField]='wrong'
    $taskRejected=$false;try{Test-OrdinarySceneReceipt $taskLaunch $taskBad 'fixed'}catch{$taskRejected=$true}
    if(-not $taskRejected){throw ('Scene replay accepted: '+$taskField)}
}
$taskEmpty=ConvertTo-MatrixData (ConvertFrom-Json '{"candidates":[],"history":[],"rows":[{"candidates":[]}]}')
if($taskEmpty.candidates.Count -ne 0 -or $taskEmpty.history.Count -ne 0 -or $taskEmpty.rows.Count -ne 1 -or $taskEmpty.rows[0].candidates.Count -ne 0){throw 'PS5 empty arrays were not preserved.'}
$taskState=@{rows=@(@{case='x';status='validated';session='old-control';attempt=1;launcher_exit_code=0},
    @{case='x';status='running';session='old-candidate';attempt=1;launcher_exit_code=$null},
    @{case='x';status='pending';session=$null;attempt=1;launcher_exit_code=$null});history=@();repeat_required_cases=@();failure='interrupted';complete=$false;analysis_status='pending'}
Reset-InterruptedMatrixGroups $taskState
if($taskState.history.Count -ne 1 -or $taskState.history[0].rows[0].status -ne 'validated' -or
   @($taskState.rows|Where-Object {$_.status -ne 'pending' -or $_.attempt -ne 2 -or $_.session}).Count){throw 'Interrupted A/B/A group was not preserved and restarted as a whole.'}
$taskDirectory=Join-Path $taskRoot ('state/runtime/r16-policy-test-'+[Guid]::NewGuid().ToString('N'))
$null=[IO.Directory]::CreateDirectory($taskDirectory)
$taskPath=Join-Path $taskDirectory 'input.txt'
[IO.File]::WriteAllText($taskPath,'first')
$taskFiles=Get-MatrixFrozenFiles $taskRoot @((Get-RelativePath $taskRoot $taskPath))
Assert-MatrixFrozenFiles $taskRoot $taskFiles
[IO.File]::WriteAllText($taskPath,'changed')
$taskRejected=$false;try{Assert-MatrixFrozenFiles $taskRoot $taskFiles}catch{$taskRejected=$true}
if(-not $taskRejected){throw 'Changed frozen inputs were accepted.'}
Write-Host 'Native status completeness/freshness, scene identity/replay, PS5 empty arrays, frozen inputs and whole-group resume checks passed. No game started.'
