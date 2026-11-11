# SPDX-License-Identifier: GPL-2.0-or-later
# Explicit user-run validation. No game, GPU capture or dependency installation.
[CmdletBinding()]
param()
Set-StrictMode -Version Latest
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot 'tools.ps1')
. (Join-Path $PSScriptRoot 'build_tools.ps1')
$root=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$build=Join-Path $root 'Build/RelWithDebInfo'
if(-not(Test-Path -LiteralPath (Join-Path $build 'CMakeCache.txt'))){throw 'Build the project first with build.bat.'}
$tools=Get-ProjectBuildTools $root
$environment=Get-ProjectBuildEnvironment $tools.CMAKE_CXX_COMPILER $root
Invoke-ProjectBuildProcess $tools.CMAKE_COMMAND @('--build',$build,'--target','ifss_feedback_policy_test','--parallel','2') $root $environment
& (Join-Path $build 'ifss_feedback_policy_test.exe')
if($LASTEXITCODE -ne 0){throw 'Feedback C++ policy checks failed.'}
& (Join-Path $env:WINDIR 'System32/WindowsPowerShell/v1.0/powershell.exe') -NoLogo -NoProfile -ExecutionPolicy Bypass -File (Join-Path $root 'tests/bootstrap/test_feedback_variants.ps1')
if($LASTEXITCODE -ne 0){throw 'Feedback variant isolation checks failed.'}
& (Resolve-InstalledPython) -X utf8 -B (Join-Path $root 'tests/bootstrap/test_feedback_pair.py')
if($LASTEXITCODE -ne 0){throw 'Feedback pair qualification checks failed.'}
Write-Host 'Feedback offline checks passed. Runtime correctness and performance still require the separate native pair.'
