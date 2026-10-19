[CmdletBinding()]
param()
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot 'tools.ps1')
$python=Resolve-InstalledPython
$testRoot=Join-Path $PSScriptRoot '..\tests\bootstrap'
& $python -B -m unittest discover -s $testRoot
if($LASTEXITCODE -ne 0){exit $LASTEXITCODE}
& (Join-Path $testRoot 'test_tools.ps1')
