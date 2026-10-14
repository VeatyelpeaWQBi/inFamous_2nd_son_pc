[CmdletBinding()]
param()
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot 'tools.ps1')
$python=Resolve-InstalledPython
& $python -B (Join-Path $PSScriptRoot 'exit_menu.py')
exit $LASTEXITCODE