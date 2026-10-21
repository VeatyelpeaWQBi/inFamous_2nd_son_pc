# SPDX-License-Identifier: GPL-2.0-or-later
[CmdletBinding()]
param([Parameter(Mandatory)][string]$Case,
      [ValidateSet('1080p','pro4k')][string]$Mode='pro4k',
      [ValidateSet('none','full')][string]$Diagnostic='none',
      [string]$ResultPath,
      [switch]$PrepareOnly)
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot 'v3_profiles.ps1')
$taskCandidates=@()
if($Case -eq 'combined-2'){$taskCandidates=@(Get-V3CombinedCandidates 2)}
elseif($Case -eq 'combined-4'){$taskCandidates=@(Get-V3CombinedCandidates 4)}
elseif($Case -ne 'control'){$taskCandidates=@($Case)}
$null=Get-V3CandidateOverrides $taskCandidates
$taskArguments=@{Mode=$Mode;NoMenu=$true;Diagnostic=$Diagnostic;PrepareOnly=[bool]$PrepareOnly}
if($Diagnostic -eq 'full'){$taskArguments.PreserveDiagnosticSessions=$true}
if($ResultPath){$taskArguments.ResultPath=$ResultPath}
if($Case -eq 'control'){$taskArguments.OptimizationV3='control'}
else{$taskArguments.OptimizationV3='custom';$taskArguments.V3Candidates=$taskCandidates}
& (Join-Path $PSScriptRoot 'launch.ps1') @taskArguments
exit $LASTEXITCODE
