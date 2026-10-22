# SPDX-License-Identifier: GPL-2.0-or-later
[CmdletBinding()]
param([ValidateSet('RelWithDebInfo','Release','Debug')][string]$Configuration='RelWithDebInfo',[switch]$CleanFirst)
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot 'tools.ps1')
. (Join-Path $PSScriptRoot 'build_tools.ps1')
$root=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
try{
    $project=Read-JsonUtf8 (Join-Path $root 'project.json')
    $tools=Get-ProjectBuildTools $root
    $build=Resolve-ProjectChild $root ('Build/'+$Configuration)
    # The caller serializes builds and game runs; no shared run lock is needed.
    $volume=[IO.DriveInfo]::new([IO.Path]::GetPathRoot($root))
    if($volume.AvailableFreeSpace -lt 3GB){throw 'At least 3 GiB of free space is required before this build.'}
    $null=[IO.Directory]::CreateDirectory($build)
    Write-Host ('Build current project sources: '+$Configuration)
    Write-Host ('Output: Build/'+$Configuration+' (first build roughly 1-3 GiB, including symbols and intermediate files).')
    Write-Host ('Existing dependency sources: '+$project.dependencies_root)
    Write-Host ('Existing dependency libraries: '+$project.dependency_build_root)
    Write-Host 'Dependency paths may point outside this project. Existing sources and FFmpeg libraries are reused; no dependency download or installation is requested.'
    foreach($key in @('CMAKE_COMMAND','CMAKE_CXX_COMPILER','CMAKE_MAKE_PROGRAM')){Write-Host ($key+': '+$tools.$key)}
    $environment=Get-ProjectBuildEnvironment $tools.CMAKE_CXX_COMPILER $root
    # CMake invokes lld-link directly, so make the installed Clang builtins available.
    $resourceRoot=Join-Path (Split-Path -Parent (Split-Path -Parent $tools.CMAKE_CXX_COMPILER)) 'lib/clang'
    if(Test-Path -LiteralPath $resourceRoot -PathType Container){
        $builtins=@(Get-ChildItem -LiteralPath $resourceRoot -Directory|ForEach-Object {
            $path=Join-Path $_.FullName 'lib/windows/clang_rt.builtins-x86_64.lib'
            if(Test-Path -LiteralPath $path -PathType Leaf){$path}
        })
        if($builtins.Count -eq 1){
            $libraryPath=if($environment.ContainsKey('LIB')){$environment['LIB']}else{$env:LIB}
            $environment['LIB']=(Split-Path -Parent $builtins[0])+';'+$libraryPath
        }
    }
    $snapshotText=Invoke-ProjectNativeTool 'build-identity' @('snapshot')
    if($LASTEXITCODE -ne 0){throw 'Native source snapshot failed.'}
    $snapshot=$snapshotText|ConvertFrom-Json
    $arguments=@('-S',$root,'-B',$build,'-G','Ninja',
        ('-DCMAKE_BUILD_TYPE='+$Configuration),'-DIFSS_BUILD_ENGINE=ON','-DENABLE_TESTS=OFF',
        (New-CMakePathArgument 'CMAKE_C_COMPILER' $tools.CMAKE_CXX_COMPILER),
        (New-CMakePathArgument 'CMAKE_CXX_COMPILER' $tools.CMAKE_CXX_COMPILER),
        (New-CMakePathArgument 'CMAKE_MAKE_PROGRAM' $tools.CMAKE_MAKE_PROGRAM),'-DCMAKE_EXPORT_COMPILE_COMMANDS=ON',
        '-DFETCHCONTENT_FULLY_DISCONNECTED=ON','-DFETCHCONTENT_UPDATES_DISCONNECTED=ON',
        '-DTRACY_ENABLE=OFF','-DSHADPS4_DIAGNOSTIC_MAP=ON')
    $vtuneSdk=Join-Path ([Environment]::GetFolderPath('ProgramFilesX86')) 'Intel/oneAPI/vtune/latest/sdk'
    $vtuneCommand=Get-Command vtune.exe -CommandType Application -ErrorAction SilentlyContinue|Select-Object -First 1
    if($vtuneCommand){$vtuneSdk=Join-Path (Split-Path -Parent (Split-Path -Parent $vtuneCommand.Source)) 'sdk'}
    if(-not(Test-Path -LiteralPath (Join-Path $vtuneSdk 'lib64/libittnotify.lib'))){$vtuneSdk=''}
    $arguments+=('-DIFSS_ITT_SDK_ROOT:PATH='+$vtuneSdk.Replace('\','/'))
    $compilerBin=Split-Path -Parent $tools.CMAKE_CXX_COMPILER
    foreach($item in @(@('CMAKE_RC_COMPILER','llvm-rc.exe'),@('CMAKE_MT','llvm-mt.exe'),@('CMAKE_LINKER','lld-link.exe'))){
        $path=Join-Path $compilerBin $item[1]
        if(Test-Path -LiteralPath $path -PathType Leaf){$arguments+=(New-CMakePathArgument $item[0] $path)}
    }
    Invoke-ProjectBuildProcess $tools.CMAKE_COMMAND $arguments $root $environment
    $compileArguments=@('--build',$build,'--target','shadps4','--parallel','8')
    if($CleanFirst){$compileArguments+='--clean-first'}
    Invoke-ProjectBuildProcess $tools.CMAKE_COMMAND $compileArguments $root $environment
    # Failed configure/compile/link commands throw before this activation step.
    $null=Invoke-ProjectNativeTool 'build-identity' @('activate','--configuration',$Configuration,'--source-sha256',$snapshot.source_sha256)
    if($LASTEXITCODE -ne 0){throw 'Native build activation failed; the runtime was not switched.'}
    Write-Host ('Build succeeded. run.bat and diagnostics now use Build/'+$Configuration+'/inFAMOUS Second Son.exe.')
    Write-Host 'No game or acceptance test was started. Run diagnostics/full.bat yourself to verify runtime fixes.'
}catch{
    Write-Host ('Build failed: '+$_.Exception.Message) -ForegroundColor Red
    exit 1
}
exit 0
