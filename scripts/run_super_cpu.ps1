# SPDX-License-Identifier: GPL-2.0-or-later
# Elevated one-release entry. No retry, warmup, build or Full collection.
Set-StrictMode -Version Latest
$ErrorActionPreference='Stop'
$projectRoot=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
. (Join-Path $PSScriptRoot 'tools.ps1')
$status=Join-Path $projectRoot 'state/super-cpu-entry.json'
$log=Join-Path $projectRoot 'state/super-cpu-run.log'
$receipt=[ordered]@{status='running';pid=$PID;started_utc=[DateTime]::UtcNow.ToString('o');finished_utc=$null;exit_code=$null;error=$null}
Write-JsonUtf8 $status $receipt
$code=1
try{
    & "$env:WINDIR\System32\WindowsPowerShell\v1.0\powershell.exe" -NoLogo -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot 'collect_super.ps1') -CpuHotspots -Profile lightweight -Rounds 1 -Experiment none *> $log
    $code=$LASTEXITCODE
    if($code -ne 0){throw ('One-release CPU diagnostic failed with exit '+$code+'. See state/super-cpu-run.log.')}
    $receipt.status='collected'
}catch{
    $receipt.status='failed';$receipt.error=$_.Exception.Message
}finally{
    $receipt.exit_code=$code;$receipt.finished_utc=[DateTime]::UtcNow.ToString('o')
    Write-JsonUtf8 $status $receipt
}
exit $code
