[CmdletBinding()]
param([switch]$RuntimeOnly)
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot 'tools.ps1')
Initialize-ProjectNativeTools -IncludeTests
[Ifss.Native.BootstrapTests]::Run([IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..')))
[Ifss.Native.NativeBuildTests]::Run([IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..')))
if($RuntimeOnly){exit 0}
$python=Resolve-InstalledPython
$testRoot=Join-Path $PSScriptRoot '..\tests\bootstrap'
& $python -B -m unittest discover -s $testRoot
if($LASTEXITCODE -ne 0){exit $LASTEXITCODE}
& (Join-Path $testRoot 'test_tools.ps1')
& (Join-Path $testRoot 'test_build_tools.ps1')
& (Join-Path $testRoot 'test_diagnostic_automation.ps1')
& (Join-Path $testRoot 'test_diagnostic_screen.ps1')
& (Join-Path $testRoot 'test_diagnostic_process.ps1')
& (Join-Path $testRoot 'test_diagnostic_full.ps1')
& (Join-Path $testRoot 'test_diagnostic_vtune.ps1')
& (Join-Path $testRoot 'test_diagnostic_sensors.ps1')
& (Join-Path $testRoot 'test_diagnostic_usage.ps1')
& (Join-Path $testRoot 'test_diagnostic_retention.ps1')
& (Join-Path $testRoot 'test_control_protocol.ps1')
