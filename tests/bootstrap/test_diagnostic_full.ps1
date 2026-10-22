# SPDX-License-Identifier: GPL-2.0-or-later
# Offline full-profile and output-evidence regression. No game or profiler is started.
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot '../../scripts/tools.ps1')
. (Join-Path $PSScriptRoot '../../scripts/diagnostic_tools.ps1')
$taskAdminProbe=(Get-Item Function:Test-DiagnosticAdministrator).ScriptBlock
try{
    function Test-DiagnosticAdministrator {return $false}
    $taskProbeSpec=[pscustomobject]@{Wpr='IFSSConcurrent';VtuneAnalysis='';Nsight=$false;Validation=$false;Crash=$false;ExternalCrash=$false;RenderDoc=$false}
    $taskProbeTools=[pscustomobject]@{wpr='existing-tool'}
    $taskRejected=$false
    try{Assert-DiagnosticTools $taskProbeSpec $taskProbeTools}catch{$taskRejected=$true}
    if(-not $taskRejected){throw 'A real full launch must retain its administrator requirement.'}
    Assert-DiagnosticTools $taskProbeSpec $taskProbeTools -RequireAdministrator $false
}finally{Set-Item Function:Test-DiagnosticAdministrator $taskAdminProbe}
$bounds=Get-DiagnosticCpuTimeRange ([DateTime]::Parse('2026-01-01T00:00:00Z').ToUniversalTime()) '2026-01-01T00:00:05.0898715Z' '2026-01-01T00:01:05.7286819Z'
if([Math]::Abs($bounds.begin-6.0898715) -gt 0.000001 -or $bounds.filter -ne '6.089872:64.728682'){
    throw 'CPU report time filters must preserve fractional seconds on Windows PowerShell.'
}
$done=[pscustomobject]@{HasExited=$true;ExitCode=0}
$readiness=Get-DiagnosticExitReadiness 42 $done $done $true
if(-not $readiness.vtune_completed -or $readiness.pid -ne 42){throw 'Actual completed collectors must allow orderly diagnostic exit.'}
$pending=[pscustomobject]@{HasExited=$false}
$pending|Add-Member ScriptProperty ExitCode {throw 'A pending collector exit code must not be read.'}
$rejected=$false
try{$null=Get-DiagnosticExitReadiness 42 $done $pending $true}catch{
    if($_.Exception.Message -ne 'VTune collection is not complete.'){throw};$rejected=$true
}
if(-not $rejected){throw 'A pending collector must block successful diagnostic exit.'}
$failed=[pscustomobject]@{HasExited=$true;ExitCode=1}
$rejected=$false
try{$null=Get-DiagnosticExitReadiness 42 $failed $done $true}catch{$rejected=$true}
if(-not $rejected){throw 'A failed export must block successful diagnostic exit.'}
$spec=Get-DiagnosticSpec 'full' 'nsight' 
if($spec.Wpr -ne 'IFSSConcurrent' -or -not $spec.Validation -or -not $spec.Crash -or -not $spec.Shaders -or
    $spec.RenderDoc -or -not $spec.Nsight -or $spec.VtuneAnalysis -ne 'memory-access' -or $spec.GpuProfile -ne 2){
    throw 'Full must request compatible collectors for one target.'
}
if($spec.GpuValidation -or $spec.ExternalCrash){throw 'Nsight must not be mixed with the reproduced failing shader/crash instrumentation.'}
$alternate=Get-DiagnosticSpec 'full' 'renderdoc' $true
if(-not $alternate.RenderDoc -or $alternate.Nsight -or -not $alternate.Validation -or -not $alternate.Crash -or -not $alternate.GpuValidation -or -not $alternate.ExternalCrash){throw 'The alternate full GPU backend lost a requested collector.'}
$stable=Get-DiagnosticSpec 'validation' '' $false
if($stable.GpuValidation -or -not $stable.Validation){throw 'Stable validation must retain CPU/core checks without the failing GPU shader instrumentation.'}
$frozen=Get-DiagnosticPlanSpec @{profile='validation';validation_features=@{gpu_assisted=$true}}
if(-not $frozen.GpuValidation){throw 'A saved plan must not silently follow later settings changes.'}
foreach($seconds in @(10,60,600)){
    $schedule=Get-DiagnosticCaptureSchedule 'full' $seconds
    if($null -ne $schedule.nsight_seconds -or $schedule.renderdoc_seconds -le 0 -or $schedule.renderdoc_seconds -ge $seconds){
        throw 'Both GPU backends must wait for verified scene readiness.'
    }
    $timing=Get-DiagnosticCollectorTiming 'full' 90 $seconds 240 20 900
    if($timing.phase -ne 'startup' -or $timing.vtune_seconds -lt 240 -or $timing.nsight_start_ms -ne 0 -or $timing.nsight_timeout_seconds -lt 240){
        throw 'Full CPU/memory tracing must cover boot; GPU tracing must wait for scene readiness.'
    }
}
$standaloneTiming=Get-DiagnosticCollectorTiming 'nsight' 90 60 240 20 900
if($standaloneTiming.nsight_timeout_seconds -lt 350){throw 'A startup-attached GPU controller must allow the entire scene-readiness and warmup window.'}
$null=Test-DiagnosticWprDefinition (Get-DiagnosticTools).wpr 'IFSSConcurrent'
$exporting=[pscustomobject]@{HasExited=$false}
$exporting|Add-Member ScriptProperty ExitCode {throw 'ExitCode must not be accessed during offline export.'}
if(@(Get-DiagnosticHelperExitError $exporting 'Nsight').Count){throw 'An in-progress export was treated as an exit failure.'}
if((Get-DiagnosticHelperExitError ([pscustomobject]@{HasExited=$true;ExitCode=7}) 'Nsight') -ne 'Nsight returned 7'){throw 'A completed helper failure was lost.'}
$policy=Get-DiagnosticStoragePolicy
if($policy.mode -ne 'monitor-only' -or $policy.vtune_data_limit_mib -ne 0 -or $policy.engine_log_size_limit_bytes -ne 0){
    throw 'Collectors must have no individual disk quotas.'
}
if($policy.minimum_free_bytes -ne 2GB -or $policy.sample_interval_seconds -ne 5){
    throw 'Storage monitoring must retain the low-free-space reserve and sampling interval.'
}
foreach($entry in @(@{path='diagnose.ps1';parameters=@('LogBudgetMiB','ResourceBudgetMiB')},@{path='launch.ps1';parameters=@('DiagnosticLogBudgetMiB','DiagnosticResourceBudgetMiB')})){
    $command=Get-Command (Join-Path $PSScriptRoot ('../../scripts/'+$entry.path))
    foreach($parameter in $entry.parameters){
        if($command.Parameters.ContainsKey($parameter)){throw 'An obsolete per-component budget parameter remains exposed.'}
    }
}
[xml]$wprProfile=[IO.File]::ReadAllText((Join-Path $PSScriptRoot '../../scripts/diagnostic.wprp'))
if(@($wprProfile.WindowsPerformanceRecorder.Profiles.Profile|Where-Object {$_.LoggingMode -ne 'File'}).Count){
    throw 'WPR must stream to files instead of discarding early events in circular memory mode.'
}
$providers=$wprProfile.WindowsPerformanceRecorder.Profiles.SystemProvider
$concurrent=$providers|Where-Object {$_.Id -eq 'Concurrent'}
$cpu=$providers|Where-Object {$_.Id -eq 'CPU'}
if('SampledProfile' -in @($concurrent.Keywords.Keyword.Value) -or 'CSwitch' -notin @($concurrent.Stacks.Stack.Value) -or 'ReadyThread' -notin @($concurrent.Stacks.Stack.Value)){
    throw 'Concurrent full must preserve wait/scheduling stacks without requesting a conflicting timer sampler.'
}
if('SampledProfile' -notin @($cpu.Keywords.Keyword.Value)) {throw 'Standalone CPU mode must retain instruction sampling.'}
$wprArguments=@(Get-DiagnosticWprArguments 'IFSSAll' 'IFSS_test' 'session path with spaces/temp')
if($wprArguments -notcontains '-filemode' -or $wprArguments[-1] -ne 'session path with spaces/temp'){
    throw 'WPR must use file mode and keep all temporary files under the session.'
}
$projectRoot=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
$testRoot=Resolve-ProjectChild $projectRoot ('state/offline-tests/'+[Guid]::NewGuid().ToString('N'))
$null=[IO.Directory]::CreateDirectory($testRoot)
$paths=@('log/shad_log.txt','user/shader/dumps/shader.spv','diagnostics/timeline.etl',
    'user/captures/frame.rdc','user/captures/nsight/frame.ngfx-gputrace','diagnostics/vtune/data.0/samples.bin')
try{
    $start=[Diagnostics.ProcessStartInfo]::new()
    $start.EnvironmentVariables['DISABLE_VULKAN_RENDERDOC_CAPTURE_1_46']='1'
    Set-DiagnosticEnvironment $start $testRoot ([pscustomobject]@{profile='full';gpu_capture_backend='renderdoc'})
    if($start.EnvironmentVariables['ENABLE_VULKAN_RENDERDOC_CAPTURE'] -ne '1' -or
        $start.EnvironmentVariables['CDL_OUTPUT_PATH'] -ne (Join-Path $testRoot 'diagnostics/cdl') -or
        $start.EnvironmentVariables['DISABLE_GAMEPP_LAYER'] -ne '1' -or
        $start.EnvironmentVariables['CDL_WATCHDOG_TIMEOUT_MS'] -ne '225000' -or
        $start.EnvironmentVariables['VK_LUNARG_CRASH_DIAGNOSTIC_OUTPUT_PATH'] -ne (Join-Path $testRoot 'diagnostics/cdl') -or
        $start.EnvironmentVariables['VK_LUNARG_CRASH_DIAGNOSTIC_WATCHDOG_TIMEOUT_MS'] -ne '225000' -or
        $start.EnvironmentVariables['VK_INSTANCE_LAYERS'] -notmatch '(^|;)VK_LAYER_RENDERDOC_Capture(;|$)' -or
        $start.EnvironmentVariables.ContainsKey('DISABLE_VULKAN_RENDERDOC_CAPTURE_1_46')){
        throw 'RenderDoc must be enabled and crash output must remain inside this session.'
    }
    $layerText=[IO.File]::ReadAllText((Join-Path $testRoot 'vk_layer_settings.txt'))
    if($layerText -notmatch 'lunarg_crash_diagnostic.output_path = diagnostics/cdl' -or
        $layerText -notmatch 'lunarg_crash_diagnostic.watchdog_timeout_ms = 225000'){
        throw 'Crash settings must be available in the private working directory.'
    }
    foreach($setting in @('track_semaphores = true','dump_queue_submits = pending',
        'dump_command_buffers = pending','dump_commands = pending','dump_shaders = on_crash')){
        if(-not $layerText.Contains('lunarg_crash_diagnostic.'+$setting)){
            throw ('Crash evidence would be incomplete: '+$setting)
        }
    }
    $sensorArguments=@(Get-DiagnosticSensorArguments $testRoot)
    if($sensorArguments.Count -ne 2 -or $sensorArguments[-1] -ne '--format=csv,nounits' -or $sensorArguments -contains '--loop=1'){
        throw 'Sensors must use completed one-shot queries rather than a killable CSV writer.'
    }
    foreach($case in @(@{game=$null;collector=0;expected=1},@{game=0;collector=1;expected=1},
        @{game=7;collector=0;expected=7},@{game=0;collector=0;expected=0})){
        if((Get-DiagnosticResultCode $case.game $case.collector) -ne $case.expected){
            throw 'Collector failure or an unknown game exit code was reported as success.'
        }
    }
    $cleanIssues=@{fatal_kind=$null;error_count=0;critical_count=0}
    $gpuIssues=@{fatal_kind='gpu-device-lost';error_count=5;critical_count=3}
    $gpuOutcome=Get-DiagnosticOutcome -2147483645 'partial' @('Missing collector output: renderdoc') $gpuIssues
    if($gpuOutcome.status -ne 'failed' -or $gpuOutcome.game_exit_hex -ne '0x80000003' -or
       $gpuOutcome.failure_kind -ne 'gpu-device-lost' -or $gpuOutcome.errors[0] -notlike 'Runtime failure:*'){
        throw 'A device-loss assertion must be a game failure with the original Windows exit code.'
    }
    $nonfatal=Get-DiagnosticOutcome 0 'collected' @() @{fatal_kind=$null;error_count=4;critical_count=0}
    if($nonfatal.status -ne 'collected' -or $nonfatal.runtime_health -ne 'findings-present' -or
        $nonfatal.runtime_error_count -ne 4 -or -not $nonfatal.runtime_warnings.Count){
        throw 'Nonfatal engine findings must be retained separately from data collection failures.'
    }
    $earlyCapture=Get-DiagnosticOutcome 0 'no-window' @() $cleanIssues
    if($earlyCapture.status -eq 'collected'){throw 'A startup artifact cannot establish gameplay capture completion.'}
    $unknownExit=Get-DiagnosticOutcome $null 'collected' @() $cleanIssues
    if($unknownExit.status -ne 'failed'){throw 'An unknown process exit cannot establish successful capture.'}
    $paths+='diagnostics/vtune-errors.log'
    $liveLog=Join-Path $testRoot 'diagnostics/vtune-errors.log'
    $null=[IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($liveLog))
    $writing=[IO.File]::Open($liveLog,[IO.FileMode]::Create,[IO.FileAccess]::Write,[IO.FileShare]::Read)
    try{
        $bytes=[Text.Encoding]::UTF8.GetBytes('Collection started.')
        $writing.Write($bytes,0,$bytes.Length);$writing.Flush()
        if((Read-DiagnosticSharedText $liveLog) -ne 'Collection started.'){
            throw 'A running collector log could not be read without blocking its writer.'
        }
    }finally{$writing.Dispose()}

    $state=@{target_pid=2147483647;target_started_utc='2026-01-01T00:00:00Z';wpr_saved=$true;vtune_started=$true;vtune_exit_code=0;nsight_exit_code=0}
    $auto=@{capture_triggered=$true}
    $plan=@{profile='full'}
    $empty=@(Get-DiagnosticCoverage $projectRoot $testRoot $plan $state $auto)
    if(@($empty|Where-Object produced).Count){throw 'Requested flags must never count as captured data.'}
    $fixture=Join-Path $testRoot 'log/shad_log.txt'
    $null=[IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($fixture))
    [IO.File]::WriteAllText($fixture,(@(
        '[Render_Vulkan] <Error> (Game:Main) vk_platform.cpp:57 DebugUtilsCallback: VUID-test: invalid'
        '[Render_Vulkan] <Critical> (Game:Main) vk_scheduler.cpp:213 lambda: GPU crash diagnostics: device lost during semaphore wait'
        '[Debug] <Critical> (Game:Main) vk_semaphore.cpp:77 Wait: Assertion Failed!'
        'Device lost while waiting for the GPU'
    ) -join [Environment]::NewLine))
    $runtimeIssues=Get-DiagnosticRuntimeIssues $projectRoot $testRoot
    if($runtimeIssues.fatal_kind -ne 'gpu-device-lost' -or $runtimeIssues.error_count -ne 1 -or
        $runtimeIssues.critical_count -ne 2 -or @($runtimeIssues.groups|Where-Object id -eq 'VUID-test').Count -ne 1){
        throw 'Runtime diagnostics must retain fatal device loss, counted errors and source locations.'
    }
    [IO.File]::WriteAllText($fixture,(@(
        '[Debug] <Critical> (shadPS4:PresentThread) vk_swapchain.cpp:144 AcquireNextImage: Assertion Failed!'
        'Swapchain availability wait failed: ErrorDeviceLost'
    ) -join [Environment]::NewLine))
    $fenceFailure=Get-DiagnosticRuntimeIssues $projectRoot $testRoot
    if($fenceFailure.fatal_kind -ne 'gpu-device-lost' -or $fenceFailure.critical_count -ne 1){
        throw 'A device-lost result on an assertion continuation line was lost.'
    }
    [IO.File]::WriteAllText($fixture,(@(
        '[Render_Vulkan] <Info> () vk_platform.cpp:95 DebugUtilsCallback: CDL: Failed to read submit semaphore. Result: -4, VkQueue: test'
        '[Render_Vulkan] <Error> () vk_platform.cpp:95 DebugUtilsCallback: CDL: Device error encountered and log being recorded'
    ) -join [Environment]::NewLine))
    $cdlFailure=Get-DiagnosticRuntimeIssues $projectRoot $testRoot
    $zeroExitFailure=Get-DiagnosticOutcome 0 'collected' @() $cdlFailure
    if($cdlFailure.fatal_kind -ne 'gpu-device-lost' -or $zeroExitFailure.status -ne 'failed'){
        throw 'A normal application exit masked the crash layer device-lost result.'
    }
    [IO.File]::WriteAllText($fixture,'[Debug] <Critical> (shadPS4:PresentThread) signals.cpp:438 SignalHandler: Unhandled Exception code 0xc0000005 at 0x1000')
    $nativeFailure=Get-DiagnosticRuntimeIssues $projectRoot $testRoot
    if($nativeFailure.fatal_kind -ne 'native-access-violation' -or
        (Get-DiagnosticOutcome 0 'collected' @() $nativeFailure).status -ne 'failed'){
        throw 'A native access violation was not preserved as a fatal runtime failure.'
    }
    foreach($relative in $paths){
        $path=Join-Path $testRoot $relative
        Assert-ProjectPath $projectRoot $path
        $null=[IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($path))
        [IO.File]::WriteAllText($path,'fixture',[Text.UTF8Encoding]::new($false))
    }
    $present=@(Get-DiagnosticCoverage $projectRoot $testRoot $plan $state $auto)
    if(@($present|Where-Object {$_.requested -and -not $_.produced}).Count){throw 'Full output inventory lost a requested collector.'}
    if(@($present|Where-Object {$_.target_pid -ne $state.target_pid -or $_.target_started_utc -ne $state.target_started_utc}).Count){throw 'All collector evidence must retain one target identity.'}
    $state.vtune_data_limited=$true
    $limited=@(Get-DiagnosticCoverage $projectRoot $testRoot $plan $state $auto)|Where-Object name -eq 'vtune'
    if(-not $limited.produced -or -not $limited.data_limited -or $limited.gameplay_window_coverage_verified){
        throw 'A capped VTune result must be retained and explicitly marked incomplete.'
    }
    $state.nsight_exit_code=1
    $failed=@(Get-DiagnosticCoverage $projectRoot $testRoot $plan $state $auto)
    if(($failed|Where-Object name -eq 'nsight').produced){throw 'A failed capture tool must not be accepted just because a file exists.'}
    [IO.File]::WriteAllText((Join-Path $testRoot 'user/captures/frame.rdc'),'')
    $emptyFrame=@(Get-DiagnosticCoverage $projectRoot $testRoot $plan $state $auto)
    if(($emptyFrame|Where-Object name -eq 'renderdoc').produced){throw 'An empty frame file must not count as output.'}
    Write-Host 'Single-process full profile, capture scheduling and output-evidence checks passed.'
}finally{
    Assert-ProjectPath $projectRoot $testRoot
    foreach($relative in @($paths)+@('vk_layer_settings.txt')){$path=Join-Path $testRoot $relative;if(Test-Path -LiteralPath $path){Remove-Item -LiteralPath $path -Force}}
    foreach($relative in @('user/shader/dumps','user/shader','user/captures/nsight','user/captures','user','diagnostics/vtune/data.0','diagnostics/vtune','diagnostics/cdl','diagnostics','log')){
        $path=Join-Path $testRoot $relative
        if(Test-Path -LiteralPath $path){[IO.Directory]::Delete($path)}
    }
    [IO.Directory]::Delete($testRoot)
}
