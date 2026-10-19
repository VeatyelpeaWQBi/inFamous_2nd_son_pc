[CmdletBinding()]
param()
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot 'tools.ps1')
Invoke-ProjectNativeTool 'exit-menu'
exit $LASTEXITCODE
