[CmdletBinding()]
param()
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot 'tools.ps1')
$testRoot=Join-Path $PSScriptRoot '..\tests\bootstrap'
Initialize-ProjectNativeTools -IncludeTests
[Ifss.Native.BootstrapTests]::Run([IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..')))
& (Join-Path $testRoot 'test_tools.ps1')
& (Join-Path $testRoot 'test_native_entry.ps1')
