# SPDX-License-Identifier: GPL-2.0-or-later
# Explicit user-run CPU checks. Does not launch the game or create shader/GPU caches.
[CmdletBinding()]
param()
Set-StrictMode -Version Latest
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot 'tools.ps1')
. (Join-Path $PSScriptRoot 'build_tools.ps1')
$root=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$build=Join-Path $root 'Build/RelWithDebInfo'
$tools=Get-ProjectBuildTools $root
$environment=Get-ProjectBuildEnvironment $tools.CMAKE_CXX_COMPILER $root
# build.ps1 must configure the newly added excluded test target first.
Invoke-ProjectBuildProcess $tools.CMAKE_COMMAND @('--build',$build,'--target','ifss_gds_counter_plan_test','--parallel','2') $root $environment
$fixtures=@('0x00000000c73c37c1_0.spv','0x0000000028e4c3cf_0.spv','0x0000000028e4c3cf_1.spv',
    '0x000000007e071b87_0.spv','0x000000007e071b87_1.spv')|ForEach-Object{
    $path=Join-Path $root ('state/runtime/cache/CUSA00309/pipeline-v2/'+$_)
    if(-not(Test-Path -LiteralPath $path -PathType Leaf)){throw ('Retained fixture missing: '+$path)}
    $path
}
& (Join-Path $build 'ifss_gds_counter_plan_test.exe') @fixtures
if($LASTEXITCODE -ne 0){throw 'GDS module proof checks failed.'}
& (Join-Path $env:WINDIR 'System32/WindowsPowerShell/v1.0/powershell.exe') -NoLogo -NoProfile -ExecutionPolicy Bypass -File (Join-Path $root 'tests/bootstrap/test_feedback_variants.ps1')
if($LASTEXITCODE -ne 0){throw 'Candidate isolation checks failed.'}
& (Resolve-InstalledPython) -X utf8 -B (Join-Path $root 'tests/bootstrap/test_feedback_pair.py')
if($LASTEXITCODE -ne 0){throw 'Pair qualification checks failed.'}
Write-Host 'CPU-only checks passed. Native rendering and performance remain unverified until a separately authorized pair.'
