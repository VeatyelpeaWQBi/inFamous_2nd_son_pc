# SPDX-License-Identifier: GPL-2.0-or-later
# Offline handshake regression. No game or profiler is started.
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot '../../scripts/tools.ps1')
. (Join-Path $PSScriptRoot '../../scripts/diagnostic_tools.ps1')
$root=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
$relative='state/sessions/offline-vtune-'+[Guid]::NewGuid().ToString('N')
$session=Resolve-ProjectChild $root $relative
$directory=Join-Path $session 'diagnostics'
$ackPath=Join-Path $session 'control-acks/0000000000000001.json'
$null=[IO.Directory]::CreateDirectory($directory)
$null=[IO.Directory]::CreateDirectory((Split-Path -Parent $ackPath))
try{
    [IO.File]::WriteAllText((Join-Path $directory 'vtune-errors.log'),'Collection started.')
    $fake=[pscustomobject]@{HasExited=$false;ExitCode=0}
    $fake|Add-Member ScriptMethod WaitForExit {param($Milliseconds) $this.HasExited=$true;return $true}
    $ack=@{sequence=1;action='vtune-detach';accepted=$false;error='collector unavailable';applied_unix_ns=1}
    Write-JsonUtf8 $ackPath $ack
    $state=[ordered]@{vtune_detach_sequence=1L}
    $rejected=$false
    try{Stop-DiagnosticVtune $fake @{} $directory $state $root $relative}catch{
        if($_.Exception.Message -notmatch 'native detach rejected'){throw};$rejected=$true
    }
    if(-not $rejected -or $fake.HasExited -or $state.Contains('vtune_detached_utc')){
        throw 'A zero helper exit code must not mask a rejected native detach request.'
    }
    $ack.accepted=$true;$ack.error='';Write-JsonUtf8 $ackPath $ack
    Stop-DiagnosticVtune $fake @{} $directory $state $root $relative
    if(-not $fake.HasExited -or $null -eq $state.vtune_exit_code -or $state.vtune_exit_code -ne 0 -or -not $state.vtune_detached_utc -or
       -not(Test-Path -LiteralPath (Join-Path $directory 'vtune-control.json'))){throw 'Native detach completion evidence was not retained.'}
    Write-Host 'VTune native detach rejection and acknowledgement checks passed.'
}finally{
    Assert-ProjectPath $root $session
    foreach($child in @('diagnostics/vtune-errors.log','diagnostics/vtune-control.json','diagnostics/layers/VkLayer_override.json','control-acks/0000000000000001.json','launch.json')){
        $path=Join-Path $session $child
        if(Test-Path -LiteralPath $path){Remove-Item -LiteralPath $path -Force}
    }
    foreach($child in @('diagnostics/layers','diagnostics','control-acks')){
        $path=Join-Path $session $child
        if(Test-Path -LiteralPath $path){[IO.Directory]::Delete($path)}
    }
    [IO.Directory]::Delete($session)
}
