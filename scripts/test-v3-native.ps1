# SPDX-License-Identifier: GPL-2.0-or-later
# Explicitly executed by the user-started suite, never by importing a helper.
$ErrorActionPreference='Stop'
$taskRoot=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
. (Join-Path $PSScriptRoot 'tools.ps1')
. (Join-Path $PSScriptRoot 'build_tools.ps1')
$taskTools=Get-ProjectBuildTools $taskRoot
$taskEnvironment=Get-ProjectBuildEnvironment $taskTools.CMAKE_CXX_COMPILER $taskRoot
$taskResource=Join-Path (Split-Path -Parent (Split-Path -Parent $taskTools.CMAKE_CXX_COMPILER)) 'lib/clang'
$taskBuiltins=@(Get-ChildItem -LiteralPath $taskResource -Directory|ForEach-Object {$taskLib=Join-Path $_.FullName 'lib/windows/clang_rt.builtins-x86_64.lib';if(Test-Path -LiteralPath $taskLib){$taskLib}})
if($taskBuiltins.Count -eq 1){$taskEnvironment['LIB']=(Split-Path -Parent $taskBuiltins[0])+';'+$taskEnvironment['LIB']}
$taskTargets=@('ifss_recording_workers_test','ifss_copy_completion_test','ifss_buffer_bind_cache_test',
    'ifss_readback_dependencies_test','ifss_optimization_v3_test','ifss_descriptor_state_test','ifss_pipeline_layout_key_test',
    'ifss_consumer_evidence_test','ifss_path_trace_queue_test','ifss_feedback_write_history_test','ifss_lighting_cse_test','ifss_sort_write_bounds_test')
Invoke-ProjectBuildProcess $taskTools.CMAKE_COMMAND (@('--build',(Join-Path $taskRoot 'Build/RelWithDebInfo'),'--target')+$taskTargets+@('--parallel','8')) $taskRoot $taskEnvironment
foreach($taskTarget in $taskTargets){
    & (Join-Path $taskRoot ('Build/RelWithDebInfo/'+$taskTarget+'.exe'))
    if($LASTEXITCODE -ne 0){throw ('Native regression failed: '+$taskTarget)}
    Write-Host ($taskTarget+': passed')
}
