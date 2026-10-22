# SPDX-License-Identifier: GPL-2.0-or-later
[CmdletBinding()]
param([ValidateSet('lite','portable')][string]$Variant='lite')
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot 'tools.ps1')
. (Join-Path $PSScriptRoot 'build_tools.ps1')
$root=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$lock=$null;$log=$null;$phase='Tool discovery'
try{
    Write-Host ('Release build ('+$Variant+'): locating installed tools...')
    $tools=Get-ProjectBuildTools $root
    $build=Resolve-ProjectChild $root 'Build/Release'
    $state=Resolve-ProjectChild $root 'state/runtime'
    $null=[IO.Directory]::CreateDirectory($state)
    $lock=[IO.File]::Open((Join-Path $state 'run.lock'),[IO.FileMode]::OpenOrCreate,[IO.FileAccess]::ReadWrite,[IO.FileShare]::None)
    $null=[IO.Directory]::CreateDirectory($build)
    $receipt=Join-Path $build 'build-receipt.json'
    if(Test-Path -LiteralPath $receipt){Remove-Item -LiteralPath $receipt}
    $logPath=Resolve-ProjectChild $root 'Build/Release/build.log'
    $log=[IO.StreamWriter]::new($logPath,$false,[Text.UTF8Encoding]::new($false))
    $log.AutoFlush=$true
    Write-ProjectBuildOutput ('Release build ('+$Variant+'), started '+[DateTime]::Now.ToString('s')) $log
    Write-ProjectBuildOutput 'Full output: Build/Release/build.log (replaced on each build).' $log
    $phase='Source fingerprint'
    Write-ProjectBuildOutput '[Prepare] Reading source fingerprints...' $log
    $before=Get-SourceFingerprint $root
    $phase='Visual Studio environment'
    Write-ProjectBuildOutput '[Prepare] Loading the installed x64 Visual Studio environment...' $log
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
    $phase='Configure'
    Write-ProjectBuildOutput '[Configure] Generating Release build files...' $log
    Invoke-ProjectBuildProcess $tools.CMAKE_COMMAND $arguments $root $environment -Log $log -Activity $phase
    $phase='Compile'
    Write-ProjectBuildOutput '[Compile] Building the engine (8 parallel jobs)...' $log
    # Ninja should finish other independent translation units after a failure,
    # so one build reports all reachable compiler errors instead of one batch.
    Invoke-ProjectBuildProcess $tools.CMAKE_COMMAND @('--build',$build,'--target','shadps4','--parallel','8','--','-k','0') $root $environment -Log $log -Activity $phase -SingleLineProgress
    $phase='Build verification'
    Write-ProjectBuildOutput '[Verify] Checking source and executable fingerprints...' $log
    if((Get-SourceFingerprint $root) -ne $before){throw 'Source changed during compilation. Build again.'}
    $exe=Join-Path $build 'inFAMOUS Second Son.exe'
    if(-not(Test-Path -LiteralPath $exe -PathType Leaf)){throw 'The Release executable was not generated.'}
    $menu=Resolve-ProjectChild $root 'Build/Release/support/Ifss.Launcher.exe'
    if(-not(Test-Path -LiteralPath $menu -PathType Leaf)){throw 'The Release launch menu was not generated.'}
    $record=[ordered]@{
        schema=1;configuration='Release';source_sha256=$before
        runtime='Build/Release/inFAMOUS Second Son.exe'
        launcher='Build/Release/support/Ifss.Launcher.exe'
        launcher_sha256=(Get-FileHash -LiteralPath $menu -Algorithm SHA256).Hash.ToLowerInvariant()
        runtime_sha256=(Get-FileHash -LiteralPath $exe -Algorithm SHA256).Hash.ToLowerInvariant()
        inspector=(Join-Path $compilerBin 'llvm-readobj.exe')
        built_at_utc=[DateTime]::UtcNow.ToString('o');variant=$Variant;game_started=$false
    }
    if($Variant -eq 'portable'){
        $phase='Runtime dependencies'
        Write-ProjectBuildOutput '[Dependencies] Inspecting portable runtime DLLs...' $log
        . (Join-Path $PSScriptRoot 'runtime_dependencies.ps1')
        $null=Get-ReleaseRuntimeFiles $root $exe $record.inspector -Portable
    }
    Write-JsonUtf8 $receipt $record
    Write-ProjectBuildOutput 'Release build complete: Build/Release/inFAMOUS Second Son.exe' $log
    Write-ProjectBuildOutput ('Package next: '+$(if($Variant -eq 'portable'){'release-portable.bat'}else{'release-lite.bat'})) $log
}catch{
    $message='Build failed ['+$phase+']: '+$_.Exception.Message
    Write-Host $message -ForegroundColor Red
    if($log){
        $log.WriteLine($message)
        $log.WriteLine($_.InvocationInfo.PositionMessage)
        $log.WriteLine($_.ScriptStackTrace)
        Write-Host 'Full output: Build/Release/build.log'
    }
    exit 1
}finally{
    if($log){$log.Dispose()}
    if($lock){$lock.Dispose()}
}
