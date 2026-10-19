[CmdletBinding()]
param([switch]$RuntimeOnly,[switch]$NativeOnly)
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot 'tools.ps1')
Initialize-ProjectNativeTools -IncludeTests
[Ifss.Native.BootstrapTests]::Run([IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..')))
[Ifss.Native.NativeBuildTests]::Run([IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..')))
if($RuntimeOnly){exit 0}
[Ifss.Native.NativeMetricTests]::Run()
[Ifss.Native.NativeTimingTests]::Run([IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..')))
[Ifss.Native.NativeQualityTests]::Run([IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..')))
if($NativeOnly){exit 0}
$testRoot=Join-Path $PSScriptRoot '..\tests\bootstrap'
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

& (Join-Path $testRoot 'test_native_entry.ps1')
