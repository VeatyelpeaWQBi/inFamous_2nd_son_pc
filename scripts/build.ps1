# SPDX-License-Identifier: GPL-2.0-or-later
[CmdletBinding()]
param([ValidateSet('local','portable')][string]$Variant='local')
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot 'tools.ps1')
. (Join-Path $PSScriptRoot 'build_tools.ps1')
$root=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$lock=$null
try{
    $tools=Get-ProjectBuildTools $root
    $build=Resolve-ProjectChild $root 'Build/Release'
    $state=Resolve-ProjectChild $root 'state/runtime'
    $null=[IO.Directory]::CreateDirectory($state)
    $lock=[IO.File]::Open((Join-Path $state 'run.lock'),[IO.FileMode]::OpenOrCreate,[IO.FileAccess]::ReadWrite,[IO.FileShare]::None)
    $null=[IO.Directory]::CreateDirectory($build)
    $receipt=Join-Path $build 'build-receipt.json'
    if(Test-Path -LiteralPath $receipt){Remove-Item -LiteralPath $receipt}
    $before=Get-SourceFingerprint $root
    $environment=Get-ProjectBuildEnvironment $tools.CMAKE_CXX_COMPILER $root
    $compilerBin=Split-Path -Parent $tools.CMAKE_CXX_COMPILER
    $resourceRoot=Join-Path (Split-Path -Parent $compilerBin) 'lib/clang'
    $builtins=@(Get-ChildItem -LiteralPath $resourceRoot -Directory|ForEach-Object {
        $path=Join-Path $_.FullName 'lib/windows/clang_rt.builtins-x86_64.lib'
        if(Test-Path -LiteralPath $path -PathType Leaf){$path}
    })
    if($builtins.Count -eq 1){
        $libraryPath=if($environment.ContainsKey('LIB')){$environment['LIB']}else{$env:LIB}
        $environment['LIB']=(Split-Path -Parent $builtins[0])+';'+$libraryPath
    }
    $arguments=@('-S',$root,'-B',$build,'-G','Ninja','-DCMAKE_BUILD_TYPE=Release',
        '-DIFSS_BUILD_ENGINE=ON','-DENABLE_TESTS=OFF','-DTRACY_ENABLE=OFF',
        '-DSHADPS4_DIAGNOSTIC_MAP=OFF','-DFETCHCONTENT_FULLY_DISCONNECTED=ON',
        '-DFETCHCONTENT_UPDATES_DISCONNECTED=ON',
        (New-CMakePathArgument 'CMAKE_C_COMPILER' $tools.CMAKE_CXX_COMPILER),
        (New-CMakePathArgument 'CMAKE_CXX_COMPILER' $tools.CMAKE_CXX_COMPILER),
        (New-CMakePathArgument 'CMAKE_MAKE_PROGRAM' $tools.CMAKE_MAKE_PROGRAM))
    foreach($item in @(@('CMAKE_RC_COMPILER','llvm-rc.exe'),@('CMAKE_MT','llvm-mt.exe'),@('CMAKE_LINKER','lld-link.exe'))){
        $path=Join-Path $compilerBin $item[1]
        if(Test-Path -LiteralPath $path -PathType Leaf){$arguments+=(New-CMakePathArgument $item[0] $path)}
    }
    Invoke-ProjectBuildProcess $tools.CMAKE_COMMAND $arguments $root $environment
    Invoke-ProjectBuildProcess $tools.CMAKE_COMMAND @('--build',$build,'--target','shadps4','--parallel','8') $root $environment
    if((Get-SourceFingerprint $root) -ne $before){throw 'Source changed during compilation. Build again.'}
    $exe=Join-Path $build 'inFAMOUS Second Son.exe'
    if(-not(Test-Path -LiteralPath $exe -PathType Leaf)){throw 'The Release executable was not generated.'}
    $record=[ordered]@{
        schema=1;configuration='Release';source_sha256=$before
        runtime='Build/Release/inFAMOUS Second Son.exe'
        runtime_sha256=(Get-FileHash -LiteralPath $exe -Algorithm SHA256).Hash.ToLowerInvariant()
        inspector=(Join-Path $compilerBin 'llvm-readobj.exe')
        built_at_utc=[DateTime]::UtcNow.ToString('o');variant=$Variant;game_started=$false
    }
    if($Variant -eq 'portable'){
        . (Join-Path $PSScriptRoot 'runtime_dependencies.ps1')
        $null=Get-ReleaseRuntimeFiles $root $exe $record.inspector -Portable
    }
    Write-JsonUtf8 $receipt $record
    Write-Host 'Release build complete: Build/Release/inFAMOUS Second Son.exe'
    Write-Host ('Package next: '+$(if($Variant -eq 'portable'){'release-portable.bat'}else{'release.bat'}))
}catch{Write-Host ('Build failed: '+$_.Exception.Message) -ForegroundColor Red;exit 1}
finally{if($lock){$lock.Dispose()}}
