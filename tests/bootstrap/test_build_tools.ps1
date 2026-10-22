# SPDX-License-Identifier: GPL-2.0-or-later
# Offline parsing/dependency fixtures; no compiler, download, collector or game.
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot '../../scripts/build_tools.ps1')
$paths=Convert-CMakeToolPaths @(
    'CMAKE_COMMAND:INTERNAL=C:/existing tools/cmake.exe'
    'CMAKE_CXX_COMPILER:UNINITIALIZED=C:/编译工具/clang-cl.exe'
    'CMAKE_MAKE_PROGRAM:FILEPATH=C:/tools/equals=directory/ninja.exe'
    'CMAKE_CXX_COMPILER_ID:STRING=Clang'
    '# unrelated=ignored'
)
if($paths.Count -ne 3 -or $paths['CMAKE_COMMAND'] -cne 'C:/existing tools/cmake.exe' -or
    $paths['CMAKE_CXX_COMPILER'] -cne 'C:/编译工具/clang-cl.exe' -or
    $paths['CMAKE_MAKE_PROGRAM'] -cne 'C:/tools/equals=directory/ninja.exe'){
    throw 'CMake cache paths must preserve spaces, Unicode and equals signs, without returning a regex label.'
}
foreach($name in @('CMAKE_C_COMPILER','CMAKE_CXX_COMPILER','CMAKE_MAKE_PROGRAM','CMAKE_RC_COMPILER','CMAKE_MT','CMAKE_LINKER')){
    $argument=@(New-CMakePathArgument $name 'C:\Program Files (x86)\编译工具\equals=directory\tool.exe')
    $expected='-D'+$name+':FILEPATH=C:/Program Files (x86)/编译工具/equals=directory/tool.exe'
    if($argument.Count -ne 1 -or $argument[0] -cne $expected){
        throw 'Every CMake tool path must be one typed FILEPATH argument with forward slashes.'
    }
    if((New-CMakePathArgument $name 'C:/tools/tool.exe') -cne ('-D'+$name+':FILEPATH=C:/tools/tool.exe')){
        throw 'CMake path normalization must preserve an already normalized path.'
    }
}
$projectRoot=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
. (Join-Path $projectRoot 'scripts/tools.ps1')
$dependencyManifest=[pscustomobject]@{dependencies_root='externals';dependency_build_root='externals/prebuilt'}
$dependencyPaths=Get-ProjectDependencyPaths $projectRoot $dependencyManifest
if($dependencyPaths.sources -ne (Join-Path $projectRoot 'externals') -or
   $dependencyPaths.ffmpeg -ne (Join-Path $projectRoot 'externals/prebuilt/ffmpeg')){
    throw 'Project dependency roots did not resolve inside the project.'
}
foreach($key in @('dependencies_root','dependency_build_root')){
    foreach($outside in @('../old-project/externals','C:/external/dependencies','externals/../../outside')){
        $invalidManifest=[pscustomobject]@{dependencies_root='externals';dependency_build_root='externals/prebuilt'}
        $invalidManifest.$key=$outside
        $rejected=$false
        try{Get-ProjectDependencyPaths $projectRoot $invalidManifest|Out-Null}catch{$rejected=$true}
        if(-not $rejected){throw ('Project dependency configuration escaped the project: '+$key)}
    }
}
$fixture=Resolve-ProjectChild $projectRoot ('state/offline-tests/dependencies-'+[Guid]::NewGuid().ToString('N'))
$null=[IO.Directory]::CreateDirectory($fixture)
try{
    $required=@('externals/CMakeLists.txt','externals/ffmpeg-core/include/libavcodec/avcodec.h')
    foreach($name in @('avformat','avcodec','swscale','avutil','avfilter','swresample')){$required+='externals/prebuilt/ffmpeg/lib/'+$name+'.lib'}
    $inventory=[Collections.Generic.List[string]]::new();$inventory.Add("sha256`tbytes`tpath")
    $inventoryBytes=0L
    foreach($relative in $required){
        $file=Resolve-ProjectChild $fixture $relative
        $null=[IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($file))
        [IO.File]::WriteAllText($file,'fixture',[Text.UTF8Encoding]::new($false))
        $size=[IO.FileInfo]::new($file).Length;$inventoryBytes+=$size
        $inventory.Add((Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash.ToLowerInvariant()+"`t"+$size+"`t"+$relative)
    }
    $inventoryFile=Resolve-ProjectChild $fixture 'externals/dependency-inventory.tsv'
    [IO.File]::WriteAllLines($inventoryFile,$inventory,[Text.UTF8Encoding]::new($false))
    Write-JsonUtf8 (Join-Path $fixture 'dependencies.lock.json') @{
        status='complete';inventory=@{path='externals/dependency-inventory.tsv';files=$required.Count;bytes=$inventoryBytes
            sha256=(Get-FileHash -LiteralPath $inventoryFile -Algorithm SHA256).Hash.ToLowerInvariant()}
    }
    $null=Assert-ProjectDependencies $fixture $dependencyManifest
    $changed=Resolve-ProjectChild $fixture 'externals/prebuilt/ffmpeg/lib/avcodec.lib'
    [IO.File]::WriteAllText($changed,'changed',[Text.UTF8Encoding]::new($false))
    $rejected=$false;try{Assert-ProjectDependencies $fixture $dependencyManifest|Out-Null}catch{$rejected=$true}
    if(-not $rejected){throw 'Changed dependency bytes were accepted under an unchanged inventory.'}
    [IO.File]::WriteAllText($changed,'fixture',[Text.UTF8Encoding]::new($false))
    [IO.File]::Delete((Resolve-ProjectChild $fixture 'externals/prebuilt/ffmpeg/lib/avformat.lib'))
    $rejected=$false;try{Assert-ProjectDependencies $fixture $dependencyManifest|Out-Null}catch{$rejected=$true}
    if(-not $rejected){throw 'A missing project-local dependency was accepted.'}
}finally{
    Assert-ProjectPath (Join-Path $projectRoot 'state/offline-tests') $fixture
    if([IO.Directory]::EnumerateFileSystemEntries($fixture,'*',[IO.SearchOption]::AllDirectories)|Where-Object{([IO.File]::GetAttributes($_) -band [IO.FileAttributes]::ReparsePoint) -ne 0}){throw 'Fixture cleanup refuses filesystem links.'}
    [IO.Directory]::Delete($fixture,$true)
}
Write-Host 'CMake tool paths, project-local dependency boundaries passed.'
