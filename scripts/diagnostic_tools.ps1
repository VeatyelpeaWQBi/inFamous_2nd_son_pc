# SPDX-License-Identifier: GPL-2.0-or-later
# Dot-source after tools.ps1. Importing this file never starts a collector.
. (Join-Path $PSScriptRoot 'diagnostic_automation.ps1')
. (Join-Path $PSScriptRoot 'diagnostic_retention.ps1')
. (Join-Path $PSScriptRoot 'diagnostic_artifacts.ps1')
. (Join-Path $PSScriptRoot 'diagnostic_sensors.ps1')
$script:DiagnosticNames=@('cpu','gpu','memory','compute','draw','validation','crash','shaders','renderdoc','nsight','vtune','vtune-memory','all','full')
function Get-DiagnosticSpec([string]$Name,[string]$FullGpuCollector='',[Nullable[bool]]$GpuAssisted=$null) {
    if($Name -notin $script:DiagnosticNames){throw 'Unknown diagnostic profile.'}
    if(($Name -eq 'full' -and -not $FullGpuCollector) -or $null -eq $GpuAssisted){
        $settings=Read-JsonUtf8 (Join-Path $PSScriptRoot '../diagnostics/settings.json')
        if($Name -eq 'full' -and -not $FullGpuCollector){$FullGpuCollector=[string]$settings.full_gpu_collector}
        if($null -eq $GpuAssisted){
            $GpuAssisted=$false
            if($settings.PSObject.Properties['gpu_assisted_validation']){
                if($settings.gpu_assisted_validation -isnot [bool]){throw 'gpu_assisted_validation must be a JSON boolean.'}
                $GpuAssisted=$settings.gpu_assisted_validation
            }
        }
    }
    if($Name -eq 'full' -and $FullGpuCollector -notin @('nsight','renderdoc')){throw 'full_gpu_collector must be nsight or renderdoc.'}
    $usesNsight=($Name -eq 'nsight' -or ($Name -eq 'full' -and $FullGpuCollector -eq 'nsight'))
    $validation=$Name -in @('validation','all','full')
    $crash=$Name -in @('crash','all','full')
    return [pscustomobject]@{
        Name=$Name;GpuValidation=($validation -and $GpuAssisted -and -not $usesNsight);ExternalCrash=($crash -and -not $usesNsight)

        Wpr=if($Name -eq 'full'){'IFSSConcurrent'}elseif($Name -eq 'all'){'IFSSAll'}elseif($Name -in @('gpu','compute','draw')){'IFSSGPU'}elseif($Name -eq 'memory'){'IFSSMemory'}elseif($Name -eq 'cpu'){'IFSSCPU'}else{''}
        GpuProfile=if($Name -in @('compute','all','full')){2}elseif($Name -eq 'draw'){3}else{1}
        Validation=$Name -in @('validation','all','full');Crash=$Name -in @('crash','all','full');Shaders=$Name -in @('shaders','all','full')
        RenderDoc=($Name -eq 'renderdoc' -or ($Name -eq 'full' -and $FullGpuCollector -eq 'renderdoc'))
        Nsight=($Name -eq 'nsight' -or ($Name -eq 'full' -and $FullGpuCollector -eq 'nsight'))
        VtuneAnalysis=if($Name -in @('vtune-memory','full')){'memory-access'}elseif($Name -eq 'vtune'){'hotspots'}else{''}
    }
}
function Get-DiagnosticPlanSpec($Plan) {
    $hasBackend=if($Plan -is [Collections.IDictionary]){$Plan.Contains('gpu_capture_backend')}else{[bool]$Plan.PSObject.Properties['gpu_capture_backend']}
    $hasFeatures=if($Plan -is [Collections.IDictionary]){$Plan.Contains('validation_features')}else{[bool]$Plan.PSObject.Properties['validation_features']}
    $gpuAssisted=if($hasFeatures){[bool]$Plan.validation_features.gpu_assisted}else{$null}
    $backend=if($Plan.profile -eq 'full' -and $hasBackend){$Plan.gpu_capture_backend}else{''}
    $spec=Get-DiagnosticSpec $Plan.profile $backend $gpuAssisted
    $hasWpr=if($Plan -is [Collections.IDictionary]){$Plan.Contains('wpr_profile')}else{[bool]$Plan.PSObject.Properties['wpr_profile']}
    if($hasWpr){$spec.Wpr=[string]$Plan.wpr_profile}
    return $spec
}
$script:DiagnosticStorageMonitor=$null
function Get-DiagnosticStoragePolicy {
    return [pscustomobject]@{
        mode='monitor-only';sample_interval_seconds=5;minimum_free_bytes=2GB
        retention_threshold_bytes=20000000000L
        vtune_data_limit_mib=0;engine_log_size_limit_bytes=0
    }
}
function Test-DiagnosticAdministrator {
    return ([Security.Principal.WindowsPrincipal]::new([Security.Principal.WindowsIdentity]::GetCurrent())).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
}
function Find-DiagnosticExecutable([string]$Name,[string[]]$Candidates) {
    $command=Get-Command $Name -CommandType Application -ErrorAction SilentlyContinue|Select-Object -First 1
    if($command -and $command.Source -notmatch '[\\/]\.codex[\\/]'){return $command.Source}
    foreach($candidate in $Candidates){
        if($candidate -and (Test-Path -LiteralPath $candidate -PathType Leaf)){return (Get-Item -LiteralPath $candidate).FullName}
    }
    return $null
}
function Get-DiagnosticTools {
    $x86=[Environment]::GetFolderPath('ProgramFilesX86')
    $installed=Get-ItemProperty 'HKLM:\SOFTWARE\Microsoft\Windows Kits\Installed Roots' -ErrorAction SilentlyContinue
    $kits=if($installed -and $installed.PSObject.Properties['KitsRoot10']){$installed.KitsRoot10}else{Join-Path $x86 'Windows Kits\10'}
    $wpt=Join-Path $kits 'Windows Performance Toolkit'
    $sdk=$null
    foreach($scope in @('Process','User','Machine')){
        $candidate=[Environment]::GetEnvironmentVariable('VULKAN_SDK',$scope)
        if($candidate -and (Test-Path -LiteralPath (Join-Path $candidate 'Bin\VkLayer_khronos_validation.dll'))){$sdk=$candidate;break}
    }
    if(-not $sdk){
        $layers=Get-ItemProperty 'HKLM:\SOFTWARE\Khronos\Vulkan\ExplicitLayers' -ErrorAction SilentlyContinue
        if($layers){foreach($property in $layers.PSObject.Properties){
            if($property.Name -like '*VkLayer_khronos_validation.json' -and $property.Value -eq 0){
                $sdk=Split-Path -Parent (Split-Path -Parent $property.Name);break
            }
        }}
    }
    $key=Get-Item 'HKLM:\SOFTWARE\Classes\RenderDoc.RDCCapture.1\DefaultIcon' -ErrorAction SilentlyContinue
    $rdoc=if($key){Split-Path -Parent ([string]$key.GetValue('')).Trim('"')}else{Join-Path $env:ProgramFiles 'RenderDoc'}
    $nv=@(Get-ChildItem -LiteralPath (Join-Path $env:ProgramFiles 'NVIDIA Corporation') -Directory -ErrorAction SilentlyContinue|Where-Object {$_.Name -like 'Nsight Graphics *'}|Sort-Object Name -Descending)
    $ngfxCandidates=@($nv|ForEach-Object {Join-Path $_.FullName 'host\windows-desktop-nomad-x64\ngfx.exe'})
    return [pscustomobject]@{
        wpr=Find-DiagnosticExecutable 'wpr.exe' @((Join-Path $wpt 'wpr.exe'),(Join-Path $env:WINDIR 'System32\wpr.exe'))
        wpa=Find-DiagnosticExecutable 'wpa.exe' @((Join-Path $wpt 'wpa.exe'))
        xperf=Find-DiagnosticExecutable 'xperf.exe' @((Join-Path $wpt 'xperf.exe'))
        gpuview=Find-DiagnosticExecutable 'GPUView.exe' @((Join-Path $wpt 'gpuview\GPUView.exe'))
        schema=Join-Path $wpt 'WPRControlProfiles.xsd';sdk=$sdk
        renderdoc=Find-DiagnosticExecutable 'qrenderdoc.exe' @((Join-Path $rdoc 'qrenderdoc.exe'))
        ngfx=Find-DiagnosticExecutable 'ngfx.exe' $ngfxCandidates
        vtune=Find-DiagnosticExecutable 'vtune.exe' @((Join-Path $x86 'Intel\oneAPI\vtune\latest\bin64\vtune.exe'))
        nvidia_smi=Find-DiagnosticExecutable 'nvidia-smi.exe' @((Join-Path $env:WINDIR 'System32\nvidia-smi.exe'))
    }
}
function Assert-DiagnosticTools($Spec,$Tools,[bool]$RequireAdministrator=$true) {
    if($Spec.Wpr -and -not $Tools.wpr){throw 'WPR is missing. No tools are installed automatically.'}
    if($RequireAdministrator -and ($Spec.Wpr -or $Spec.VtuneAnalysis -or $Spec.Nsight) -and -not (Test-DiagnosticAdministrator)){
        throw 'Double-click the matching BAT in diagnostics and accept the Windows administrator prompt.'
    }
    if(($Spec.Validation -or $Spec.Crash) -and -not $Tools.sdk){throw 'Cannot locate the installed Vulkan SDK layers.'}
    if($Spec.ExternalCrash -and -not (Test-Path -LiteralPath (Join-Path $Tools.sdk 'Bin\VkLayer_crash_diagnostic.dll'))){throw 'The crash diagnostic layer is missing.'}
    if($Spec.RenderDoc -and -not $Tools.renderdoc){throw 'The installed RenderDoc registry entry/executable is missing.'}
    if($Spec.Nsight -and -not $Tools.ngfx){throw 'Nsight Graphics CLI is missing.'}
    if($Spec.VtuneAnalysis -and -not $Tools.vtune){throw 'VTune CLI is missing.'}
}
function Test-DiagnosticWprDefinition([string]$Executable,[string]$Profile) {
    $start=[Diagnostics.ProcessStartInfo]::new()
    $start.FileName=$Executable
    $arguments=@('-profiledetails',((Join-Path $PSScriptRoot 'diagnostic.wprp')+'!'+$Profile),'-filemode')
    $start.Arguments=($arguments|ForEach-Object {Quote-WindowsArgument $_}) -join ' '
    $start.UseShellExecute=$false;$start.CreateNoWindow=$true
    $start.RedirectStandardOutput=$true;$start.RedirectStandardError=$true
    $process=[Diagnostics.Process]::Start($start)
    try{
        $output=$process.StandardOutput.ReadToEndAsync();$errors=$process.StandardError.ReadToEndAsync()
        if(-not $process.WaitForExit(10000)){$process.Kill();$process.WaitForExit();throw 'WPR profile inspection timed out.'}
        $text=$output.Result+$errors.Result
        if($process.ExitCode -ne 0){throw 'WPR could not load the recording profile.'}
        if($Profile -in @('IFSSAll','IFSSConcurrent','IFSSGPU')){
            if($text -notmatch '(?i)Microsoft-Windows-DxgKrnl:\s+0x([0-9a-f]+):'){throw 'WPR silently omitted the GPU kernel provider; no game was started.'}
            $mask=[Convert]::ToUInt64($Matches[1],16)
            if(($mask -band 0x0C008000) -ne 0x0C008000){throw 'WPR omitted GPU scheduling, hardware scheduling or Present keywords.'}
        }
        return $true
    }finally{$process.Dispose()}
}
function Write-DiagnosticText([string]$Root,[string]$Path,[string]$Text) {
    Assert-ProjectPath $Root $Path
    $pending=$Path+'.'+[Guid]::NewGuid().ToString('N')+'.pending'
    try{
        [IO.File]::WriteAllText($pending,$Text,[Text.UTF8Encoding]::new($false))
        Publish-ProjectFile $pending $Path
    }finally{if(Test-Path -LiteralPath $pending){Remove-Item -LiteralPath $pending -Force}}
}
function Set-DiagnosticTuning([string]$Root,[string]$Session,$Values) {
    foreach($relative in @('perf_tuning.ini','user/perf_tuning.ini')){
        $path=Resolve-ProjectChild $Root ((Get-RelativePath $Root $Session)+'/'+$relative)
        $text=if(Test-Path -LiteralPath $path){[IO.File]::ReadAllText($path)}else{''}
        foreach($key in $Values.Keys){
            $pattern='(?m)^\s*'+[regex]::Escape($key)+'\s*=.*$'
            $line=$key+' = '+[string]$Values[$key]
            if([regex]::IsMatch($text,$pattern)){$text=[regex]::Replace($text,$pattern,[Text.RegularExpressions.MatchEvaluator]{param($m) $line})}
            else{$text+=[Environment]::NewLine+$line}
        }
        Write-DiagnosticText $Root $path ($text+[Environment]::NewLine)
    }
}
function Initialize-DiagnosticSession([string]$Root,[string]$Session,[string]$Profile,[int]$DelaySeconds,[int]$Seconds,[switch]$PrepareOnly) {
    $spec=Get-DiagnosticSpec $Profile;$tools=Get-DiagnosticTools
    Assert-DiagnosticTools $spec $tools -RequireAdministrator (-not $PrepareOnly)
    if($spec.Wpr){$null=Test-DiagnosticWprDefinition $tools.wpr $spec.Wpr}
    $relative=Get-RelativePath $Root $Session
    $directory=Resolve-ProjectChild $Root ($relative+'/diagnostics')
    $null=[IO.Directory]::CreateDirectory((Join-Path $directory 'temp'))
    foreach($path in @('user/config.json','user/custom_configs/CUSA00309.json')){
        $configPath=Join-Path $Session $path
        $config=Read-JsonUtf8 $configPath
        $overrides=@{
            General=@{home_dir='home';console_language=1;volume_slider=0}
            Debug=@{debug_dump=$spec.Shaders;shader_collect=$spec.Shaders}
            GPU=@{dump_shaders=$spec.Shaders}
            Vulkan=@{
                renderdoc_enabled=$spec.RenderDoc;vkvalidation_enabled=$spec.Validation
                vkvalidation_core_enabled=$spec.Validation;vkvalidation_sync_enabled=$spec.Validation
                vkvalidation_gpu_enabled=$spec.GpuValidation;vkcrash_diagnostic_enabled=$spec.ExternalCrash
                vkhost_markers=$true;vkguest_markers=$true
            }
            Log=@{enable=$true;append=$false;separate=$false;size_limit=(Get-DiagnosticStoragePolicy).engine_log_size_limit_bytes
                filter=if($Profile -in @('all','full')){'*:Debug'}else{'*:Info Debug:Debug'};flush_level='error'}
        }
        foreach($section in $overrides.Keys){
            if(-not $config.PSObject.Properties[$section]){$config|Add-Member NoteProperty $section ([pscustomobject]@{})}
            foreach($key in $overrides[$section].Keys){$config.$section|Add-Member NoteProperty $key $overrides[$section][$key] -Force}
        }
        Write-JsonUtf8 $configPath $config
    }
    Copy-ProjectTree $Root (Join-Path $Session 'user/custom_configs/CUSA00309.json') (Join-Path $Session 'custom_configs/CUSA00309.json')
    Set-DiagnosticTuning $Root $Session ([ordered]@{
        perf_log=1;profile_stacks=1;gpu_profile=$spec.GpuProfile;gpu_profile_compute_hash=0;gpu_profile_compute_limit=0;path_trace=0;gpu_checkpoints=[int]$spec.Crash;dump_frames=0
        trace_shader='';trace_reads='';trace_draw=0;dump_images='';dump_after=''
    })
    if($spec.RenderDoc){
        $inputPath=Join-Path $Session 'user/input_config/global.ini'
        $text=[IO.File]::ReadAllText($inputPath)
        $text=[regex]::Replace($text,'(?m)^\s*hotkey_(renderdoc_capture|capture_frame)\s*=.*$','')
        Write-DiagnosticText $Root $inputPath ($text+[Environment]::NewLine+'hotkey_renderdoc_capture = f8'+[Environment]::NewLine)
    }
    $storageRoot=$Session
    $inventory=@()
    foreach($name in @('wpr','wpa','xperf','gpuview','renderdoc','ngfx','vtune','nvidia_smi')){
        $path=$tools.$name
        $inventory+=@{name=$name;available=[bool]$path;version=if($path){(Get-Item -LiteralPath $path).VersionInfo.FileVersion}else{$null}}
    }
    $plan=[ordered]@{
        schema=3;profile=$Profile;session=$relative;scope='single-game-process';game_launch_limit=1;wpr_profile=$spec.Wpr
        wpr_instance=('IFSS_'+[Guid]::NewGuid().ToString('N'));delay_seconds=$DelaySeconds;seconds=$Seconds
        storage=(Get-DiagnosticStoragePolicy);storage_root=(Get-RelativePath $Root $storageRoot)
        cache_policy='session-private-cold';saves_published=$false;tools=$inventory;audio_muted=$true
        collectors=@{wpr=[bool]$spec.Wpr;renderdoc=$spec.RenderDoc;nsight=$spec.Nsight;vtune_analysis=$spec.VtuneAnalysis;sensors_optional=$true}
        capture_schedule=(Get-DiagnosticCaptureSchedule $Profile $Seconds)
        collector_timing=(Get-DiagnosticCollectorTiming $Profile $DelaySeconds $Seconds 240 20 900)
        capture_combination_verified=$false;restart_on_collector_failure=$false
        gpu_capture_backend=if($spec.Nsight){'nsight'}elseif($spec.RenderDoc){'renderdoc'}else{'engine'}
        nsight_injection=if($spec.Nsight){'native-sdk-before-vulkan; validation-library-loaded-first'}else{$null}
        nsight_exit_policy=if($spec.Nsight){'engine-shutdown-self-terminate'}else{$null}
        incompatible_gpu_collectors='Nsight injection blocks RenderDoc DLL loading; one GPU capture backend per process'
        validation_features=@{core=$spec.Validation;sync=$spec.Validation;gpu_assisted=$spec.GpuValidation;external_crash_layer=$spec.ExternalCrash;engine_checkpoints=$spec.Crash}
        validation_scope='enabled validation layer call chain; raw profiler-generated validation errors retained'
        compatibility_exclusions=@(
            if($Profile -eq 'full'){
                'WPR timer CPU sampling conflicts with concurrent VTune hardware sampling; target CPU hotspots are required from time-filtered VTune EBS; ETW scheduling stacks remain enabled'
            }
            if($spec.Nsight -and $Profile -eq 'full'){
                'RenderDoc DLL blocked by Nsight injection'
                'External crash interception excluded from Nsight target; available in crash/all BATs'
            }
            if($spec.Validation -and -not $spec.GpuValidation){
                'GPU-assisted validation disabled after isolated device-loss reproduction; core/sync validation retained; advanced opt-in in diagnostics/settings.json'
            }
        )
        excluded_implicit_layers=@('VK_LAYER_GAMEPP');crash_watchdog_timeout_ms=225000
        cpu_hotspots_sources=if($Profile -eq 'full'){@('VTune time-filtered target process/function CPU samples','ETW CSwitch/ReadyThread stacks','Engine thread statistics')}else{@()}
        cpu_sampling_backend=if($Profile -eq 'full'){'vtune-hardware'}else{'wpr-timer'}
        same_frame_across_capture_tools=$false
        automation=@{enabled=$true;boot_timeout_seconds=240;warmup_seconds=20;session_timeout_seconds=900;language='en-US';input_acknowledgement='native per-command application receipts';scene_verification='heuristic evidence only'}
        host_timezone=[TimeZoneInfo]::Local.Id
        matching_runtime_pdb_verified=$false;performance_acceptance=$false
        quality_contract='event-content-time-coverage-and-clock-v2'
        nsight_trace_scope='3-frame-detail-snapshot; full-window GPU scheduling via ETW';nsight_detail_frames=3;bandwidth_calibration_requested=($spec.VtuneAnalysis -eq 'memory-access')
        vtune_stop_method=if($spec.VtuneAnalysis){'native-itt-detach'}else{$null}
        storage_policy='no component or session quotas; monitor all session files; stop only for low free disk space'
        prepared_at=(Get-Date).ToUniversalTime().ToString('o')
    }
    Write-JsonUtf8 (Join-Path $directory 'plan.json') $plan
    return $plan
}
function Set-DiagnosticEnvironment($Start,[string]$Session,$Plan) {
    $tools=Get-DiagnosticTools
    $Start.EnvironmentVariables['SHADPS4_PERF_SAMPLER']='1'
    # The installed GamePP manifest provides this target-only opt-out. Its shader hook
    # is unrelated to collection and changes swapchain/private-data behavior.
    $Start.EnvironmentVariables['DISABLE_GAMEPP_LAYER']='1'
    $Start.EnvironmentVariables['TEMP']=Join-Path $Session 'diagnostics/temp'
    $Start.EnvironmentVariables['TMP']=Join-Path $Session 'diagnostics/temp'
    $spec=Get-DiagnosticPlanSpec $Plan
    $Start.EnvironmentVariables['IFSS_NSIGHT_ACTIVE']=if($spec.Nsight){'1'}else{'0'}
    $Start.EnvironmentVariables.Remove('IFSS_VTUNE_CONTROL')
    $Start.EnvironmentVariables.Remove('INTEL_LIBITTNOTIFY64')
    if($spec.VtuneAnalysis){
        $collector=Join-Path (Split-Path -Parent $tools.vtune) 'runtime/ittnotify_collector.dll'
        if(-not(Test-Path -LiteralPath $collector -PathType Leaf)){throw 'The installed VTune ITT collector is missing.'}
        $Start.EnvironmentVariables['IFSS_VTUNE_CONTROL']='1'
        $Start.EnvironmentVariables['INTEL_LIBITTNOTIFY64']=$collector
    }
    $layers=@()
    if($spec.Validation -or $spec.ExternalCrash){$layers+=Join-Path $tools.sdk 'Bin'}
    if($spec.ExternalCrash){
        $crashPath=Join-Path $Session 'diagnostics/cdl'
        Assert-ProjectPath ([IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))) $crashPath
        $null=[IO.Directory]::CreateDirectory($crashPath)
        $Start.EnvironmentVariables['CDL_OUTPUT_PATH']=$crashPath
        # Heavy instrumentation can legitimately delay the first submission.
        # Retain a hang dump before the automatic boot deadline; device-loss dumps stay enabled.
        $Start.EnvironmentVariables['CDL_WATCHDOG_TIMEOUT_MS']='225000'
        $Start.EnvironmentVariables['VK_LUNARG_CRASH_DIAGNOSTIC_OUTPUT_PATH']=$crashPath
        $Start.EnvironmentVariables['VK_LUNARG_CRASH_DIAGNOSTIC_WATCHDOG_TIMEOUT_MS']='225000'
        # A settings file in this private working directory also covers native layer builds
        # that do not recognize the legacy CDL_* environment aliases.
        $settingsPath=Join-Path $Session 'vk_layer_settings.txt'
        if(Test-Path -LiteralPath $settingsPath){throw 'Refusing to replace an existing private Vulkan layer settings file.'}
        Write-DiagnosticText ([IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))) $settingsPath (
            'lunarg_crash_diagnostic.output_path = diagnostics/cdl'+[Environment]::NewLine+
            'lunarg_crash_diagnostic.watchdog_timeout_ms = 225000'+[Environment]::NewLine+
            'lunarg_crash_diagnostic.track_semaphores = true'+[Environment]::NewLine+
            'lunarg_crash_diagnostic.dump_queue_submits = pending'+[Environment]::NewLine+
            'lunarg_crash_diagnostic.dump_command_buffers = pending'+[Environment]::NewLine+
            'lunarg_crash_diagnostic.dump_commands = pending'+[Environment]::NewLine+
            'lunarg_crash_diagnostic.dump_shaders = on_crash'+[Environment]::NewLine)
    }
    if($spec.RenderDoc){
        $layers+=Split-Path -Parent $tools.renderdoc
        $Start.EnvironmentVariables['ENABLE_VULKAN_RENDERDOC_CAPTURE']='1'
        $requestedLayers=@($Start.EnvironmentVariables['VK_INSTANCE_LAYERS'] -split ';'|Where-Object {$_})
        if($requestedLayers -notcontains 'VK_LAYER_RENDERDOC_Capture'){$requestedLayers+='VK_LAYER_RENDERDOC_Capture'}
        $Start.EnvironmentVariables['VK_INSTANCE_LAYERS']=$requestedLayers -join ';'
        # The manifest's versioned disable key also applies to this target only.
        $manifestPath=Join-Path (Split-Path -Parent $tools.renderdoc) 'renderdoc.json'
        $manifest=[IO.File]::ReadAllText($manifestPath)|ConvertFrom-Json
        foreach($property in $manifest.layer.disable_environment.PSObject.Properties){
            if($property.Name -like 'DISABLE_VULKAN_RENDERDOC_CAPTURE*'){$Start.EnvironmentVariables.Remove($property.Name)}
        }
    }
    $Start.EnvironmentVariables.Remove('IFSS_NSIGHT_DURATION_MS')
    $Start.EnvironmentVariables.Remove('IFSS_NSIGHT_INSTALL')
    $Start.EnvironmentVariables.Remove('IFSS_NSIGHT_SAFE_EXIT')
    $Start.EnvironmentVariables.Remove('IFSS_VALIDATION_LIBRARY')
    if($spec.Nsight){
        $Start.EnvironmentVariables['IFSS_NSIGHT_DURATION_MS']='9000'
        $install=Split-Path -Parent (Split-Path -Parent (Split-Path -Parent $tools.ngfx))
        $Start.EnvironmentVariables['IFSS_NSIGHT_INSTALL']=$install
        $Start.EnvironmentVariables['IFSS_NSIGHT_SAFE_EXIT']='1'
        if($spec.Validation){$Start.EnvironmentVariables['IFSS_VALIDATION_LIBRARY']=Join-Path $tools.sdk 'Bin/VkLayer_khronos_validation.dll'}
        $layers+=Join-Path $install 'target/windows-desktop-nomad-x64'
    }
    if($layers.Count){$Start.EnvironmentVariables['VK_LAYER_PATH']=($layers|Select-Object -Unique) -join ';'}
}
function Start-DiagnosticRuntime($Start,[string]$Root,[string]$Session,$Plan,[ref]$Wrapper) {
    # The engine uses the installed SDK before Vulkan initialization. A launcher
    # injected before validation reverses their DLL teardown order and aborts.
    $Wrapper.Value=$null
    return [Diagnostics.Process]::Start($Start)
}
function Get-DiagnosticResultCode($GameExitCode,[int]$CollectorExitCode) {
    if($null -eq $GameExitCode){return 1}
    if([int]$GameExitCode -ne 0){return [int]$GameExitCode}
    return $CollectorExitCode
}
function Get-DiagnosticRuntimeIssues([string]$Root,[string]$Session) {
    Assert-ProjectPath $Root $Session
    $groups=@{};$sources=@();$errorCount=0L;$criticalCount=0L;$otherGroups=0L;$fatal=$null
    foreach($relative in @('log/shad_log.txt','log/CUSA00309.log','log/shadps4.log','user/log/shad_log.txt')){
        $path=Join-Path $Session $relative
        Assert-ProjectPath $Root $path
        if(-not(Test-Path -LiteralPath $path -PathType Leaf)){continue}
        $source=Get-RelativePath $Root $path;$sources+=$source
        $stream=[IO.File]::Open($path,[IO.FileMode]::Open,[IO.FileAccess]::Read,([IO.FileShare]::ReadWrite -bor [IO.FileShare]::Delete))
        $reader=$null;$number=0L
        try{
            $reader=[IO.StreamReader]::new($stream,[Text.UTF8Encoding]::new($false),$true)
            while($null -ne ($line=$reader.ReadLine())){
                $number++
                if($line -match 'GPU crash diagnostics: device lost|Device lost while waiting for the GPU|\b(?:ErrorDeviceLost|VK_ERROR_DEVICE_LOST)\b|CDL: Failed to read submit semaphore\. Result: -4\b'){$fatal='gpu-device-lost'}
                if(-not $fatal -and $line -match 'Unhandled Exception code 0xc0000005\b'){
                    $fatal='native-access-violation'
                }
                if($line -notmatch '^\[.+?\]\s+<(Error|Critical)>'){continue}
                $severity=$matches[1]
                if($severity -eq 'Critical'){$criticalCount++}else{$errorCount++}
                if(-not $fatal -and $line -match 'Assertion Failed!'){$fatal='engine-assertion'}
                $key=if($line -match '(VUID-[^:\s]+|SYNC-HAZARD-[^:\s]+)'){$matches[1]}
                     elseif($line -match '(\w+\.(?:cpp|h):\d+ \w+):'){$matches[1]}else{'unclassified-'+$severity}
                if(-not $groups.ContainsKey($key)){
                    if($groups.Count -ge 64){$otherGroups++;continue}
                    $groups[$key]=@{id=$key;count=0L;examples=@()}
                }
                $item=$groups[$key];$item.count++
                if($item.examples.Count -lt 2){
                    $item.examples+=@{source=$source;line=$number;severity=$severity;text=$line.Substring(0,[Math]::Min(1024,$line.Length))}
                }
            }
        }finally{if($reader){$reader.Dispose()}else{$stream.Dispose()}}
    }
    return @{
        schema=1;error_count=$errorCount;critical_count=$criticalCount;fatal_kind=$fatal
        sources=$sources;groups=@($groups.Values|Sort-Object id);ungrouped_occurrences=$otherGroups
        inference='Device loss identifies the failure, not its shader, layer or driver cause.'
    }
}
function Get-DiagnosticOutcome($GameExitCode,[string]$CaptureStatus,[string[]]$Errors,$RuntimeIssues) {
    $messages=@($Errors);$kind=$null;$hex=$null;$status=$CaptureStatus
    if($null -eq $GameExitCode){
        $kind='unknown-game-exit';$messages+='The game exit code is unavailable.'
    }else{
        $hex='0x{0:X8}' -f ([int]$GameExitCode)
        if([int]$GameExitCode -ne 0){$kind='game-exit';$messages+=('The game exited abnormally: '+$hex+' ('+$GameExitCode+').')}
    }
    if($RuntimeIssues.fatal_kind){
        $kind=$RuntimeIssues.fatal_kind
        $messages=@('Runtime failure: '+$kind+'; see diagnostics/runtime-errors.json.')+$messages
    }elseif($RuntimeIssues.critical_count){
        $kind='engine-critical';$messages+='The engine logged a critical runtime error; see diagnostics/runtime-errors.json.'
    }
    $runtimeWarnings=@()
    if($RuntimeIssues.error_count){
        $runtimeWarnings+=('Engine log contains '+$RuntimeIssues.error_count+' nonfatal error records; see diagnostics/runtime-errors.json.')
    }
    if($CaptureStatus -eq 'collected' -and $messages.Count){$status='partial'}
    if($kind){$status='failed'}
    return @{
        status=$status;errors=@($messages|Where-Object {$_}|Select-Object -Unique)
        game_exit_code=$GameExitCode;game_exit_hex=$hex;failure_kind=$kind
        runtime_error_count=$RuntimeIssues.error_count;runtime_critical_count=$RuntimeIssues.critical_count
        runtime_health=if($kind){'failed'}elseif($RuntimeIssues.error_count){'findings-present'}else{'clean'}
        runtime_warnings=$runtimeWarnings
    }
}
function Get-DiagnosticSensorArguments([string]$Directory) {
    return @('--query-gpu=timestamp,index,name,utilization.gpu,utilization.memory,memory.used,memory.total,power.draw,clocks.sm,clocks.mem',
        '--format=csv,nounits')
}
function Get-DiagnosticHelperExitError($Process,[string]$Name) {
    if($Process -and $Process.HasExited -and $Process.ExitCode -ne 0){return ($Name+' returned '+$Process.ExitCode)}
}
function Get-DiagnosticExitReadiness([int]$TargetPid,$Nsight,$Vtune,[bool]$RequiresVtune) {
    if(-not $Nsight -or -not $Nsight.HasExited -or $Nsight.ExitCode -ne 0){throw 'Nsight export is not complete.'}
    if($RequiresVtune -and (-not $Vtune -or -not $Vtune.HasExited -or $Vtune.ExitCode -ne 0)){
        throw 'VTune collection is not complete.'
    }
    return @{pid=$TargetPid;policy='engine-shutdown-self-terminate';nsight_exit_code=$Nsight.ExitCode;vtune_completed=$true}
}
function Start-DiagnosticProcess([hashtable]$Options) {
    $process=Start-Process @Options
    # Retain the Windows handle while the helper is alive, including its eventual exit code.
    try{$null=$process.Handle;return $process}catch{$process.Dispose();throw}
}
function Start-DiagnosticCollector([string]$Root,[string]$Session) {
    $relative=Get-RelativePath $Root $Session
    $toolArguments=@('-NoLogo','-NoProfile','-ExecutionPolicy','Bypass','-File',(Join-Path $PSScriptRoot 'diagnose.ps1'),'-Action','worker','-Session',$relative)
    $options=@{
        FilePath=(Join-Path $env:WINDIR 'System32/WindowsPowerShell/v1.0/powershell.exe')
        ArgumentList=(($toolArguments|ForEach-Object {Quote-WindowsArgument $_}) -join ' ');WorkingDirectory=$Root;PassThru=$true;WindowStyle='Hidden'
        RedirectStandardOutput=(Join-Path $Session 'diagnostics/worker-output.log');RedirectStandardError=(Join-Path $Session 'diagnostics/worker-errors.log')
    }
    $p=Start-DiagnosticProcess $options
    Write-JsonUtf8 (Join-Path $Session 'diagnostics/worker.json') @{pid=$p.Id;started=$p.StartTime.ToUniversalTime().ToString('o')}
    return $p
}
function Get-DiagnosticTarget($Launch,[string]$Root) {
    if(-not $Launch.game_launched){return $null}
    $p=Get-Process -Id $Launch.pid -ErrorAction SilentlyContinue
    if(-not $p){return $null}
    try{
        $null=$p.Handle
        if($p.HasExited){$p.Dispose();return $null}
        $expected=Resolve-ProjectChild $Root $Launch.runtime
        if(-not(Test-ProjectProcessIdentity $p $expected $Launch.process_started)){
            if($p.HasExited){$p.Dispose();return $null}
            $actual=Get-ProjectProcessIdentity $p
            throw ('Diagnostic target identity changed: PID '+$p.Id+'; expected '+$expected+' @ '+$Launch.process_started+'; observed '+$actual.Path+' @ '+[DateTime]::FromFileTimeUtc($actual.CreationFileTime).ToString('o'))
        }
        return $p
    }catch{
        if($p.HasExited){$p.Dispose();return $null}
        $p.Dispose();throw
    }
}
function Send-DiagnosticControl([string]$Root,[string]$Session,[string]$Action,[int]$Frames=2) {
    $record=Read-ProjectSession $Root $Session
    if(-not $record.Launch.PSObject.Properties['diagnostic'] -or -not $record.Launch.diagnostic){throw 'This is not a diagnostic session.'}
    $target=Get-DiagnosticTarget $record.Launch $Root
    if(-not $target){throw 'The diagnostic game process has exited.'}
    $directory=Join-Path $record.Path 'diagnostics'
    $lock=[IO.File]::Open((Join-Path $directory 'command.lock'),[IO.FileMode]::OpenOrCreate,[IO.FileAccess]::ReadWrite,[IO.FileShare]::None)
    try{
        $path=Join-Path $directory 'command.json'
        $seq=if(Test-Path -LiteralPath $path){1+[long](Read-JsonUtf8 $path).sequence}else{1}
        $queue=Join-Path $directory 'commands'
        Assert-ProjectPath $Root $queue
        $null=[IO.Directory]::CreateDirectory($queue)
        $command=@{sequence=$seq;action=$Action;frames=$Frames;target_pid=$target.Id;process_started=$record.Launch.process_started;at=(Get-Date).ToUniversalTime().ToString('o')}
        Write-JsonUtf8 (Join-Path $queue ($seq.ToString('D16')+'.json')) $command
        Write-JsonUtf8 $path $command
    }finally{$lock.Dispose()}
}
function Get-DiagnosticDirectoryEntries([string]$Directory) {
    return [IO.Directory]::EnumerateFileSystemEntries($Directory)
}
function Get-DiagnosticUsage([string]$Directory) {
    $total=0L
    $pending=[Collections.Generic.Stack[string]]::new();$pending.Push($Directory)
    while($pending.Count){
        $current=$pending.Pop()
        try{
            foreach($path in Get-DiagnosticDirectoryEntries $current){
                try{
                    $attributes=[IO.File]::GetAttributes($path)
                    if($attributes -band [IO.FileAttributes]::ReparsePoint){continue}
                    if($attributes -band [IO.FileAttributes]::Directory){$pending.Push($path)}
                    else{$total+=([IO.FileInfo]::new($path)).Length}
                }catch [IO.FileNotFoundException]{
                    # ETL/VTune can rotate a file after its name was enumerated.
                    continue
                }catch [IO.DirectoryNotFoundException]{continue}
            }
        }catch [IO.DirectoryNotFoundException]{
            if($current -eq $Directory){throw}
        }
    }
    return @{total_bytes=$total}
}
function New-DiagnosticStorageSample([long]$TotalBytes,[long]$PreviousPeakBytes,[long]$AvailableFreeBytes,[long]$MinimumFreeBytes) {
    return @{
        total_bytes=$TotalBytes;peak_total_bytes=[Math]::Max($TotalBytes,$PreviousPeakBytes)
        available_free_bytes=$AvailableFreeBytes;minimum_free_bytes=$MinimumFreeBytes
        low_space=($AvailableFreeBytes -lt $MinimumFreeBytes)
    }
}
function Update-DiagnosticStorage($Monitor,[switch]$Force) {
    if(-not $Monitor -or $Monitor.failed){return}
    $elapsed=$Monitor.clock.Elapsed.TotalSeconds
    if(-not $Force -and ($elapsed-$Monitor.last_sample_s) -lt $Monitor.policy.sample_interval_seconds){return}
    $Monitor.last_sample_s=$elapsed
    $usage=Get-DiagnosticUsage $Monitor.directory
    $sample=New-DiagnosticStorageSample $usage.total_bytes $Monitor.peak_total_bytes $Monitor.volume.AvailableFreeSpace $Monitor.policy.minimum_free_bytes
    $Monitor.peak_total_bytes=$sample.peak_total_bytes
    $Monitor.state.usage=$sample
    if($sample.low_space -and -not $Monitor.low_space){
        $Monitor.low_space=$true
        $Monitor.state.status='disk-space-stop'
        $Monitor.state.errors+='Disk free space is below the 2 GiB reserve; a normal exit was requested. Collected files are preserved.'
        try{
            Reset-DiagnosticAutoInput $Monitor.root $Monitor.session
            & (Join-Path $PSScriptRoot 'control.ps1') -Action quit -Session $Monitor.session | Out-Null
        }catch{$Monitor.state.errors+=('Low-space exit request: '+$_.Exception.Message)}
    }
    $Monitor.writer.WriteLine([string]::Join(',',@([DateTime]::UtcNow.ToString('o'),$elapsed.ToString('F3',[Globalization.CultureInfo]::InvariantCulture),$sample.total_bytes,$sample.peak_total_bytes,$sample.available_free_bytes)))
    Write-JsonUtf8 $Monitor.status_path $Monitor.state
}
function Wait-DiagnosticHelper($Process,[int]$TimeoutMilliseconds) {
    $timer=[Diagnostics.Stopwatch]::StartNew()
    while(-not $Process.WaitForExit(250)){
        try{Update-DiagnosticStorage $script:DiagnosticStorageMonitor}
        catch{
            $monitor=$script:DiagnosticStorageMonitor
            if(-not $monitor){throw}
            $monitor.failed=$true;$monitor.state.status='failed'
            $monitor.state.errors+=('Storage monitoring failed while waiting for a tool: '+$_.Exception.Message)
        }
        if($timer.Elapsed.TotalMilliseconds -ge $TimeoutMilliseconds){return $false}
    }
    return $true
}
function Get-DiagnosticWprArguments([string]$Profile,[string]$Instance,[string]$TemporaryDirectory) {
    return @('-start',((Join-Path $PSScriptRoot 'diagnostic.wprp')+'!'+$Profile),
        '-filemode','-instancename',$Instance,'-recordtempto',$TemporaryDirectory)
}
function Invoke-DiagnosticNative([string]$Executable,[string[]]$Arguments,[string]$Log,[int]$TimeoutMilliseconds=120000) {
    $start=[Diagnostics.ProcessStartInfo]::new()
    $start.FileName=$Executable;$start.Arguments=($Arguments|ForEach-Object {Quote-WindowsArgument $_}) -join ' '
    $start.UseShellExecute=$false;$start.CreateNoWindow=$true;$start.RedirectStandardOutput=$true;$start.RedirectStandardError=$true
    $p=[Diagnostics.Process]::Start($start)
    try{
        $output=$p.StandardOutput.ReadToEndAsync();$errors=$p.StandardError.ReadToEndAsync()
        if(-not (Wait-DiagnosticHelper $p $TimeoutMilliseconds)){$p.Kill();$p.WaitForExit();throw 'This session diagnostic helper timed out.'}
        $p.WaitForExit()
        [IO.File]::AppendAllText($Log,($output.Result+$errors.Result),[Text.UTF8Encoding]::new($false))
        if($p.ExitCode -ne 0){throw ('Diagnostic tool returned '+$p.ExitCode+'; see '+[IO.Path]::GetFileName($Log))}
    }finally{$p.Dispose()}
}
function Read-DiagnosticSharedText([string]$Path) {
    $stream=[IO.File]::Open($Path,[IO.FileMode]::Open,[IO.FileAccess]::Read,([IO.FileShare]::ReadWrite -bor [IO.FileShare]::Delete))
    $reader=$null
    try{
        $reader=[IO.StreamReader]::new($stream,[Text.UTF8Encoding]::new($false),$true)
        return $reader.ReadToEnd()
    }finally{if($reader){$reader.Dispose()}else{$stream.Dispose()}}
}
function Stop-DiagnosticVtune($Process,$Tools,[string]$Directory,$State,[string]$Root,[string]$Session) {
    if(-not $Process){return}
    if($Process.HasExited){$State.vtune_exit_code=$Process.ExitCode;return}
    # A startup failure can request cleanup before VTune creates its result.
    $readyClock=[Diagnostics.Stopwatch]::StartNew()
    $readyLog=Join-Path $Directory 'vtune-errors.log'
    while(-not $Process.HasExited){
        if((Test-Path -LiteralPath $readyLog) -and (Read-DiagnosticSharedText $readyLog) -match 'Collection started\.') {break}
        if($readyClock.Elapsed.TotalSeconds -ge 30){throw 'VTune did not become ready for a stop command.'}
        $null=Wait-DiagnosticHelper $Process 1000
    }
    if($Process.HasExited){$State.vtune_exit_code=$Process.ExitCode;return}
    # Detach the profiler while the target and its DLLs are still alive. Do not
    # combine target DLL teardown with a live profiler's exit notification.
    if(-not $State.Contains('vtune_detach_sequence')){
        & (Join-Path $PSScriptRoot 'control.ps1') -Action vtune-detach -Session $Session | Out-Null
        $State.vtune_detach_sequence=[long](Read-JsonUtf8 (Join-Path $Root ($Session+'/command.json'))).sequence
        $State.vtune_stop_method='native-itt-detach'
    }
    $ackPath=Resolve-ProjectChild $Root ($Session+'/control-acks/'+$State.vtune_detach_sequence.ToString('D16')+'.json')
    $ackClock=[Diagnostics.Stopwatch]::StartNew()
    while(-not(Test-Path -LiteralPath $ackPath -PathType Leaf)){
        if($ackClock.Elapsed.TotalSeconds -ge 90){throw 'VTune native detach acknowledgement timed out.'}
        Update-DiagnosticStorage $script:DiagnosticStorageMonitor
        Start-Sleep -Milliseconds 100
    }
    $ack=Read-JsonUtf8 $ackPath
    if($ack.sequence -ne $State.vtune_detach_sequence -or $ack.action -ne 'vtune-detach' -or
       -not $ack.accepted -or $ack.error -or $ack.applied_unix_ns -le 0){
        throw ('VTune native detach rejected: '+$ack.error)
    }
    Write-JsonUtf8 (Join-Path $Directory 'vtune-control.json') $ack
    if(-not (Wait-DiagnosticHelper $Process 30000)){throw 'VTune did not detach before game shutdown.'}
    $State.vtune_detached_utc=[DateTime]::UtcNow.ToString('o')
    $State.vtune_exit_code=$Process.ExitCode
    if($Process.ExitCode -ne 0){throw ('VTune returned '+$Process.ExitCode+' while detaching.')}
}
function Invoke-DiagnosticWorker([string]$Root,[string]$Session) {
    $record=Read-ProjectSession $Root $Session;$launch=$record.Launch;$sessionPath=$record.Path
    if(-not $launch.PSObject.Properties['diagnostic'] -or -not $launch.diagnostic){throw 'No diagnostic plan belongs to this session.'}
    $dir=Join-Path $sessionPath 'diagnostics'
    $plan=Read-JsonUtf8 (Join-Path $dir 'plan.json')
    if($plan.session -ne (Get-RelativePath $Root $sessionPath) -or $plan.wpr_instance -notmatch '^IFSS_[a-f0-9]{32}$'){throw 'Invalid diagnostic plan.'}
    if($plan.schema -ne 3 -or $plan.storage.mode -ne 'monitor-only'){throw 'This diagnostic plan uses an obsolete storage policy; prepare a new session.'}
    $storageRoot=Resolve-ProjectChild $Root $plan.storage_root
    if($storageRoot -ne $sessionPath){throw 'Invalid diagnostic storage root.'}
    $tools=Get-DiagnosticTools;$spec=Get-DiagnosticPlanSpec $plan
    $env:TEMP=Join-Path $dir 'temp';$env:TMP=$env:TEMP
    $state=[ordered]@{profile=$plan.profile;status='waiting';wpr_started=$false;wpr_saved=$false;errors=@();marks=@();window_started=$null;window_finished=$null;complete=$false;performance_acceptance=$false}
    $state.startup_window_started=$null;$state.startup_collectors_started=$false;$state.collector_start_errors=@{};$state.capture_files=@();$state.helpers=@();$state.scope='single-game-process';$state.vtune_started=$false;$state.vtune_exit_code=$null;$state.nsight_exit_code=$null;$state.vtune_data_limited=$false;$state.vtune_collection_finished_utc=$null
    $state.collector_started_utc=[DateTime]::UtcNow.ToString('o');$state.target_pid=$launch.pid;$state.target_started_utc=$launch.process_started
    $statusPath=Join-Path $dir 'status.json'
    $wprOwned=$false;$wprAttempted=$false;$vtune=$null;$smi=$null;$nsight=$null;$capturing=$false;$finished=$false;$sequence=0L;$drawPhase=$false
    $auto=New-DiagnosticAutomation $plan;$state.scene_confirmed=$false;$state.automation=$true
    $writer=$null;$target=$null;$captureAt=$null;$storage=$null;$tick=0;$lastCpu=$null;$lastTime=$null
    $clock=[Diagnostics.Stopwatch]::StartNew()
    $startCollectors={
        if($spec.Nsight -and -not $nsight){
            try{
                $captureDirectory=Join-Path $sessionPath 'user/captures/nsight'
                Assert-ProjectPath $Root $captureDirectory
                $null=[IO.Directory]::CreateDirectory($captureDirectory)
                $toolArguments=@('--activity','GPU Trace Profiler','--attach-pid',[string]$target.Id,
                    '--output-dir',(Join-Path $sessionPath 'user/captures/nsight'),'--start-with-ngfx-sdk',
                    '--auto-export','--max-duration-ms','9000','--limit-to-frames','3','--trace-timeout',[string]$plan.collector_timing.nsight_timeout_seconds,'--set-gpu-clocks','unaltered')
                $options=@{FilePath=$tools.ngfx;ArgumentList=(($toolArguments|ForEach-Object {Quote-WindowsArgument $_}) -join ' ');WorkingDirectory=$sessionPath;PassThru=$true;WindowStyle='Hidden';RedirectStandardOutput=(Join-Path $dir 'nsight-output.log');RedirectStandardError=(Join-Path $dir 'nsight-errors.log')}
                $nsight=Start-DiagnosticProcess $options
                $state.nsight_controller=@{pid=$nsight.Id;started=$nsight.StartTime.ToUniversalTime().ToString('o')}
                $state.helpers+=@{pid=$nsight.Id;started=$nsight.StartTime.ToUniversalTime().ToString('o');path=$tools.ngfx}
            }catch{
                if($plan.profile -ne 'full'){throw}
                $state.errors+=('Nsight startup: '+$_.Exception.Message)
                $state.collector_start_errors['Nsight']=Get-DiagnosticErrorInfo $_ $Root
            }
        }
        if($plan.wpr_profile -and -not $wprAttempted){
            try{
                Write-JsonUtf8 $statusPath $state
                $wprAttempted=$true
                Invoke-DiagnosticNative $tools.wpr (Get-DiagnosticWprArguments $plan.wpr_profile $plan.wpr_instance (Join-Path $dir 'temp')) (Join-Path $dir 'wpr.log')
                $wprOwned=$true;$state.wpr_started=$true
            }catch{
                if($plan.profile -ne 'full'){throw}
                $state.errors+=('WPR startup: '+$_.Exception.Message)
                $state.collector_start_errors['WPR']=Get-DiagnosticErrorInfo $_ $Root
            }
        }
        if($spec.VtuneAnalysis -and -not $state.vtune_started){
            try{
                $analysis=$spec.VtuneAnalysis
                $vtuneUser=Join-Path $dir 'vtune-user'
                Assert-ProjectPath $Root $vtuneUser
                $null=[IO.Directory]::CreateDirectory($vtuneUser)
                $toolArguments=@('-collect',$analysis,'-target-pid',[string]$target.Id,'-duration',[string]$plan.collector_timing.vtune_seconds,'-data-limit',[string]$plan.storage.vtune_data_limit_mib,'-finalization-mode','none','-result-dir',(Join-Path $dir 'vtune'),'-user-data-dir',(Join-Path $dir 'vtune-user'))
                if($analysis -eq 'memory-access'){$toolArguments+=@('-knob','dram-bandwidth-limits=true')}
                if($analysis -eq 'hotspots'){$toolArguments+=@('-knob','sampling-mode=hw','-knob','enable-stack-collection=true')}
                $options=@{FilePath=$tools.vtune;ArgumentList=(($toolArguments|ForEach-Object {Quote-WindowsArgument $_}) -join ' ');WorkingDirectory=$sessionPath;PassThru=$true;WindowStyle='Hidden';RedirectStandardOutput=(Join-Path $dir 'vtune-output.log');RedirectStandardError=(Join-Path $dir 'vtune-errors.log')}
                $vtune=Start-DiagnosticProcess $options
                $state.vtune_started=$true;$state.vtune_started_utc=[DateTime]::UtcNow.ToString('o')
                $state.helpers+=@{pid=$vtune.Id;started=$vtune.StartTime.ToUniversalTime().ToString('o');path=$tools.vtune}
            }catch{
                if($plan.profile -ne 'full'){throw}
                $state.errors+=('VTune startup: '+$_.Exception.Message)
                $state.collector_start_errors['VTune']=Get-DiagnosticErrorInfo $_ $Root
            }
        }
        if($tools.nvidia_smi -and -not $smi){
            try{
                $smi=New-DiagnosticSensors $tools.nvidia_smi $dir
                Update-DiagnosticSensors $smi
            }catch{
                if($plan.profile -ne 'full'){throw}
                $state.errors+=('NVIDIA sensors startup: '+$_.Exception.Message)
                $state.collector_start_errors['NVIDIA sensors']=Get-DiagnosticErrorInfo $_ $Root
            }
        }
    }
    try{
        $storageWriter=[IO.StreamWriter]::new((Join-Path $dir 'storage.csv'),$false,[Text.UTF8Encoding]::new($false));$storageWriter.AutoFlush=$true
        $storageWriter.WriteLine('utc,elapsed_s,total_bytes,peak_total_bytes,available_free_bytes')
        $storage=[pscustomobject]@{
            directory=$storageRoot;root=$Root;session=$Session;state=$state;status_path=$statusPath
            clock=$clock;last_sample_s=-10.0;policy=$plan.storage;peak_total_bytes=0L;low_space=$false;failed=$false
            volume=[IO.DriveInfo]::new([IO.Path]::GetPathRoot($storageRoot));writer=$storageWriter
        }
        $script:DiagnosticStorageMonitor=$storage
        Update-DiagnosticStorage $storage -Force
        if($storage.low_space){throw 'Insufficient disk space for diagnostic collection.'}
        $writer=[IO.StreamWriter]::new((Join-Path $dir 'process.csv'),$false,[Text.UTF8Encoding]::new($false));$writer.AutoFlush=$true
        $writer.WriteLine('utc,elapsed_s,cpu_percent_of_machine,private_bytes,working_set_bytes,virtual_bytes,threads,handles')
        $target=Get-DiagnosticTarget $launch $Root
        if($target){
            try{Write-JsonUtf8 (Join-Path $dir 'modules.json') @($target.Modules|ForEach-Object {@{module=$_.ModuleName;base_address=('0x{0:x}' -f $_.BaseAddress.ToInt64());bytes=$_.ModuleMemorySize}})}
            catch{$state.errors+=('Module inventory: '+$_.Exception.Message)}
        }
        Write-JsonUtf8 $statusPath $state
        if(($plan.profile -eq 'full' -or $spec.Nsight) -and $target){
            # Full records startup too. Scene readiness gates input and RenderDoc, not tracing.
            $state.startup_window_started=[DateTime]::UtcNow.ToString('o')
            . $startCollectors
            $state.startup_collectors_started=$true
            Write-JsonUtf8 $statusPath $state
        }
        # The validated handle continues to identify this process even after its PID is recycled.
        while($target -and -not $target.HasExited){
            $target.Refresh()
            if($smi){Update-DiagnosticSensors $smi}
            if(Test-Path -LiteralPath (Join-Path $dir 'launcher-finished')){break}
            $now=[DateTime]::UtcNow;$elapsed=$clock.Elapsed.TotalSeconds;$cpu=$target.TotalProcessorTime.TotalSeconds
            $percent=if($null -ne $lastCpu){100*($cpu-$lastCpu)/([Environment]::ProcessorCount*($now-$lastTime).TotalSeconds)}else{0}
            $writer.WriteLine([string]::Join(',',@($now.ToString('o'),$elapsed.ToString('F3',[Globalization.CultureInfo]::InvariantCulture),$percent.ToString('F3',[Globalization.CultureInfo]::InvariantCulture),$target.PrivateMemorySize64,$target.WorkingSet64,$target.VirtualMemorySize64,$target.Threads.Count,$target.HandleCount)))
            $lastCpu=$cpu;$lastTime=$now;$command=$null
            if($nsight -and $nsight.HasExited -and $nsight.ExitCode -ne 0){
                $details=Get-Content -LiteralPath (Join-Path $dir 'nsight-output.log') -Tail 8 -ErrorAction SilentlyContinue
                throw ('Nsight controller failed: '+($details -join ' '))
            }
            if($state.Contains('nsight_trigger_sequence') -and -not $state.Contains('nsight_sdk_accepted')){
                $ackPath=Join-Path $sessionPath ('control-acks/'+([long]$state.nsight_trigger_sequence).ToString('D16')+'.json')
                if(Test-Path -LiteralPath $ackPath -PathType Leaf){
                    Assert-ProjectPath $Root $ackPath
                    $ack=Read-JsonUtf8 $ackPath
                    if($ack.sequence -ne $state.nsight_trigger_sequence -or $ack.action -ne 'nsight' -or -not $ack.accepted){throw ('Nsight native trigger failed: '+$ack.error)}
                    $state.nsight_sdk_accepted=$true
                    $state.nsight_scene_anchor_utc=[DateTimeOffset]::FromUnixTimeMilliseconds($ack.applied_unix_ms).UtcDateTime.ToString('o')
                }
            }
            if($vtune -and $vtune.HasExited -and -not $state.vtune_collection_finished_utc){$state.vtune_collection_finished_utc=$now.ToString('o')}
            $queue=Join-Path $dir 'commands'
            if(Test-Path -LiteralPath $queue){
                $next=@(Get-ChildItem -LiteralPath $queue -File -Filter '*.json'|Where-Object {$_.BaseName -match '^\d{16,19}$' -and [long]$_.BaseName -gt $sequence}|Sort-Object Name|Select-Object -First 1)
                if($next.Count){
                    $candidate=Read-JsonUtf8 $next[0].FullName
                    $sequence=[long]$next[0].BaseName
                    if($candidate.sequence -eq $sequence -and $candidate.target_pid -eq $target.Id -and $candidate.process_started -eq $launch.process_started){$command=$candidate}
                    $state.last_command_sequence=$sequence
                }
            }
            if($command -and $command.action -eq 'mark'){$state.marks+=@{utc=$now.ToString('o');elapsed_s=$elapsed};Write-JsonUtf8 $statusPath $state}
            if($command -and $command.action -eq 'xray'){
                Set-DiagnosticTuning $Root $sessionPath @{dump_frames=0}
                Start-Sleep -Milliseconds 1200
                Set-DiagnosticTuning $Root $sessionPath @{dump_frames=[Math]::Max(2,[Math]::Min(30,[int]$command.frames))}
                $state.marks+=@{utc=[DateTime]::UtcNow.ToString('o');elapsed_s=$clock.Elapsed.TotalSeconds;action='xray';frames=$command.frames};Write-JsonUtf8 $statusPath $state
            }
            if($tick % 10 -eq 0){
                $liveIssues=Get-DiagnosticRuntimeIssues $Root $sessionPath
                if($liveIssues.fatal_kind){
                    $state.status='failed'
                    $state.errors+=('Runtime failure during collection: '+$liveIssues.fatal_kind+'; capture stopped.')
                    Write-JsonUtf8 (Join-Path $dir 'runtime-errors-live.json') $liveIssues
                    break
                }
            }
            Update-DiagnosticStorage $storage;$tick++
            $stopRequested=$command -and $command.action -eq 'stop'
            if($storage.low_space -or $storage.failed){break}
            Update-DiagnosticAutomation $Root $Session $plan $auto $elapsed
            $state.automation_phase=$auto.phase;$state.scene_status=$auto.scene_status;$state.elapsed_s=$elapsed
            if($tick % 5 -eq 0){Write-JsonUtf8 $statusPath $state}
            $beginRequested=$auto.ready -and ($elapsed-$auto.ready_at) -ge $plan.automation.warmup_seconds -and
                (($command -and $command.action -eq 'capture') -or ($plan.delay_seconds -ge 0 -and $elapsed -ge $plan.delay_seconds))
            if(-not $capturing -and -not $finished -and -not $stopRequested -and $beginRequested){
                $captureAt=$elapsed;$capturing=$true;$state.status='recording';$state.window_started=$now.ToString('o')
                $auto.capture_at=$elapsed;$auto.phase='recording'
                if($spec.Nsight){
                    $state.nsight_scene_anchor_utc=$now.ToString('o')
                    & (Join-Path $PSScriptRoot 'control.ps1') -Action nsight -Session $Session | Out-Null
                    $state.nsight_trigger_sequence=[long](Read-JsonUtf8 (Join-Path $sessionPath 'command.json')).sequence
                }
                $state.marks+=@{utc=$now.ToString('o');elapsed_s=$elapsed;action='gpu_profile';value=$spec.GpuProfile}
                . $startCollectors
                Write-JsonUtf8 (Join-Path $dir 'modules.json') @($target.Modules|ForEach-Object {@{module=$_.ModuleName;base_address=('0x{0:x}' -f $_.BaseAddress.ToInt64());bytes=$_.ModuleMemorySize}})
                $state.module_inventory_utc=[DateTime]::UtcNow.ToString('o')
                Write-JsonUtf8 $statusPath $state
            }
            if($capturing -and $plan.profile -in @('all','full') -and -not $drawPhase -and ($elapsed-$captureAt) -ge ($plan.seconds/2)){
                Set-DiagnosticTuning $Root $sessionPath @{gpu_profile=3};$drawPhase=$true
                $state.marks+=@{utc=$now.ToString('o');elapsed_s=$elapsed;action='gpu_profile';value=3};Write-JsonUtf8 $statusPath $state
            }
            if($nsight -and -not $nsight.HasExited -and $capturing -and ($elapsed-$captureAt) -gt ($plan.seconds+150)){throw 'Nsight automatic capture did not finish within its bounded window.'}
            if($nsight -and $state.Contains('nsight_sdk_accepted') -and -not $state.Contains('nsight_trace_saved')){
                $traceLog=Join-Path $dir 'nsight-output.log'
                if((Read-DiagnosticSharedText $traceLog) -match 'GPU Trace report saved to '){
                    $state.nsight_trace_saved=$true
                    $state.nsight_trace_saved_utc=[DateTime]::UtcNow.ToString('o')
                }
            }
            # The target is no longer needed after the trace is saved. Offline export can
            # take several minutes; it must not extend the gameplay window or keep the GPU loaded.
            $captureToolsReady=(-not $nsight -or ($state.Contains('nsight_trace_saved') -and $state.nsight_trace_saved)) -and (-not $spec.RenderDoc -or $auto.capture_completed)
            if($capturing -and ($elapsed-$captureAt) -gt ($plan.seconds+90)){
                throw 'Capture completion timed out: input acknowledgements or GPU frame output are missing.'
            }
            if(-not $finished -and ($stopRequested -or ($capturing -and ($elapsed-$captureAt) -ge $plan.seconds -and $captureToolsReady -and $auto.workload_completed))){
                if(-not $state.window_finished){$state.window_finished=[DateTime]::UtcNow.ToString('o')}
                $state.stop_reason=if($stopRequested){'requested-stop'}else{'completed-window-and-applied-workload'}
                # Stop the game while the trace is still recording; ETL merging must not
                # leave heavy GPU instrumentation running for another two minutes.
                Stop-DiagnosticVtune $vtune $tools $dir $state $Root $Session
                if($spec.Nsight -and $plan.nsight_exit_policy -eq 'engine-shutdown-self-terminate'){
                    if(-not (Wait-DiagnosticHelper $nsight 1800000)){throw 'Nsight export did not finish before diagnostic exit.'}
                    $readiness=Get-DiagnosticExitReadiness $target.Id $nsight $vtune ([bool]$spec.VtuneAnalysis)
                    $state.nsight_exit_code=$nsight.ExitCode
                    Write-JsonUtf8 (Join-Path $dir 'exit-ready.json') $readiness
                }
                Reset-DiagnosticAutoInput $Root $Session
                & (Join-Path $PSScriptRoot 'control.ps1') -Action quit -Session $Session | Out-Null
                if(-not (Wait-DiagnosticHelper $target 30000)){throw 'The game did not exit after the acknowledged workload.'}
                if($wprOwned){
                    Invoke-DiagnosticNative $tools.wpr @('-stop',(Join-Path $dir 'timeline.etl'),'-instancename',$plan.wpr_instance) (Join-Path $dir 'wpr.log')
                    $wprOwned=$false;$state.wpr_saved=$true
                }
                $finished=$true;$capturing=$false;$state.status=if($state.window_started){'collected'}else{'no-window'}
                if($vtune -and $vtune.HasExited -and $vtune.ExitCode -ne 0){$state.errors+=('VTune returned '+$vtune.ExitCode)}
                $auto.phase='exiting';$state.errors+=@($auto.warnings)
                if($auto.event_index -lt $auto.events.Count){$state.errors+='The workload window ended before all scheduled inputs could be queued.'}
                $state.errors+=@(Get-DiagnosticHelperExitError $nsight 'Nsight')
                Set-DiagnosticTuning $Root $sessionPath @{dump_frames=0;gpu_profile=1};Write-JsonUtf8 $statusPath $state
                Write-JsonUtf8 (Join-Path $dir 'automation.json') $auto
                break
            }
            Start-Sleep -Milliseconds 1000
        }
    }catch{
        $state.status='failed';$state.errors+=$_.Exception.Message
        $state.error_details=Get-DiagnosticErrorInfo $_ $Root
        # The final quit command is queued after cleanup, so camera reset cannot overwrite it.
    }
    finally{
        try{Stop-DiagnosticVtune $vtune $tools $dir $state $Root $Session}catch{$state.errors+=('Profiler detach: '+$_.Exception.Message)}
        try{Reset-DiagnosticAutoInput $Root $Session}catch{$state.errors+=('Input cleanup: '+$_.Exception.Message)}
        try{
            $live=Get-DiagnosticTarget $launch $Root
            if($live){
                try{
                    & (Join-Path $PSScriptRoot 'control.ps1') -Action quit -Session $Session | Out-Null
                    if(-not (Wait-DiagnosticHelper $live 30000)){$state.errors+='Game shutdown is still pending during collector cleanup.'}
                }finally{$live.Dispose()}
            }
        }catch{$state.errors+=('Early cleanup exit: '+$_.Exception.Message)}
        if($storage -and $storage.low_space){
            try{& (Join-Path $PSScriptRoot 'control.ps1') -Action quit -Session $Session | Out-Null}
            catch{$state.errors+=('Low-space cleanup exit: '+$_.Exception.Message)}
        }
        if($nsight){
            if(-not $nsight.HasExited -and -not (Wait-DiagnosticHelper $nsight 1800000)){$nsight.Kill();$nsight.WaitForExit();$state.errors+='Stopped this session Nsight helper after timeout.'}
            if($nsight.ExitCode -ne 0){$state.errors+=('Nsight returned '+$nsight.ExitCode)}
            $state.nsight_exit_code=$nsight.ExitCode
            $nsight.Dispose()
        }
        if($wprOwned -or ($wprAttempted -and -not $state.wpr_saved)){
            try{
                Invoke-DiagnosticNative $tools.wpr @('-stop',(Join-Path $dir 'timeline.etl'),'-instancename',$plan.wpr_instance) (Join-Path $dir 'wpr.log')
                $wprOwned=$false;$state.wpr_saved=$true
            }catch{$state.errors+=$_.Exception.Message}
        }
        if($target){$target.Dispose()}
        if($smi){Stop-DiagnosticSensors $smi;$state.sensors=@{rows=$smi.rows;errors=@($smi.errors)}}
        if($vtune){
            if(-not $vtune.HasExited){
                if(-not (Wait-DiagnosticHelper $vtune 30000)){$vtune.Kill();$vtune.WaitForExit();$state.errors+='VTune cleanup timed out.'}
            }
            if($vtune.HasExited -and $vtune.ExitCode -ne 0){$state.errors+=('VTune returned '+$vtune.ExitCode)}
            $state.vtune_exit_code=$vtune.ExitCode
            if(-not $state.vtune_collection_finished_utc){$state.vtune_collection_finished_utc=[DateTime]::UtcNow.ToString('o')}
            $vtuneLog=Join-Path $dir 'vtune-errors.log'
            if(Test-Path -LiteralPath $vtuneLog){
                $state.vtune_data_limited=(Read-DiagnosticSharedText $vtuneLog) -match '(?i)data limit.+reached'
                if($state.vtune_data_limited){$state.errors+='VTune reached its data limit and stopped early; its output does not establish full gameplay-window coverage.'}
            }
            $vtune.Dispose()
        }
        if($writer){$writer.Dispose()}
        try{
            $state.artifact_validation=Complete-DiagnosticArtifacts $Root $sessionPath $plan $state
        }catch{
            $state.errors+=('Artifact parsing: '+$_.Exception.Message)
        }
        if($plan.profile -eq 'full'){
            try{
                $currentTarget=Get-DiagnosticTarget $launch $Root
                if($currentTarget){
                    Write-JsonUtf8 (Join-Path $dir 'modules.json') @($currentTarget.Modules|ForEach-Object {@{module=$_.ModuleName;base_address=('0x{0:x}' -f $_.BaseAddress.ToInt64());bytes=$_.ModuleMemorySize}})
                    $state.module_inventory_utc=[DateTime]::UtcNow.ToString('o')
                }
            }catch{$state.errors+=('Final module inventory: '+$_.Exception.Message)}
        }
        if($spec.Nsight -and $launch.PSObject.Properties['diagnostic_wrapper']){
            $helper=Get-Process -Id $launch.diagnostic_wrapper.pid -ErrorAction SilentlyContinue
            if($helper){
                try{
                    if($helper.Path -eq $tools.ngfx -and $helper.StartTime.ToUniversalTime().ToString('o') -eq $launch.diagnostic_wrapper.started -and -not (Wait-DiagnosticHelper $helper 30000)){$state.errors+='Nsight launcher completion timed out.'}
                }catch{$state.errors+=('Nsight completion: '+$_.Exception.Message)}
                finally{$helper.Dispose()}
            }
        }
        if($spec.RenderDoc){
            $captures=@(Get-ChildItem -LiteralPath (Join-Path $sessionPath 'user/captures') -Filter '*.rdc' -File -Recurse -ErrorAction SilentlyContinue|Where-Object {$_.Length -gt 0})
            $state.capture_files+=@($captures|ForEach-Object {Get-RelativePath $Root $_.FullName})
            if(-not $captures.Count){
                $state.errors+=if(-not $auto.capture_trigger_attempted){'RenderDoc was not triggered: the gameplay capture window was not reached.'}
                               else{'RenderDoc produced no frame file; check module inventory, API initialization and capture-trigger records.'}
            }
        }
        if($spec.Nsight){
            $captures=@(Get-ChildItem -LiteralPath (Join-Path $sessionPath 'user/captures/nsight') -File -Recurse -ErrorAction SilentlyContinue|Where-Object {$_.Length -gt 0 -and $_.Extension -in @('.ngfx-gputrace','.nsight-gputrace')})
            $state.capture_files+=@($captures|ForEach-Object {Get-RelativePath $Root $_.FullName})
            if(-not $captures.Count){$state.errors+='Automatic Nsight capture produced no trace; check this session profiler logs.'}
        }
        if($plan.wpr_profile -and -not $state.wpr_saved){$state.errors+='No completed ETL trace is available.'}
        if($plan.profile -eq 'full'){
            try{
                $state.collectors=@(Get-DiagnosticCoverage $Root $sessionPath $plan $state $auto)
                foreach($item in $state.collectors){if($item.requested -and -not $item.produced){$state.errors+=('Missing collector output: '+$item.name)}}
                Write-JsonUtf8 (Join-Path $dir 'coverage.json') @{
                    schema=1;scope='single-game-process';target_pid=$launch.pid;target_started_utc=$launch.process_started
                    collector_start_errors=$state.collector_start_errors;capture_schedule=$plan.capture_schedule;collector_timing=$plan.collector_timing
                    startup_window_started=$state.startup_window_started
                    collector_started_utc=$state.collector_started_utc;window_started=$state.window_started;window_finished=$state.window_finished
                    marks=$state.marks;collectors=$state.collectors;same_frame_across_capture_tools=$false;performance_acceptance=$false
                }
            }catch{$state.errors+=('Coverage inventory: '+$_.Exception.Message)}
        }
        if($state.errors.Count -and $state.status -eq 'collected'){$state.status='partial'}
        try{
            if($storage){Update-DiagnosticStorage $storage -Force}
            else{$state.usage=Get-DiagnosticUsage $storageRoot}
        }catch{
            $state.errors+=('Final storage sample: '+$_.Exception.Message);$state.status='failed'
        }finally{
            $script:DiagnosticStorageMonitor=$null
            if($storage){
                try{$storage.writer.Dispose()}
                catch{$state.errors+=('Storage trace close: '+$_.Exception.Message);$state.status='failed'}
            }
        }
        if($storage -and $storage.low_space){$state.status='disk-space-stop'}
        $state.wpr_active=$wprOwned;$state.complete=$true;$state.finished_at=[DateTime]::UtcNow.ToString('o')
        if($state.status -eq 'waiting'){$state.status='no-window'}elseif($state.status -eq 'recording'){$state.status='interrupted'}
        $auto.phase=if($state.errors.Count -or $state.status -notin @('collected')){'failed'}else{'finished'}
        $state.automation_phase=$auto.phase;$state.scene_status=$auto.scene_status
        try{
            Write-JsonUtf8 (Join-Path $dir 'automation.json') $auto
            Write-JsonUtf8 $statusPath $state
        }finally{
            # A persistent recording-file failure must not suppress the exit request.
            try{
                if(Get-DiagnosticTarget $launch $Root){& (Join-Path $PSScriptRoot 'control.ps1') -Action quit -Session $Session | Out-Null}
            }catch{
                $state.errors+=('Exit request: '+$_.Exception.Message);$state.status='failed';$state.automation_phase='failed';$auto.phase='failed'
                Write-JsonUtf8 (Join-Path $dir 'automation.json') $auto;Write-JsonUtf8 $statusPath $state
            }
        }
    }
}

function Get-DiagnosticCollectorTiming([string]$Profile,[int]$Delay,[int]$Seconds,[int]$BootTimeout,[int]$Warmup,[int]$SessionTimeout) {
    $startup=$Profile -eq 'full'
    return @{
        phase=if($startup){'startup'}else{'gameplay'}
        anchor=if($startup){'collector-attach'}else{'gameplay-window-start'}
        vtune_seconds=if($startup){$SessionTimeout}else{$Seconds}
        nsight_start_ms=0
        nsight_timeout_seconds=[Math]::Max(120,$BootTimeout+$Warmup+$Seconds+30)
        nsight_scene_verified=$false;nsight_anchor='scene-ready-and-warm'
    }
}
function Get-DiagnosticCaptureSchedule([string]$Profile,[int]$Seconds) {
    return @{
        anchor='gameplay-window-start'
        nsight_seconds=if($Profile -eq 'full'){$null}else{[int][Math]::Max(1,[Math]::Min(15,[Math]::Floor($Seconds/4)))}
        renderdoc_seconds=if($Profile -eq 'full'){[int][Math]::Floor($Seconds/2)}else{[int][Math]::Min(15,$Seconds-2)}
        guest_trace_seconds=[int][Math]::Max(1,[Math]::Min(15,[Math]::Floor($Seconds/4)))
    }
}
function Get-DiagnosticOutputFiles([string]$Root,[string]$Directory,[string]$Filter='*') {
    Assert-ProjectPath $Root $Directory
    if(-not(Test-Path -LiteralPath $Directory -PathType Container)){return}
    foreach($item in Get-ChildItem -LiteralPath $Directory -Force){
        Assert-ProjectPath $Root $item.FullName
        if($item.PSIsContainer){Get-DiagnosticOutputFiles $Root $item.FullName $Filter}
        elseif($item.Length -gt 0 -and $item.Name -like $Filter){Get-RelativePath $Root $item.FullName}
    }
}
function Get-DiagnosticCoverage([string]$Root,[string]$Session,$Plan,$State,$Auto) {
    $spec=Get-DiagnosticPlanSpec $Plan
    $files=@{}
    $files.engine=@(Get-DiagnosticOutputFiles $Root (Join-Path $Session 'log'))+@(Get-DiagnosticOutputFiles $Root (Join-Path $Session 'user/log'))+@(Get-DiagnosticOutputFiles $Root (Join-Path $Session 'diagnostics/cdl'))
    $files.shaders=@(Get-DiagnosticOutputFiles $Root (Join-Path $Session 'user/shader'))+@(Get-DiagnosticOutputFiles $Root (Join-Path $Session 'shader'))
    $files.wpr=@(Get-DiagnosticOutputFiles $Root (Join-Path $Session 'diagnostics') '*.etl')
    $files.renderdoc=@(Get-DiagnosticOutputFiles $Root (Join-Path $Session 'user/captures') '*.rdc')
    $files.nsight=@(Get-DiagnosticOutputFiles $Root (Join-Path $Session 'user/captures/nsight') '*.ngfx-gputrace')+
        @(Get-DiagnosticOutputFiles $Root (Join-Path $Session 'user/captures/nsight') '*.nsight-gputrace')
    $files.vtune=@(Get-DiagnosticOutputFiles $Root (Join-Path $Session 'diagnostics/vtune/data.0'))
    $files.sensors=@(Get-DiagnosticOutputFiles $Root (Join-Path $Session 'diagnostics') 'nvidia.csv')
    $entries=@(
        @{name='engine-log-validation-crash';requested=$true;produced=($files.engine.Count -gt 0);scope='target-process'}
        @{name='shaders';requested=$spec.Shaders;produced=($files.shaders.Count -gt 0);scope='target-process'}
        @{name='wpr';requested=[bool]$spec.Wpr;produced=($State.wpr_saved -and $files.wpr.Count -gt 0);scope='system-trace-filter-by-target-pid'}
        @{name='renderdoc';requested=$spec.RenderDoc;produced=($Auto.capture_triggered -and $files.renderdoc.Count -gt 0);scope='target-process-one-frame'}
        @{name='nsight';requested=$spec.Nsight;produced=($null -ne $State.nsight_exit_code -and $State.nsight_exit_code -eq 0 -and $files.nsight.Count -gt 0);scope='target-process-bounded-gpu-trace'}
        @{name='vtune';requested=[bool]$spec.VtuneAnalysis;produced=($State.vtune_started -and $null -ne $State.vtune_exit_code -and $State.vtune_exit_code -eq 0 -and $files.vtune.Count -gt 0);scope='target-process-finalized-memory-access'}
        @{name='sensors';requested=$false;produced=($files.sensors.Count -gt 0);scope='whole-gpu-optional'}
    )
    $qualityPath=Join-Path $Session 'diagnostics/data-quality.json'
    $quality=if(Test-Path -LiteralPath $qualityPath){Read-JsonUtf8 $qualityPath}else{$null}
    foreach($entry in $entries){
        $key=if($entry.name -eq 'engine-log-validation-crash'){'engine'}else{$entry.name}
        $entry.artifacts=$files[$key]
        $entry.target_pid=$State.target_pid;$entry.target_started_utc=$State.target_started_utc
        # Nonempty artifacts are evidence of output, not validation of every recorded event.
        $entry.exclusion_reason=if($Plan.profile -eq 'full' -and -not $entry.requested -and $entry.name -in @('renderdoc','nsight')){'Mutually exclusive GPU capture backend; available through its separate BAT.'}else{$null}
        $entry.content_validated=($State.Contains('artifact_validation') -and
            $State.artifact_validation.Contains($entry.name) -and $State.artifact_validation[$entry.name].validated)
        $entry.data_limited=($entry.name -eq 'vtune' -and $State.Contains('vtune_data_limited') -and $State.vtune_data_limited)
        $content=$quality -and $quality.checks.PSObject.Properties[$entry.name] -and $quality.checks.($entry.name).validated
        $entry.gameplay_window_coverage_verified=($content -and $entry.name -in @('wpr','vtune','sensors') -and -not $entry.data_limited)
        $entry.detail_window_coverage_verified=($content -and $entry.name -eq 'nsight')
        if($entry.name -eq 'nsight'){$entry.scope='target-process-short-detail-window; full-window GPU scheduling in WPR'}
    }
    return $entries
}

function Test-DiagnosticBootstrap {
    foreach($file in @('tools.ps1','diagnose.ps1','diagnostic-click.ps1','diagnostic_tools.ps1','diagnostic_automation.ps1','diagnostic_artifacts.ps1','diagnostic_sensors.ps1','diagnostic_retention.ps1','launch.ps1','control.ps1')){
        $tokens=$null;$errors=$null
        $null=[System.Management.Automation.Language.Parser]::ParseFile((Join-Path $PSScriptRoot $file),[ref]$tokens,[ref]$errors)
        if($errors.Count){throw ($file+': '+($errors.Message -join '; '))}
    }
    & (Join-Path $PSScriptRoot '../tests/bootstrap/test_tools.ps1')
    & (Join-Path $PSScriptRoot '../tests/bootstrap/test_diagnostic_automation.ps1')
    & (Join-Path $PSScriptRoot '../tests/bootstrap/test_diagnostic_screen.ps1')
    & (Join-Path $PSScriptRoot '../tests/bootstrap/test_diagnostic_process.ps1')
    & (Join-Path $PSScriptRoot '../tests/bootstrap/test_diagnostic_full.ps1')
    & (Join-Path $PSScriptRoot '../tests/bootstrap/test_diagnostic_save_source.ps1')
    & (Join-Path $PSScriptRoot '../tests/bootstrap/test_diagnostic_retention.ps1')
    & (Join-Path $PSScriptRoot '../tests/bootstrap/test_control_protocol.ps1')
}
function Get-DiagnosticErrorInfo($Record,[string]$Root) {
    $causes=@()
    for($cause=$Record.Exception;$cause -and $causes.Count -lt 8;$cause=$cause.InnerException){
        $message=([string]$cause.Message).Replace($Root,'.')
        if($message.Length -gt 2048){$message=$message.Substring(0,2048)+' ...'}
        $causes+=@{message=$message;exception_type=$cause.GetType().FullName;hresult=('0x{0:X8}' -f $cause.HResult)}
    }
    return @{
        message=$causes[0].message;exception_type=$Record.Exception.GetType().FullName;causes=$causes
        line=$Record.InvocationInfo.ScriptLineNumber
        position=([string]$Record.InvocationInfo.PositionMessage).Replace($Root,'.')
        stack=([string]$Record.ScriptStackTrace).Replace($Root,'.')
    }
}
