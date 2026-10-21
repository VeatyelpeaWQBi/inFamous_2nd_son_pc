# SPDX-License-Identifier: GPL-2.0-or-later
[CmdletBinding()]
param([Parameter(Mandatory)][string]$Session,[Parameter(Mandatory)][string]$Screenshot,
      [Parameter(Mandatory)][string]$SceneKey,[switch]$ConfirmGameplay,[switch]$ConfirmView)
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot 'tools.ps1')
. (Join-Path $PSScriptRoot 'diagnostic_tools.ps1')
. (Join-Path $PSScriptRoot 'ordinary_scene.ps1')
$taskRoot=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$taskRecord=Read-ProjectSession $taskRoot $Session
if(-not $ConfirmGameplay -or -not $ConfirmView){throw 'Review the actual image and explicitly confirm gameplay and matching view first.'}
if($taskRecord.Launch.complete -or -not $taskRecord.Launch.audio_muted){throw 'A running muted session is required.'}
$taskTarget=Get-DiagnosticTarget $taskRecord.Launch $taskRoot
if(-not $taskTarget){throw 'The pinned game process is no longer running.'}
try{
    $taskImage=Resolve-ProjectChild $taskRoot $Screenshot
    if(-not $taskImage.StartsWith($taskRecord.Path+[IO.Path]::DirectorySeparatorChar,[StringComparison]::OrdinalIgnoreCase) -or -not (Test-DiagnosticPngReady $taskImage)){throw 'A complete screenshot from this session is required.'}
    $taskInfo=Get-Item -LiteralPath $taskImage
    if($taskInfo.LastWriteTimeUtc -lt [DateTime]::Parse($taskRecord.Launch.process_started) -or [DateTime]::UtcNow-$taskInfo.LastWriteTimeUtc -gt [TimeSpan]::FromSeconds(90)){throw 'Scene screenshot is stale.'}
    $taskPath=Join-Path $taskRecord.Path 'ordinary-observer/scene-confirmed.json'
    if(Test-Path -LiteralPath $taskPath){throw 'Scene confirmation already exists; do not overwrite evidence.'}
    $taskReceipt=@{schema=1;session=$Session;runtime_sha256=$taskRecord.Launch.runtime_sha256;game_sha256=$taskRecord.Launch.game_sha256
        pid=$taskRecord.Launch.pid;process_started=$taskRecord.Launch.process_started;scene_key=$SceneKey
        screenshot=$Screenshot;screenshot_sha256=(Get-FileHash -LiteralPath $taskImage -Algorithm SHA256).Hash.ToLowerInvariant()
        evidence='agent-reviewed-gameplay-image';gameplay_confirmed=$true;view_confirmed=$true
        elapsed_seconds=(Get-OrdinaryElapsed $taskRecord.Path);confirmed_utc=[DateTime]::UtcNow.ToString('o')}
    Test-OrdinarySceneReceipt $taskRecord.Launch $taskReceipt $SceneKey
    Write-JsonUtf8 $taskPath $taskReceipt
    Write-Host ('Gameplay and view review recorded for '+$Session)
}finally{$taskTarget.Dispose()}
