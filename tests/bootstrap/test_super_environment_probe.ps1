# SPDX-License-Identifier: GPL-2.0-or-later
# Replay the actual probe body with fail-on-input stubs. No game or files.
$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest
$tokens=$null;$errors=$null
$ast=[Management.Automation.Language.Parser]::ParseFile((Join-Path $PSScriptRoot '../../scripts/collect_super.ps1'),[ref]$tokens,[ref]$errors)
if($errors.Count){throw ($errors|Out-String)}
$definition=$ast.Find({param($n)$n -is [Management.Automation.Language.FunctionDefinitionAst] -and $n.Name -eq 'Invoke-SuperEnvironmentProbe'},$true)
Invoke-Expression $definition.Extent.Text
$environmentOwner=[pscustomobject]@{Phase='loading'}
$batch=@{environment_probe=$null;comparison_qualified=$true};$record=@{Path='unused'}
$script:elapsed=0;$script:snapshots=0;$script:wrote=$false
function Send-SuperCommand {throw 'Probe must never send charge/release/input.'}
function Invoke-SuperCycle {throw 'Probe must never enter the super cycle.'}
function Set-SuperProfile([string]$Name){if($Name -ne 'lightweight'){throw 'Probe added a profiler.'}}
function Get-SuperSnapshot {$script:snapshots++;return @{frame_count=$script:snapshots}}
function Wait-SuperSeconds([double]$Seconds){$script:elapsed+=$Seconds}
function Write-SuperRounds([string]$Path,[object[]]$Cycles){if($Cycles.Count){throw 'Probe cannot create a super round.'};$script:wrote=$true}
Invoke-SuperEnvironmentProbe 20
if($script:elapsed -ne 20 -or $script:snapshots -ne 5 -or -not $script:wrote -or
   -not $batch.environment_probe.no_release -or $batch.comparison_qualified){throw 'Probe duration or no-release contract changed.'}
$prefix=$ast.Extent.Text.Substring(0,$ast.Extent.Text.IndexOf('. (Join-Path $PSScriptRoot ''tools.ps1'')'))
$settings=[scriptblock]::Create($prefix+"`nreturn @{rounds=`$Rounds;experiment=`$Experiment}")
$valid=& $settings -EnvironmentProbeSeconds 20 -ObserveEnvironment -Profile lightweight -Experiment none
if($valid.rounds -ne 1 -or $valid.experiment -ne 'none'){throw 'Probe inherited performance A/B or extra rounds.'}
foreach($extra in @(@{FullCapture=$true},@{CriticalCapture=$true},@{Candidate='feedback-arena-pool'})){
    $caught=$false
    try{$null=& $settings -EnvironmentProbeSeconds 20 -ObserveEnvironment -Profile lightweight -Experiment none @extra}catch{$caught=$true}
    if(-not $caught){throw 'Probe was mixed with a workload candidate or full trace.'}
}
Write-Host 'Actual standing-scene probe replay sent no input, no super rounds and no profiler. No game started.'
