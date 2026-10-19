[CmdletBinding()]
param()
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot 'tools.ps1')
$projectRoot=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$project=Get-Content -LiteralPath (Join-Path $projectRoot 'project.json') -Raw|ConvertFrom-Json
$dependencyBuildRoot=Resolve-ProjectPath $projectRoot $project.dependency_build_root
$dependenciesRoot=Resolve-ProjectPath $projectRoot $project.dependencies_root
$cachePath=Join-Path $dependencyBuildRoot 'CMakeCache.txt'
$configured=@{}
foreach($line in Get-Content -LiteralPath $cachePath){
    if($line -match '^(CMAKE_(COMMAND|CXX_COMPILER|MAKE_PROGRAM)):[^=]+=(.*)$'){$configured[$Matches[1]]=$Matches[2]}
}
foreach($key in @('CMAKE_COMMAND','CMAKE_CXX_COMPILER','CMAKE_MAKE_PROGRAM')){
    if(-not $configured.ContainsKey($key) -or -not (Test-Path -LiteralPath $configured[$key] -PathType Leaf)){throw ('Missing installed build tool: '+$key)}
    Write-Host ($key+': '+$configured[$key])
}
if(-not (Test-Path -LiteralPath (Join-Path $dependenciesRoot 'CMakeLists.txt'))){throw 'Existing dependency sources are missing.'}
Write-Host ('Existing dependencies: '+$project.dependencies_root)
Write-Host ('Existing Python: '+(Resolve-InstalledPython))
Write-Host 'BAT phase: no configure, compiler, dependency download, or new EXE was invoked.'
