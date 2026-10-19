[CmdletBinding()]
param()
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot 'tools.ps1')
. (Join-Path $PSScriptRoot 'build_tools.ps1')
$projectRoot=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$project=Read-JsonUtf8 (Join-Path $projectRoot 'project.json')
$dependenciesRoot=Resolve-ProjectPath $projectRoot $project.dependencies_root
$tools=Get-ProjectBuildTools $projectRoot
foreach($key in @('CMAKE_COMMAND','CMAKE_CXX_COMPILER','CMAKE_MAKE_PROGRAM')){
    Write-Host ($key+': '+$tools.$key)
}
if(-not (Test-Path -LiteralPath (Join-Path $dependenciesRoot 'CMakeLists.txt'))){throw 'Existing dependency sources are missing.'}
Write-Host ('Existing dependencies: '+$project.dependencies_root)
Write-Host ('Runtime helpers: Windows PowerShell / .NET Framework '+[Environment]::Version.ToString())
Write-Host ('Existing dependency libraries: '+$project.dependency_build_root)
Write-Host 'Check only: no configure, compiler, dependency download, or new EXE was invoked. Run build.bat to build this project.'
