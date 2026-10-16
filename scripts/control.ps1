[CmdletBinding()]
param([Parameter(Mandatory)][ValidateSet('status','screenshot','capture','nsight','nsight-stop','vtune-detach','quit','button','axis')][string]$Action,
      [ValidateSet('cross','circle','options','up','down','left','right','square','triangle')][string]$Button='cross',
      [ValidateRange(50,2000)][int]$DurationMs=200,[ValidateRange(0,3)][int]$Axis=2,
      [ValidateRange(0,255)][int]$Value=128,[string]$Session)
$ErrorActionPreference='Stop'
# Reuse helpers already loaded by the automation caller.
if(-not(Get-Command Get-ProjectProcessIdentity -CommandType Function -ErrorAction SilentlyContinue)){
    . (Join-Path $PSScriptRoot 'tools.ps1')
}
$projectRoot=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$record=Read-ProjectSession $projectRoot $Session
$sessionPath=$record.Path
$active=$record.Launch
if(-not $active.game_launched -or $active.complete){throw 'This session is not running.'}
$process=Get-Process -Id $active.pid -ErrorAction Stop
if(-not(Test-ProjectProcessIdentity $process (Resolve-ProjectChild $projectRoot $active.runtime) $active.process_started)){throw 'The saved process identity no longer matches.'}
$lockPath=Resolve-ProjectChild $projectRoot ((Get-RelativePath $projectRoot $sessionPath)+'/control.lock')
$commandLock=[IO.File]::Open($lockPath,[IO.FileMode]::OpenOrCreate,[IO.FileAccess]::ReadWrite,[IO.FileShare]::None)
try{
    $path=Resolve-ProjectChild $projectRoot ((Get-RelativePath $projectRoot $sessionPath)+'/command.json')
    $sequence=1L
    if(Test-Path -LiteralPath $path){$sequence=1+[long](Read-JsonUtf8 $path).sequence}
    $command=[ordered]@{sequence=$sequence;action=$Action}
    if($Action -eq 'button'){$command.button=$Button;$command.duration_ms=$DurationMs}
    if($Action -eq 'axis'){$command.axis=$Axis;$command.value=$Value}
    # Publish an immutable FIFO entry before updating the latest-command summary.
    # Fast producers must never overwrite an unapplied screenshot/input request.
    $queue=Resolve-ProjectChild $projectRoot ((Get-RelativePath $projectRoot $sessionPath)+'/control-queue')
    $null=[IO.Directory]::CreateDirectory($queue)
    $queued=Join-Path $queue ($sequence.ToString('D16')+'.json')
    if(Test-Path -LiteralPath $queued){throw 'Control sequence collision; no command was replaced.'}
    Write-JsonUtf8 $queued $command
    Write-JsonUtf8 $path $command
    Write-Host ('Command '+$sequence+': '+$Action+'; session '+(Get-RelativePath $projectRoot $sessionPath))
}finally{
    $commandLock.Dispose()
}
