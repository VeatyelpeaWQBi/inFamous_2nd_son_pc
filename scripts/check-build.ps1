[CmdletBinding()]
param()
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot 'tools.ps1')
. (Join-Path $PSScriptRoot 'build_tools.ps1')
$projectRoot=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$project=Read-JsonUtf8 (Join-Path $projectRoot 'project.json')
$dependencies=Assert-ProjectDependencies $projectRoot $project
$tools=Get-ProjectBuildTools $projectRoot
foreach($key in @('CMAKE_COMMAND','CMAKE_CXX_COMPILER','CMAKE_MAKE_PROGRAM')){
    Write-Host ($key+': '+$tools.$key)
}
Write-Host ('Project-local dependency sources: '+$project.dependencies_root)
Write-Host ('Runtime helpers: Windows PowerShell / .NET Framework '+[Environment]::Version.ToString())
Write-Host ('Project-local dependency libraries: '+$project.dependency_build_root+'/ffmpeg/lib')
Write-Host 'Check only: no configure, compiler, dependency download, or new EXE was invoked. Run build.bat to build this project.'
