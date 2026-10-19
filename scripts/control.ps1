[CmdletBinding()]
param([Parameter(Mandatory)][ValidateSet('status','screenshot','quit','button','axis')][string]$Action,
      [ValidateSet('cross','circle','options','up','down','left','right','square','triangle')][string]$Button='cross',
      [ValidateRange(50,2000)][int]$DurationMs=200,[ValidateRange(0,3)][int]$Axis=2,
      [ValidateRange(0,255)][int]$Value=128,[string]$Session)
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot 'tools.ps1')
$projectRoot=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$session=Read-ProjectSession $projectRoot $Session
$sessionPath=$session.Path
$active=$session.Launch
if(-not $active.game_launched -or $active.complete){throw 'This session is not running.'}
$process=Get-Process -Id $active.pid -ErrorAction Stop
if($process.Path -ne (Resolve-ProjectPath $projectRoot $active.runtime) -or $process.StartTime.ToUniversalTime().ToString('o') -ne $active.process_started){throw 'The saved process identity no longer matches.'}
$lockPath=Resolve-ProjectChild $projectRoot ((Get-RelativePath $projectRoot $sessionPath)+'/control.lock')
$commandLock=[IO.File]::Open($lockPath,[IO.FileMode]::OpenOrCreate,[IO.FileAccess]::ReadWrite,[IO.FileShare]::None)
try{
    $path=Resolve-ProjectChild $projectRoot ((Get-RelativePath $projectRoot $sessionPath)+'/command.json')
    $sequence=1L
    if(Test-Path -LiteralPath $path){$sequence=1+[long](Get-Content -LiteralPath $path -Raw -Encoding UTF8|ConvertFrom-Json).sequence}
    $command=[ordered]@{sequence=$sequence;action=$Action}
    if($Action -eq 'button'){$command.button=$Button;$command.duration_ms=$DurationMs}
    if($Action -eq 'axis'){$command.axis=$Axis;$command.value=$Value}
    Write-JsonUtf8 $path $command
    Write-Host ('Command '+$sequence+': '+$Action+'; session '+(Get-RelativePath $projectRoot $sessionPath))
}finally{
    $commandLock.Dispose()
}
