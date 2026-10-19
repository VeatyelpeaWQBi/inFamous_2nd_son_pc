[CmdletBinding()]
param([Parameter(Mandatory)][double]$StartSeconds,[Parameter(Mandatory)][double]$EndSeconds,
      [string]$Session,[switch]$SceneConfirmed,[switch]$BrightnessConfirmed,
      [switch]$ParticlesConfirmed,[switch]$ResolutionConfirmed)
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot 'tools.ps1')
$projectRoot=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$session=Read-ProjectSession $projectRoot $Session
$sessionRoot=$session.Path
$launch=$session.Launch
if($launch.PSObject.Properties['diagnostic'] -and $launch.diagnostic){
    throw 'Instrumented diagnostic sessions cannot be accepted as performance benchmarks. Use a normal or frozen benchmark session.'
}
$arguments=@('-B',(Join-Path $PSScriptRoot 'metrics.py'),(Get-RelativePath $projectRoot (Join-Path $sessionRoot 'frames.csv')),
    '--start',$StartSeconds.ToString([Globalization.CultureInfo]::InvariantCulture),
    '--end',$EndSeconds.ToString([Globalization.CultureInfo]::InvariantCulture),'--mode',$launch.mode,
    '--output',(Get-RelativePath $projectRoot (Join-Path $sessionRoot 'measurement.json')))
if($SceneConfirmed){$arguments+='--scene-confirmed'}
if($BrightnessConfirmed){$arguments+='--brightness-confirmed'}
if($ParticlesConfirmed){$arguments+='--particles-confirmed'}
if($ResolutionConfirmed){$arguments+='--resolution-confirmed'}
$python=Resolve-InstalledPython
& $python @arguments
exit $LASTEXITCODE
