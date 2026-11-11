# SPDX-License-Identifier: GPL-2.0-or-later
# Pure gate and actual collector ordering checks. No game or native observer is started.
$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest
. (Join-Path $PSScriptRoot '../../scripts/super_environment.ps1')
$policy=@{nongame_cpu_core_equiv=1.0;unrelated_gpu_engine_percent=10.0
    disk_bytes_per_second=20MB;disk_average_queue=1.0;preflight_idle_seconds=60}
$point=@{cpu_available=$true;gpu=@{available=$true;classifier_available=$true;unrelated_max_engine_percent=1.0}
    disk_available=$true;nongame_core_equiv=0.2;disk_bytes_per_second=1024
    disk_average_queue=0.01;read_wall_ms=1.0;monitor_cpu_ms=0;last_input_idle_seconds=120}
if(-not (Test-SuperEnvironmentQuiet $point $policy -Preflight)){throw 'Quiet desktop rejected.'}
$point.gpu.available=$false
if(Test-SuperEnvironmentQuiet $point $policy){throw 'Unknown GPU load accepted as quiet.'}
$point.gpu.available=$true;$point.nongame_core_equiv=1.2
if(Test-SuperEnvironmentQuiet $point $policy){throw 'One busy external core hidden in a low total percentage.'}
$point.nongame_core_equiv=0.2;$point.last_input_idle_seconds=0
if(Test-SuperEnvironmentQuiet $point $policy -Preflight){throw 'Recent user input ignored.'}
if(-not (Test-SuperEnvironmentQuiet $point $policy)){throw 'Automated game input cannot invalidate telemetry itself.'}
$root=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
$tokens=$null;$errors=$null
$ast=[Management.Automation.Language.Parser]::ParseFile((Join-Path $root 'scripts/collect_super.ps1'),[ref]$tokens,[ref]$errors)
if($errors.Count){throw ($errors|Out-String)}
$text=$ast.Extent.Text
if($text.IndexOf('Wait-SuperEnvironmentQuiet $environmentOwner') -ge $text.IndexOf('$launcher=Start-DiagnosticProcess')){
    throw 'Environment preflight must precede game launch.'
}
$cycle=$ast.Find({param($n)$n -is [Management.Automation.Language.FunctionDefinitionAst] -and $n.Name -eq 'Invoke-SuperCycle'},$true).Extent.Text
if($cycle.IndexOf('Assert-SuperEnvironmentBeforeRelease') -ge $cycle.IndexOf("Send-SuperCommand 'button'")){
    throw 'Busy desktop must stop before super input.'
}
Write-Host 'External-load unknown/busy/idle gates and prelaunch/preinput ordering passed. No game started.'
