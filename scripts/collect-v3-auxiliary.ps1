# SPDX-License-Identifier: GPL-2.0-or-later
[CmdletBinding()]
param([ValidateSet('1080p','pro4k')][string]$Mode='pro4k',[Parameter(Mandatory)][string]$ResultPath)
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot 'tools.ps1')
. (Join-Path $PSScriptRoot 'diagnostic_tools.ps1')
$taskRoot=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$taskResult=Resolve-ProjectChild $taskRoot $ResultPath
$taskShell=Join-Path $env:WINDIR 'System32/WindowsPowerShell/v1.0/powershell.exe'
$taskArgs=@('-NoLogo','-NoProfile','-ExecutionPolicy','Bypass','-File',(Join-Path $PSScriptRoot 'launch.ps1'),
    '-Mode',$Mode,'-NoMenu','-TimingProfile','auxiliary','-AutomatedComparison','-ResultPath',$ResultPath)
$taskProcess=$null;$taskSession=$null
try{
    $taskProcess=Start-Process -FilePath $taskShell -ArgumentList (($taskArgs|ForEach-Object {Quote-WindowsArgument $_}) -join ' ') -WorkingDirectory $taskRoot -WindowStyle Hidden -PassThru -RedirectStandardOutput ($taskResult+'.engine.log') -RedirectStandardError ($taskResult+'.engine.stderr.log')
    $taskDeadline=[DateTime]::UtcNow.AddSeconds(120)
    while(-not $taskSession){
        if(Test-Path -LiteralPath $taskResult){$taskLaunch=Read-JsonUtf8 $taskResult;if($taskLaunch.game_launched){$taskSession=$taskLaunch.session}}
        if($taskProcess.HasExited -or [DateTime]::UtcNow -gt $taskDeadline){throw 'Auxiliary engine did not start.'}
        Start-Sleep -Milliseconds 250
    }
    & (Join-Path $PSScriptRoot 'observe-ordinary.ps1') -Session $taskSession -WarmupSeconds 30 -MeasureSeconds 60 -EvidenceOnly
    if(-not $taskProcess.WaitForExit(45000)){throw 'Auxiliary normal exit timed out.'}
    $taskFinal=Read-ProjectSession $taskRoot $taskSession
    if(-not $taskFinal.Launch.complete -or $taskFinal.Launch.exit_code -ne 0){throw 'Auxiliary recording did not complete normally.'}
    $taskSummary=Join-Path $taskFinal.Path 'auxiliary-summary.json'
    if(-not(Test-Path -LiteralPath $taskSummary)){throw 'Auxiliary analyzer did not produce a summary.'}
}finally{
    if($taskSession){try{$taskRecord=Read-ProjectSession $taskRoot $taskSession;if(-not $taskRecord.Launch.complete){& (Join-Path $PSScriptRoot 'control.ps1') -Action quit -Session $taskSession | Out-Null}}catch{Write-Warning $_.Exception.Message}}
    if($taskProcess){$taskProcess.Dispose()}
}
