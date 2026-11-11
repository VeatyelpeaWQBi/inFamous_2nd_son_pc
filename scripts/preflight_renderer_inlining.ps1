# SPDX-License-Identifier: GPL-2.0-or-later
# Compare configured commands by output object (a source can belong to multiple targets).
[CmdletBinding()]
param()
Set-StrictMode -Version Latest
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot 'tools.ps1')
. (Join-Path $PSScriptRoot 'build_tools.ps1')
$root=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$build=Join-Path $root 'Build/RelWithDebInfo'
$tools=Get-ProjectBuildTools $root
$environment=Get-ProjectBuildEnvironment $tools.CMAKE_CXX_COMPILER $root
$sets=@{}
$state=@{status='configuring';game_started=$false;changed_units=@();baseline_config_restored=$false;error=$null}
$path=Join-Path $root 'state/runtime/renderer-inline-preflight.json'
try{
    foreach($value in @('OFF','ON')){
        Invoke-ProjectBuildProcess $tools.CMAKE_COMMAND @('-Wno-author','-Wno-deprecated','-S',$root,'-B',$build,
            ('-DIFSS_RENDERER_INLINE_CANDIDATE:BOOL='+$value)) $root $environment -Capture | Out-Null
        $sets[$value]=@(Read-JsonUtf8 (Join-Path $build 'compile_commands.json'))
    }
    $old=@{}
    foreach($entry in $sets.OFF){
        if(-not $entry.output -or $old.ContainsKey($entry.output)){throw 'Missing/duplicate compilation output identity.'}
        $old[$entry.output]=$entry
    }
    $expected=@('src/video_core/renderer_vulkan/vk_pipeline_cache.cpp',
        'src/video_core/renderer_vulkan/vk_rasterizer.cpp','src/video_core/buffer_cache/buffer_cache.cpp')
    foreach($entry in $sets.ON){
        if(-not $old.ContainsKey($entry.output)){throw 'Compilation output set changed.'}
        $baseline=$old[$entry.output]
        if($baseline.file -cne $entry.file){throw 'Source identity of an output changed.'}
        if($baseline.command -cne $entry.command){
            $relative=(Get-RelativePath $root $entry.file).Replace('\','/')
            if($relative -notin $expected -or [regex]::Replace($entry.command,' /Ob2(?= )','') -cne $baseline.command){
                throw ('Unexpected compilation change: '+$relative)
            }
            $state.changed_units+=@{file=$relative;output=$entry.output}
        }
    }
    if($state.changed_units.Count -ne 3 -or $sets.ON.Count -ne $sets.OFF.Count){throw 'Unexpected compiler unit count.'}
    $state.compilation_commands=$sets.OFF.Count;$state.status='passed'
}catch{
    $state.status='failed';$state.error=$_.Exception.Message
}finally{
    try{
        Invoke-ProjectBuildProcess $tools.CMAKE_COMMAND @('-Wno-author','-Wno-deprecated','-S',$root,'-B',$build,
            '-DIFSS_RENDERER_INLINE_CANDIDATE:BOOL=OFF') $root $environment -Capture | Out-Null
        $state.baseline_config_restored=$true
    }catch{$state.status='failed';$state.error=([string]$state.error+' Restore configuration: '+$_.Exception.Message)}
    $state.updated_utc=[DateTime]::UtcNow.ToString('o');Write-JsonUtf8 $path $state
}
Write-Host ('Renderer compiler preflight: '+$state.status)
if($state.status -ne 'passed'){throw $state.error}
