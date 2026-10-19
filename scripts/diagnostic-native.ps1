# SPDX-License-Identifier: GPL-2.0-or-later
[CmdletBinding()]
param([Parameter(Mandatory)][ValidateSet('metrics','analyze-game','diagnostic-quality','analyze-timing')][string]$Module,
      [string]$ArgumentsJson='[]')
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot 'native_tools.ps1')
try{
    if($ArgumentsJson -notmatch '^\s*\['){throw 'ArgumentsJson must be a JSON array of strings.'}
    # Windows PowerShell 5.1 writes a JSON array as one pipeline object.
    # Assign first, then enumerate its elements; wrapping the pipeline adds an array level.
    $values=ConvertFrom-Json -InputObject $ArgumentsJson -ErrorAction Stop
    if($null -eq $values){$values=[object[]]@()}
    foreach($value in $values){if($value -isnot [string]){throw 'ArgumentsJson must be a JSON array of strings.'}}
    Invoke-ProjectNativeTool $Module ([string[]]$values)
    exit $LASTEXITCODE
}catch{
    [Console]::Error.WriteLine($_.Exception.Message)
    exit 1
}
