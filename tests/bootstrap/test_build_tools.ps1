# SPDX-License-Identifier: GPL-2.0-or-later
# Pure cache parsing regression; does not create files or start any tool.
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
Write-Host 'CMake tool-path parsing and typed path argument checks passed.'
