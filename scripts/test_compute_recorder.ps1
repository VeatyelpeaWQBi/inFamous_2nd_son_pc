# SPDX-License-Identifier: GPL-2.0-or-later
# Explicit user-run CPU checks only. Does not start the game or GPU capture.
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
# The prepared patch must be applied and build.ps1 configured first.
Invoke-ProjectBuildProcess $tools.CMAKE_COMMAND @('--build',$build,'--target','ifss_compile_readiness_test','--parallel','2') $root $environment
& (Join-Path $build 'ifss_compile_readiness_test.exe')
if($LASTEXITCODE -ne 0){throw 'Shared pipeline-readiness concurrency checks failed.'}
$powershell=Join-Path $env:WINDIR 'System32/WindowsPowerShell/v1.0/powershell.exe'
foreach($script in @('tests/bootstrap/test_feedback_variants.ps1','tests/bootstrap/test_super_capture.ps1')){
    & $powershell -NoLogo -NoProfile -ExecutionPolicy Bypass -File (Join-Path $root $script)
    if($LASTEXITCODE -ne 0){throw ('Offline collection/isolation checks failed: '+$script)}
}
& (Resolve-InstalledPython) -X utf8 -B (Join-Path $root 'tests/bootstrap/test_feedback_pair.py')
if($LASTEXITCODE -ne 0){throw 'Recorder pair identity/CPU freshness checks failed.'}
Write-Host 'CPU-only checks passed. Runtime ordering, image correctness and performance remain unverified.'
