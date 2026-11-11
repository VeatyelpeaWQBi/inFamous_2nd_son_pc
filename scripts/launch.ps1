[CmdletBinding()]
param([ValidateSet('auto','720p','900p','1080p','1440p','1800p','pro4k')][string]$Mode='auto',
      [switch]$WaitForDebugger,[switch]$Benchmark,[switch]$CheckOnly,[switch]$PrepareOnly,[switch]$NoMenu,
      [ValidateSet('none','baseline','critical','lighting')][string]$TimingProfile='none',
      [string]$SaveSession,[switch]$SuperCapture,[string]$SuperCaptureBatch,[switch]$CriticalCapture,[switch]$CpuHotspots,
      [switch]$ReadbackBarrierAudit,[switch]$PageProtectAudit,[switch]$ComputeScopeAudit,
      [switch]$ReadbackWaitScopeAudit,
      [switch]$ComputeProtectAudit,
      [switch]$DrawPrepAudit,
      [switch]$C73ScanCandidate,[switch]$ParticlePollPauseCandidate,[switch]$AccessUnionCandidate,
      [switch]$ReadbackCopyStageCandidate,[switch]$ImageReadTransitionCandidate,[switch]$FeedbackArenaCandidate,
      [switch]$FeedbackArenaGroupCandidate,
      [switch]$FeedbackArenaPoolCandidate,[switch]$SuperPool,
      [switch]$FeedbackArenaTiledCandidate,
      [switch]$VertexInputKeyCandidate,
      [ValidateSet('Off','Train','Use')][string]$RendererPgoTrial='Off',
      [ValidateSet('none','cpu','gpu','memory','compute','draw','validation','crash','shaders','renderdoc','nsight','vtune','vtune-memory','all','full')][string]$Diagnostic='none',
      [ValidateRange(-1,3600)][int]$DiagnosticDelaySeconds=90,[ValidateRange(10,600)][int]$DiagnosticSeconds=60)
Set-StrictMode -Version Latest
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot 'tools.ps1')
$projectRoot=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$isDiagnostic=$Diagnostic -ne 'none'
$isTiming=$TimingProfile -ne 'none'
$useFeedbackPool=[bool]($FeedbackArenaPoolCandidate -or $SuperPool)
if($SuperPool -and ($Benchmark -or $SuperCapture -or $isDiagnostic -or $isTiming -or $CpuHotspots)){
    throw 'Super-pool manual testing cannot be combined with automated captures or benchmarks.'
}
if(($C73ScanCandidate -or $ParticlePollPauseCandidate -or $AccessUnionCandidate -or
    $ReadbackCopyStageCandidate -or $ImageReadTransitionCandidate -or $FeedbackArenaCandidate -or
    $VertexInputKeyCandidate -or $FeedbackArenaGroupCandidate -or $FeedbackArenaPoolCandidate -or $FeedbackArenaTiledCandidate) -and
   (-not $SuperCapture -or -not $SuperCaptureBatch -or $isDiagnostic)){
    throw 'Experimental patch requires an owned native super capture.'
}
if($CpuHotspots -and (-not $SuperCapture -or -not $SuperCaptureBatch -or $isDiagnostic -or $CriticalCapture)){
    throw 'CPU control is restricted to an owned native super CPU diagnostic.'
}
if($RendererPgoTrial -ne 'Off' -and
    (-not $SuperCaptureBatch -or $isDiagnostic -or $CriticalCapture -or $CpuHotspots -or
     $C73ScanCandidate -or $ParticlePollPauseCandidate -or $AccessUnionCandidate -or
     $ReadbackCopyStageCandidate -or $ImageReadTransitionCandidate -or $FeedbackArenaCandidate -or
     $VertexInputKeyCandidate -or $FeedbackArenaGroupCandidate -or $FeedbackArenaPoolCandidate -or $FeedbackArenaTiledCandidate)){
    throw 'Renderer PGO requires its own native super capture.'
}
if($CriticalCapture -and (-not $SuperCapture -or $isDiagnostic)){throw 'Critical capture requires the native super collector.'}
if($DrawPrepAudit -and (-not $CriticalCapture -or -not $SuperCaptureBatch -or $isDiagnostic -or
   $CpuHotspots -or $ReadbackBarrierAudit -or $PageProtectAudit -or $ComputeScopeAudit -or
   $ReadbackWaitScopeAudit -or $ComputeProtectAudit -or -not $FeedbackArenaPoolCandidate)){
    throw 'Draw preparation audit requires its owned compact-pool native capture.'
}
if($ReadbackBarrierAudit -and (-not $CriticalCapture -or -not $SuperCaptureBatch)){
    throw 'Readback barrier audit requires an owned native critical capture.'
}
if($PageProtectAudit -and (-not $CriticalCapture -or -not $SuperCaptureBatch -or $ReadbackBarrierAudit)){
    throw 'Page protection audit requires its own native critical capture.'
}
if($ComputeScopeAudit -and (-not $CriticalCapture -or -not $SuperCaptureBatch -or
    $ReadbackBarrierAudit -or $PageProtectAudit)){
    throw 'Compute scope audit requires its own native critical capture.'
}
if($ReadbackWaitScopeAudit -and (-not $CriticalCapture -or -not $SuperCaptureBatch -or
    $ReadbackBarrierAudit -or $PageProtectAudit -or $ComputeScopeAudit -or $ComputeProtectAudit)){
    throw 'Readback wait audit requires its own native critical capture.'
}
if($ComputeProtectAudit -and (-not $CriticalCapture -or -not $SuperCaptureBatch -or
    $ReadbackBarrierAudit -or $PageProtectAudit -or $ComputeScopeAudit)){
    throw 'Compute protection audit requires its own native critical capture.'
}
if(($ReadbackCopyStageCandidate -or $ImageReadTransitionCandidate) -and
   ($CriticalCapture -or $CpuHotspots -or $C73ScanCandidate -or $ParticlePollPauseCandidate -or
    $AccessUnionCandidate -or ($ReadbackCopyStageCandidate -and $ImageReadTransitionCandidate))){
    throw 'The barrier experiment requires an isolated lightweight release.'
}
if(($FeedbackArenaCandidate -or $VertexInputKeyCandidate) -and
   (($CriticalCapture -and -not $FeedbackArenaCandidate) -or $CpuHotspots -or $C73ScanCandidate -or $ParticlePollPauseCandidate -or
    $AccessUnionCandidate -or $ReadbackCopyStageCandidate -or $ImageReadTransitionCandidate -or
    ($FeedbackArenaCandidate -and $VertexInputKeyCandidate))){
    throw 'The requested candidate requires an isolated lightweight release.'
}
if($FeedbackArenaGroupCandidate -and
   ($CpuHotspots -or $C73ScanCandidate -or $ParticlePollPauseCandidate -or $AccessUnionCandidate -or
    $ReadbackCopyStageCandidate -or $ImageReadTransitionCandidate -or $FeedbackArenaCandidate -or
    $VertexInputKeyCandidate)){
    throw 'Feedback arena group requires an isolated native super capture.'
}
if($SuperCaptureBatch -and -not $SuperCapture){throw 'A super capture batch requires collection-only mode.'}
if(($FeedbackArenaPoolCandidate -or $FeedbackArenaTiledCandidate) -and
   ($CpuHotspots -or $C73ScanCandidate -or $ParticlePollPauseCandidate -or $AccessUnionCandidate -or
    $ReadbackCopyStageCandidate -or $ImageReadTransitionCandidate -or $FeedbackArenaCandidate -or
    $FeedbackArenaGroupCandidate -or $VertexInputKeyCandidate -or
    ($FeedbackArenaPoolCandidate -and $FeedbackArenaTiledCandidate))){
    throw 'Feedback arena pool requires an isolated native super capture.'
}
if($SuperCaptureBatch){
    . (Join-Path $PSScriptRoot 'super_capture_tools.ps1')
    Assert-SuperCaptureStarting $projectRoot $SuperCaptureBatch
}
if($SuperCapture){
    if($isTiming -or ($isDiagnostic -and $Diagnostic -ne 'full') -or $Benchmark -or $WaitForDebugger){throw 'Super capture supports collection-only mode or the dedicated Full collector.'}
    if($isDiagnostic -and -not $SuperCaptureBatch){throw 'Full super capture requires its owning batch.'}
    if($Mode -eq 'auto'){$Mode='pro4k'}
    if($Mode -notin @('1080p','1440p','pro4k')){throw 'Super capture supports 1080p, 1440p and pro4k.'}
    $NoMenu=$true
}
if($isTiming){
    if($isDiagnostic -or $Benchmark -or $WaitForDebugger){throw 'Timing profiles require ordinary play without full/profiler/debugger instrumentation.'}
    if($Mode -eq 'auto'){$Mode='pro4k'}
    if($Mode -notin @('1080p','pro4k')){throw 'Timing comparisons support 1080p and pro4k.'}
}
if($isDiagnostic -or $WaitForDebugger -or $isTiming -or $SaveSession -or $SuperCapture){. (Join-Path $PSScriptRoot 'diagnostic_tools.ps1')}
if($isDiagnostic){
    if($Benchmark){throw 'Diagnostic captures cannot be submitted as frozen benchmarks.'}
    if($Mode -eq 'auto'){$Mode='pro4k'}
    if($Mode -notin @('1080p','pro4k') -and -not($SuperCapture -and $Mode -eq '1440p')){throw 'Diagnostics currently support 1080p and pro4k; super capture also supports 1440p.'}
    if($WaitForDebugger){$DiagnosticDelaySeconds=-1}
    Assert-DiagnosticTools (Get-DiagnosticSpec $Diagnostic) (Get-DiagnosticTools) -RequireAdministrator (-not $PrepareOnly)

}
if($Benchmark){
    if($Mode -eq 'auto'){$Mode='pro4k'}
    if($Mode -notin @('1080p','pro4k')){throw 'Frozen benchmarks support only 1080p and pro4k.'}
}
if($CheckOnly){Invoke-ProjectNativeTool 'preflight' @('--mode',$Mode); exit $LASTEXITCODE}
$runtimeState=Resolve-ProjectChild $projectRoot 'state\runtime'
$null=[IO.Directory]::CreateDirectory($runtimeState)
$process=$null;$windowBranding=$null;$collector=$null;$diagnosticWrapper=$null;$diagnosticPlan=$null;$diagnosticExitCode=0;$session=$null
try{
    $manifest=Read-JsonUtf8 (Join-Path $projectRoot 'project.json')
    $runtime=Resolve-ProjectChild $projectRoot $manifest.runtime_path
    if($manifest.phase -ne 'project-engine' -or -not $manifest.new_executable_built -or -not(Test-Path -LiteralPath $runtime -PathType Leaf)){
        throw 'Project engine has not been built or is missing. Run build.bat first; no external engine will be used.'
    }
    $compilerReceiptPath=Join-Path (Split-Path -Parent $runtime) 'compiler-variant.json'
    $actualPgo='Off'
    if(Test-Path -LiteralPath $compilerReceiptPath -PathType Leaf){
        $compilerReceipt=Read-JsonUtf8 $compilerReceiptPath
        if($compilerReceipt.PSObject.Properties['renderer_pgo']){
            $actualPgo=$compilerReceipt.renderer_pgo
            if($actualPgo -ne 'Off' -and $compilerReceipt.runtime_sha256 -cne $manifest.runtime_sha256){
                throw 'The renderer PGO receipt does not match the pinned runtime.'
            }
        }
    }
    if($RendererPgoTrial -ne $actualPgo){
        throw 'Renderer PGO trial and build do not match. Use the normal build for ordinary play.'
    }
    if(Get-Process -Name ([IO.Path]::GetFileNameWithoutExtension($runtime)) -ErrorAction SilentlyContinue){throw 'The pinned engine is already running.'}
    $saveRecord=$null
    $saveSourcePolicy=if($SaveSession){'explicit-session'}else{'fixed-project-home'}
    if($SaveSession){
        if($Benchmark){throw 'Frozen benchmarks cannot override their save source.'}
        $saveRecord=Read-ProjectSaveSession $projectRoot $SaveSession
        $SaveSession=Get-RelativePath $projectRoot $saveRecord.Path
    }
    $baseline=Resolve-ProjectChild $projectRoot 'state/baseline/user'
    $liveHome=Resolve-ProjectChild $projectRoot 'state/play/home'
    $homeSource=if($Benchmark){Join-Path $baseline 'home'}else{$liveHome}
    if($saveRecord){$homeSource=Resolve-ProjectChild $projectRoot ($SaveSession+'/home')}
    elseif($homeSource -ne $liveHome){$saveSourcePolicy='baseline-home'}
    if(($isDiagnostic -or $isTiming -or $SuperCapture) -and -not(Test-ProjectGameSaveHome $projectRoot $homeSource $manifest.title)){
        throw 'Fixed project save is missing: state/play/home. Restore the gameplay save there; no automatic session selection or new-game fallback is allowed.'
    }
    if(($isDiagnostic -or $WaitForDebugger) -and -not $PrepareOnly -and -not $SuperCapture){
        # Keep the previous latest session before creating the new one.
        $null=Invoke-DiagnosticSessionRetention $projectRoot $SaveSession
    }
    if($isDiagnostic){
        $volume=[IO.DriveInfo]::new([IO.Path]::GetPathRoot($projectRoot))
        if($volume.AvailableFreeSpace -lt (Get-DiagnosticStoragePolicy).minimum_free_bytes){throw 'At least 2 GiB of free disk space is required for diagnostic shutdown and flushing.'}
    }
    if(-not $isDiagnostic -and -not $Benchmark -and -not $isTiming -and -not $PrepareOnly -and -not $NoMenu){
        $selection=@(Invoke-ProjectNativeTool 'launch-menu' @('--mode',$Mode))
        if($LASTEXITCODE -eq 1){exit 0}
        if($LASTEXITCODE -ne 0){throw 'Launch settings could not be opened; game was not started.'}
        $Mode=($selection -join '').Trim()
    }
    $sessionName=(Get-Date).ToString('yyyyMMdd-HHmmss-fff')+'-'+$Mode+'-'+[Guid]::NewGuid().ToString('N').Substring(0,8)
    if($isTiming){$sessionName+='-timing-'+$TimingProfile}
    if($isDiagnostic){$sessionName+='-diagnostic-'+$Diagnostic}
    $parent='state/sessions'
    $session=Resolve-ProjectChild $projectRoot ($parent+'/'+$sessionName)
    $sessionRelative=Get-RelativePath $projectRoot $session
    $null=[IO.Directory]::CreateDirectory($session)
    $verifyArguments=@('--mode',$Mode,'--session',$sessionRelative)
    if($Benchmark){$verifyArguments+='--frozen'}
    $result=@(Invoke-ProjectNativeTool 'preflight' $verifyArguments)
    if($LASTEXITCODE -ne 0){throw 'Preflight failed; game was not started.'}
    $verified=($result -join [Environment]::NewLine)|ConvertFrom-Json
    $Mode=$verified.mode
    $runtime=Resolve-ProjectChild $projectRoot $verified.runtime
    Write-Host ('Selected engine: '+$verified.runtime)
    if(-not $verified.runtime_build.source_checked){throw 'A verified project build is required. Run build.bat first.'}
    $portableUser=Resolve-ProjectChild $projectRoot ($sessionRelative+'/user')
    $null=[IO.Directory]::CreateDirectory($portableUser)
    $persistent=Resolve-ProjectChild $projectRoot 'state\play\user'
    $configSource=if(-not $Benchmark -and (Test-Path -LiteralPath $persistent)){$persistent}else{$baseline}
    foreach($item in Get-ChildItem -LiteralPath $configSource){
        if($item.Name -notin @('home','custom_configs','cache','shader','log','temp','screenshots','captures','download','play_time.txt')){
            Copy-ProjectTree $projectRoot $item.FullName (Join-Path $portableUser $item.Name)
        }
    }
    # --benchmark-dir makes the engine read game overrides from session/custom_configs.
    Convert-ProjectConfigPaths (Join-Path $session 'custom_configs/CUSA00309.json') $projectRoot $session
    Copy-ProjectTree $projectRoot (Join-Path $session 'custom_configs') (Join-Path $portableUser 'custom_configs')
    Ensure-ProjectQuitMenu $portableUser
    Convert-ProjectConfigPaths (Join-Path $portableUser 'config.json') $projectRoot $session
    if($null -ne $verified.launcher_settings){
        $globalPath=Join-Path $portableUser 'config.json'
        $globalConfig=Read-JsonUtf8 $globalPath
        $globalConfig.General|Add-Member -MemberType NoteProperty -Name 'show_fps_counter' -Value ([bool]$verified.launcher_settings.show_fps) -Force
        foreach($key in @('full_screen','full_screen_mode','window_width','window_height')){
            $globalConfig.GPU|Add-Member -MemberType NoteProperty -Name $key -Value $verified.gpu_config.$key -Force
        }
        Write-JsonUtf8 $globalPath $globalConfig
    }
    $saveFiles=@(Get-ChildItem -LiteralPath $homeSource -Recurse -File | ForEach-Object {
        Assert-ProjectPath $projectRoot $_.FullName
        @{path=(Get-RelativePath $homeSource $_.FullName);bytes=$_.Length;sha256=(Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant()}
    })
    if($saveRecord -and -not $saveFiles.Count){throw 'Selected session has no save files.'}
    Copy-ProjectTree $projectRoot $homeSource (Join-Path $session 'home')
    Write-Host ('Save snapshot source: '+(Get-RelativePath $projectRoot $homeSource)+'; policy='+$saveSourcePolicy+'; original preserved.')
    $null=New-Item -ItemType Junction -Path (Join-Path $portableUser 'home') -Target (Join-Path $session 'home')
    Copy-ProjectTree $projectRoot (Join-Path $configSource 'perf_tuning.ini') (Join-Path $session 'perf_tuning.ini')
    # Ordinary run enables the candidate. Measurements start from an explicit baseline,
    # even when a prior ordinary session saved its enabled tuning back to portable user data.
    $computeBarrierEnabled=-not ($Benchmark -or $isDiagnostic -or $isTiming -or $SuperCapture)
    $tuningPath=Join-Path $session 'perf_tuning.ini'
    $tuningText=if(Test-Path -LiteralPath $tuningPath){[IO.File]::ReadAllText($tuningPath)}else{''}
    $sharpLine='command_buffer_sharps = 0'
    $sharpPattern='(?m)^\s*command_buffer_sharps\s*=.*$'
    if([regex]::IsMatch($tuningText,$sharpPattern)){$tuningText=[regex]::Replace($tuningText,$sharpPattern,$sharpLine)}
    else{$tuningText+=[Environment]::NewLine+$sharpLine}
    $barrierLine='compute_barrier_scope = '+[int]$computeBarrierEnabled
    $barrierPattern='(?m)^\s*compute_barrier_scope\s*=.*$'
    if([regex]::IsMatch($tuningText,$barrierPattern)){$tuningText=[regex]::Replace($tuningText,$barrierPattern,$barrierLine)}
    else{$tuningText+=[Environment]::NewLine+$barrierLine}
    [IO.File]::WriteAllText($tuningPath,($tuningText+[Environment]::NewLine),[Text.UTF8Encoding]::new($false))
    Write-Host ('Compute barrier scope candidate: '+$(if($computeBarrierEnabled){'ON (ordinary play)'}else{'OFF (measurement baseline; experiment may override)'}))
    foreach($name in @('cache','shader')){
        if($isDiagnostic -and $SuperCapture -and $name -eq 'cache'){
            $source=Resolve-ProjectChild $projectRoot 'state/runtime/cache'
            $existing=@(Get-ChildItem -LiteralPath $source -Recurse -File -ErrorAction Stop | Where-Object {$_.Length -gt 0})
            if(-not $existing.Count){throw 'Full super requires an existing populated state/runtime/cache; run ordinary play/collection first.'}
            $null=New-Item -ItemType Junction -Path (Join-Path $portableUser 'cache') -Target $source
            Write-JsonUtf8 (Join-Path $session 'cache-source.json') @{policy='reuse-existing-cache';source='state/runtime/cache';files=$existing.Count;bytes=($existing|Measure-Object Length -Sum).Sum;compatibility='engine cache version and game fingerprint validation';reused_utc=[DateTime]::UtcNow.ToString('o')}
            continue
        }
        if($isDiagnostic){
            $private=Resolve-ProjectChild $projectRoot ($sessionRelative+'/user/'+$name)
            $null=[IO.Directory]::CreateDirectory($private)
            continue
        }
        $shared=Resolve-ProjectChild $projectRoot ('state/runtime/'+$name)
        $null=[IO.Directory]::CreateDirectory($shared)
        $link=Join-Path $portableUser $name
        if(Test-Path -LiteralPath $link){throw 'Unexpected pre-existing cache directory in a fresh session.'}
        $null=New-Item -ItemType Junction -Path $link -Target $shared
    }
    $configPath=Join-Path $portableUser 'custom_configs\CUSA00309.json'
    if($Benchmark){
        $config=Read-JsonUtf8 $configPath
        $config.General.volume_slider=0
        Write-JsonUtf8 $configPath $config
        Write-JsonUtf8 (Join-Path $session 'custom_configs\CUSA00309.json') $config
    }
    if($isDiagnostic){
        $diagnosticPlan=Initialize-DiagnosticSession $projectRoot $session $Diagnostic $DiagnosticDelaySeconds $DiagnosticSeconds -PrepareOnly:$PrepareOnly
        if($SuperCapture){
            $superRequest=Read-JsonUtf8 (Join-Path (Resolve-SuperCaptureBatch $projectRoot $SuperCaptureBatch) 'capture.json')
            Set-SuperFullPlan $diagnosticPlan $superRequest
            Write-JsonUtf8 (Join-Path $session 'diagnostics/plan.json') $diagnosticPlan
        }
    }
    $timingOverrides=[ordered]@{}
    $timingConfigOverrides=[ordered]@{}
    if($isTiming -or ($SuperCapture -and -not $isDiagnostic)){
        $timingOverrides=[ordered]@{
            # perf_log also enables the legacy 1 ms SuspendThread sampler. PathTrace and
            # targeted GPU query records do not depend on those periodic counters.
            perf_log=0
            written_sync_fastpath=0
            image_sync_once=0
            readback_word_page_cluster=0
            compute_compile_parallel=1;async_readbacks=1;readback_prefetch=1;readback_relaxed_sites='0'
            path_trace=[int]($CriticalCapture -or ($isTiming -and $TimingProfile -ne 'baseline'))
            gpu_profile=if($TimingProfile -eq 'lighting'){2}else{0}
            gpu_profile_compute_hash=if($TimingProfile -eq 'lighting'){'ce1675e0'}else{'0'}
            profile_stacks=0;dump_frames=0;trace_reads='0';trace_writer_word='0';trace_shader='';trace_draw=0
            dump_images='';dump_after='';gpu_checkpoints=0
        }
        Set-DiagnosticTuning $projectRoot $session $timingOverrides
        $timingConfigOverrides=[ordered]@{
            General=@{volume_slider=0}
            Debug=@{debug_dump=$false;shader_collect=$false}
            GPU=@{dump_shaders=$false}
            Vulkan=@{
                renderdoc_enabled=$false;vkvalidation_enabled=$false;vkvalidation_core_enabled=$false
                vkvalidation_sync_enabled=$false;vkvalidation_gpu_enabled=$false;vkcrash_diagnostic_enabled=$false
                vkhost_markers=$false;vkguest_markers=$false
            }
            Log=@{enable=$true;filter='*:Info'}
        }
        if($SuperCapture){$timingConfigOverrides.General.console_language=1}
        foreach($relativeConfig in @('user/config.json','user/custom_configs/CUSA00309.json')){
            $timingConfigPath=Join-Path $session $relativeConfig
            $timingConfig=Read-JsonUtf8 $timingConfigPath
            foreach($section in $timingConfigOverrides.Keys){
                if(-not $timingConfig.PSObject.Properties[$section]){$timingConfig|Add-Member NoteProperty $section ([pscustomobject]@{})}
                foreach($key in $timingConfigOverrides[$section].Keys){$timingConfig.$section|Add-Member NoteProperty $key $timingConfigOverrides[$section][$key] -Force}
            }
            Write-JsonUtf8 $timingConfigPath $timingConfig
        }
        Copy-ProjectTree $projectRoot (Join-Path $session 'user/custom_configs/CUSA00309.json') (Join-Path $session 'custom_configs/CUSA00309.json')
        if($SuperCapture){Write-Host 'Super collection: isolated save copy, English navigation, native frame/compile/meter records; no automatic analysis.'}
        else{
            Write-Host ('Timing profile: '+$TimingProfile+'; integrated core policies; no external collectors or automatic input.')
            Write-Host 'Manual comparison: use the same save, scene, camera, warmup and input. F1: exit confirmation.'
            Write-Host 'CPU trace uses about 2.3 MiB bounded queues and a background CSV file; losses are reported, never blocked.'
        }
        if($TimingProfile -eq 'lighting'){Write-Host 'GPU timestamps measure only cs_ce1675e0. Instrumented intervals are not ordinary frame times.'}
    }
    $arguments=@('-g',(Get-RelativePath $session (Join-Path $projectRoot 'game\CUSA00309\eboot.bin')),
                 '-p','patches/CUSA00309.xml','--benchmark-dir','.')
    if($null -ne $verified.launcher_settings -and $verified.launcher_settings.show_fps){$arguments+='--show-fps'}
    if($WaitForDebugger){$arguments+='--wait-for-debugger'}
    $launch=[ordered]@{
        schema=3;path_base='project';argument_path_base='working_directory';session=$sessionRelative;mode=$Mode;resolution=$verified.resolution;benchmark=[bool]$Benchmark;debug=[bool]$WaitForDebugger
        runtime=$verified.runtime;runtime_sha256=$verified.runtime_sha256
        game_sha256=$verified.game_sha256;arguments=$arguments;working_directory=$sessionRelative
        user_root=(Get-RelativePath $projectRoot $portableUser);save_source=(Get-RelativePath $projectRoot $homeSource);game_launched=$false;complete=$false
        save_source_session=if($saveRecord){$SaveSession}else{$null};save_source_files=$saveFiles
        save_source_policy=$saveSourcePolicy;save_source_runtime_sha256=if($saveRecord){$saveRecord.Launch.runtime_sha256}else{$null}
        source_game_written=$false;prepared_at=(Get-Date).ToString('o')
        source_config_preserved=$true;acceptance_verified=$false
        audio_muted=[bool]($Benchmark -or $isDiagnostic -or $isTiming -or $SuperCapture)
        counter_kind='guest_flip';native_benchmark_cli=$verified.runtime_capabilities.benchmark_dir
        shared_cache=if($isDiagnostic -and -not $SuperCapture){$null}else{Get-RelativePath $projectRoot $runtimeState};launcher_settings=$verified.launcher_settings;quit_menu_key='F1'
        diagnostic=if($isDiagnostic){$Diagnostic}else{$null};progress_published=$false;super_capture=[bool]$SuperCapture
        super_capture_batch=$SuperCaptureBatch
        cpu_hotspots=[bool]$CpuHotspots
        readback_barrier_audit=[bool]$ReadbackBarrierAudit
        page_protect_audit=[bool]$PageProtectAudit
        compute_scope_audit=[bool]$ComputeScopeAudit
        readback_wait_scope_audit=[bool]$ReadbackWaitScopeAudit
        compute_protect_audit=[bool]$ComputeProtectAudit
        draw_prep_audit=[bool]$DrawPrepAudit
        readback_copy_stage_candidate=[bool]$ReadbackCopyStageCandidate
        image_read_transition_candidate=[bool]$ImageReadTransitionCandidate
        feedback_arena_candidate=[bool]$FeedbackArenaCandidate
        feedback_arena_group_candidate=[bool]$FeedbackArenaGroupCandidate
        feedback_arena_pool_candidate=$useFeedbackPool
        manual_super_pool=[bool]$SuperPool
        feedback_arena_tiled_candidate=[bool]$FeedbackArenaTiledCandidate
        vertex_input_key_candidate=[bool]$VertexInputKeyCandidate
        renderer_pgo=$RendererPgoTrial
        engine_policy='core-v1-v2'
        timing_profile=if($isTiming){$TimingProfile}else{$null};timing_overrides=$timingOverrides;timing_config_overrides=$timingConfigOverrides
    }
    Write-JsonUtf8 (Join-Path $session 'launch.json') $launch
    if($PrepareOnly){Write-Host ('Prepared session: '+$session);exit 0}
    if(-not (Test-Path -LiteralPath (Join-Path $portableUser 'config.json'))){throw 'Portable config missing: never allow AppData fallback.'}
    $start=[Diagnostics.ProcessStartInfo]::new()
    $start.FileName=$runtime;$start.WorkingDirectory=$session
    $start.Arguments=($arguments|ForEach-Object{Quote-WindowsArgument $_}) -join ' '
    $start.UseShellExecute=$false
    $start.EnvironmentVariables.Remove('IFSS_C73_SCAN_SPV')
    $start.EnvironmentVariables.Remove('IFSS_PARTICLE_POLL_PAUSE')
    $start.EnvironmentVariables.Remove('IFSS_EXPERIMENT_ACCESS_UNION')
    $start.EnvironmentVariables.Remove('IFSS_READBACK_BARRIER_AUDIT')
    if($ReadbackBarrierAudit){$start.EnvironmentVariables['IFSS_READBACK_BARRIER_AUDIT']='1'}
    $start.EnvironmentVariables.Remove('IFSS_PAGE_PROTECT_AUDIT')
    if($PageProtectAudit){$start.EnvironmentVariables['IFSS_PAGE_PROTECT_AUDIT']='1'}
    $start.EnvironmentVariables.Remove('IFSS_COMPUTE_SCOPE_AUDIT')
    if($ComputeScopeAudit){$start.EnvironmentVariables['IFSS_COMPUTE_SCOPE_AUDIT']='1'}
    $start.EnvironmentVariables.Remove('IFSS_READBACK_WAIT_SCOPE_AUDIT')
    if($ReadbackWaitScopeAudit){$start.EnvironmentVariables['IFSS_READBACK_WAIT_SCOPE_AUDIT']='1'}
    $start.EnvironmentVariables.Remove('IFSS_COMPUTE_PROTECT_AUDIT')
    if($ComputeProtectAudit){$start.EnvironmentVariables['IFSS_COMPUTE_PROTECT_AUDIT']='1'}
    $start.EnvironmentVariables.Remove('IFSS_DRAW_PREP_AUDIT')
    if($DrawPrepAudit){$start.EnvironmentVariables['IFSS_DRAW_PREP_AUDIT']='1'}
    $start.EnvironmentVariables.Remove('IFSS_READBACK_COPY_STAGE')
    if($ReadbackCopyStageCandidate){$start.EnvironmentVariables['IFSS_READBACK_COPY_STAGE']='1'}
    $start.EnvironmentVariables.Remove('IFSS_IMAGE_READ_TRANSITION')
    if($ImageReadTransitionCandidate){$start.EnvironmentVariables['IFSS_IMAGE_READ_TRANSITION']='1'}
    $start.EnvironmentVariables.Remove('IFSS_FEEDBACK_ARENA')
    $start.EnvironmentVariables.Remove('IFSS_FEEDBACK_ARENA_GROUP')
    $start.EnvironmentVariables.Remove('IFSS_FEEDBACK_ARENA_POOL')
    if($useFeedbackPool){$start.EnvironmentVariables['IFSS_FEEDBACK_ARENA_POOL']='1'}
    if($FeedbackArenaTiledCandidate){$start.EnvironmentVariables['IFSS_FEEDBACK_ARENA_POOL']='tiled64'}
    if($FeedbackArenaGroupCandidate){$start.EnvironmentVariables['IFSS_FEEDBACK_ARENA_GROUP']='1'}
    if($FeedbackArenaCandidate){$start.EnvironmentVariables['IFSS_FEEDBACK_ARENA']='1'}
    $start.EnvironmentVariables.Remove('IFSS_VERTEX_INPUT_KEY')
    if($VertexInputKeyCandidate){$start.EnvironmentVariables['IFSS_VERTEX_INPUT_KEY']='1'}
    $start.EnvironmentVariables.Remove('LLVM_PROFILE_FILE')
    if($RendererPgoTrial -eq 'Train'){
        $start.EnvironmentVariables['LLVM_PROFILE_FILE']=Join-Path $session 'renderer-pgo.profraw'
    }
    $launch.access_union_candidate=[bool]$AccessUnionCandidate
    if($AccessUnionCandidate){$start.EnvironmentVariables['IFSS_EXPERIMENT_ACCESS_UNION']='1'}
    if($ParticlePollPauseCandidate){$start.EnvironmentVariables['IFSS_PARTICLE_POLL_PAUSE']='1'}
    if($C73ScanCandidate){
        $scanSpv=Join-Path (Split-Path -Parent $runtime) 'c73-scan.spv'
        if(-not (Test-Path -LiteralPath $scanSpv -PathType Leaf)){throw 'Missing validated c73-scan.spv.'}
        $start.EnvironmentVariables['IFSS_C73_SCAN_SPV']=$scanSpv
    }
    $start.EnvironmentVariables['SHADPS4_GAME_PROFILE']='1'
    foreach($setting in @('xbox_prompts','touchpad_chords')){
        $property=$manifest.PSObject.Properties[$setting]
        $enabled=$true
        if($null -ne $property){
            if($property.Value -isnot [bool]){throw ('project.json '+$setting+' must be true or false.')}
            $enabled=$property.Value
        }
        $start.EnvironmentVariables['IFSS_'+$setting.ToUpperInvariant()]=if($enabled){'1'}else{'0'}
    }
    if($CriticalCapture){$start.EnvironmentVariables['IFSS_PATH_TRACE_ARMED']='1'}
    if($CpuHotspots){$start.EnvironmentVariables['IFSS_VTUNE_CONTROL']='1'}
    $start.EnvironmentVariables['IFSS_USER_ROOT']=Get-RelativePath $projectRoot $portableUser
    $running=Get-Process -Name ([IO.Path]::GetFileNameWithoutExtension($runtime)) -ErrorAction SilentlyContinue
    if($running){throw 'Another instance of the pinned engine is running. Close it before this session.'}
    if($SuperCaptureBatch){Assert-SuperCaptureStarting $projectRoot $SuperCaptureBatch}
    if($isDiagnostic){
        Set-DiagnosticEnvironment $start $session $diagnosticPlan
        $process=Start-DiagnosticRuntime $start $projectRoot $session $diagnosticPlan ([ref]$diagnosticWrapper)
    }else{$process=[Diagnostics.Process]::Start($start)}
    $launch.game_launched=$true;$launch.pid=$process.Id
    $launch.process_started=$process.StartTime.ToUniversalTime().ToString('o')
    if($diagnosticWrapper){$launch.diagnostic_wrapper=@{pid=$diagnosticWrapper.Id;started=$diagnosticWrapper.StartTime.ToUniversalTime().ToString('o')}}
    Write-JsonUtf8 (Join-Path $session 'launch.json') $launch
    if($SuperCaptureBatch){Publish-SuperCaptureSession $projectRoot $SuperCaptureBatch $launch}
    Write-JsonUtf8 (Join-Path $projectRoot 'state\active.json') ([ordered]@{
        schema=2;path_base='project';session=$sessionRelative;pid=$process.Id;runtime=$verified.runtime
        process_started=$launch.process_started;mode=$Mode;resolution=$verified.resolution
    })
    if($isDiagnostic){
        try{$collector=Start-DiagnosticCollector $projectRoot $session}
        catch{
            & (Join-Path $PSScriptRoot 'control.ps1') -Action quit -Session $sessionRelative | Out-Null
            throw
        }
        Write-Host ('DIAGNOSTIC: '+$Diagnostic+'; private caches; save/configuration changes will remain in this session.')
        Write-Host 'Automatic scene navigation, warmup, workload, capture and normal exit are enabled.'
        Write-Host ('Capture duration: '+$DiagnosticSeconds+' s after readiness; scene evidence is saved in diagnostics/automation.json.')
    }
    $brandingStart=[Diagnostics.ProcessStartInfo]::new()
    $brandingStart.FileName=[IO.Path]::Combine([Environment]::GetFolderPath('System'),'WindowsPowerShell/v1.0/powershell.exe');$brandingStart.WorkingDirectory=$projectRoot
    $brandingArguments=@('-NoLogo','-NoProfile','-ExecutionPolicy','Bypass','-File',(Join-Path $PSScriptRoot 'native.ps1'),'-Module','window-branding','-ArgumentsJson',(ConvertTo-Json -InputObject @('--session',$sessionRelative) -Compress))
    $brandingStart.Arguments=($brandingArguments|ForEach-Object{Quote-WindowsArgument $_}) -join ' '
    $brandingStart.UseShellExecute=$false;$brandingStart.CreateNoWindow=$true
    $brandingStart.WindowStyle=[Diagnostics.ProcessWindowStyle]::Hidden
    try{$windowBranding=[Diagnostics.Process]::Start($brandingStart)}
    catch{Write-Warning ('Game window identity could not be applied: '+$_.Exception.Message)}
    Write-Host ('Game PID: '+$process.Id+'; session: '+$session)
    if($isDiagnostic){Write-Host 'Unattended diagnostic: the game will exit automatically.'}
    else{Write-Host 'F1: exit confirmation; Enter: quit; Escape: return to game. You can also open quit.bat.'}
    if($WaitForDebugger){Write-Host 'Attach your existing native Windows debugger to this PID, then continue.'}
    if($isDiagnostic){Wait-DiagnosticAutomaticGame $projectRoot $sessionRelative $process $collector $diagnosticPlan}
    else{$process.WaitForExit()}
    if($windowBranding){$null=$windowBranding.WaitForExit(2000)}
    if($collector){
        Write-JsonUtf8 (Join-Path $session 'diagnostics/game-exit.json') @{
            target_pid=$launch.pid;target_started_utc=$launch.process_started
            exit_code=$process.ExitCode;observed_utc=[DateTime]::UtcNow.ToString('o')
        }
        Wait-DiagnosticAutomaticCollector $projectRoot $sessionRelative $collector $diagnosticPlan
        $diagnosticStatus=Join-Path $session 'diagnostics/status.json'
        if(Test-Path -LiteralPath $diagnosticStatus){
            $capture=Read-JsonUtf8 $diagnosticStatus
            $issues=Get-DiagnosticRuntimeIssues $projectRoot $session
            Write-JsonUtf8 (Join-Path $session 'diagnostics/runtime-errors.json') $issues
            $outcome=Get-DiagnosticOutcome $process.ExitCode $capture.status @($capture.errors) $issues
            foreach($key in $outcome.Keys){$capture|Add-Member NoteProperty $key $outcome[$key] -Force}
            if($outcome.status -ne 'collected'){$capture|Add-Member NoteProperty automation_phase 'failed' -Force}
            $capture|Add-Member NoteProperty runtime_errors_file ($sessionRelative+'/diagnostics/runtime-errors.json') -Force
            Write-JsonUtf8 $diagnosticStatus $capture
            $coveragePath=Join-Path $session 'diagnostics/coverage.json'
            if(Test-Path -LiteralPath $coveragePath){
                $coverage=Read-JsonUtf8 $coveragePath
                $coverage|Add-Member NoteProperty outcome $outcome -Force
                Write-JsonUtf8 $coveragePath $coverage
            }
            $launch.diagnostic_status=$capture.status
            $launch.diagnostic_failure_kind=$outcome.failure_kind
            $launch.runtime_errors_file=$capture.runtime_errors_file
            if($capture.errors.Count -or $capture.status -ne 'collected'){$diagnosticExitCode=1}
            Write-Host ('Diagnostic result: '+$capture.status+'; '+$diagnosticStatus)
            foreach($message in $capture.errors){Write-Host ('Collector error: '+$message) -ForegroundColor Red}
            foreach($message in $capture.runtime_warnings){Write-Warning $message}
            if($Diagnostic -eq 'full'){Write-Host ('Collector evidence: '+(Join-Path $session 'diagnostics/coverage.json'))}
        }else{$diagnosticExitCode=1;Write-Warning 'Collector did not finish; double-click diagnostics/control/status.bat or recover.bat.'}
    }
    if($diagnosticWrapper -and -not $diagnosticWrapper.HasExited -and -not $diagnosticWrapper.WaitForExit(30000)){throw 'Nsight launcher did not finish.'}
    if($isTiming){
        & (Resolve-InstalledPython) -X utf8 -B (Join-Path $PSScriptRoot 'analyze_timing.py') --session $session
        if($LASTEXITCODE -ne 0){Write-Warning 'Timing evidence is incomplete or invalid. Review timing-summary.json; do not treat this run as accepted performance evidence.'}
    }
    $launch.exit_code=$process.ExitCode
    $launch.finished_at=(Get-Date).ToString('o')
    $finalGameHash=(Get-FileHash -LiteralPath (Join-Path $projectRoot 'game\CUSA00309\eboot.bin') -Algorithm SHA256).Hash.ToLowerInvariant()
    if($finalGameHash -ne $manifest.game_sha256){throw 'Game executable changed; session progress was not published.'}
    $publishProgress=(-not $isDiagnostic -and -not $Benchmark -and -not $isTiming -and -not $SuperCapture -and $process.ExitCode -eq 0)
    if($publishProgress){
        $null=[IO.Directory]::CreateDirectory((Split-Path -Parent $liveHome))
        Copy-ProjectTree $projectRoot (Join-Path $session 'home') $liveHome
        Copy-ProjectTree $projectRoot (Join-Path $session 'custom_configs\CUSA00309.json') (Join-Path $portableUser 'custom_configs\CUSA00309.json')
        Copy-ProjectTree $projectRoot (Join-Path $session 'perf_tuning.ini') (Join-Path $portableUser 'perf_tuning.ini')
        foreach($relativeConfig in @('config.json','custom_configs/CUSA00309.json')){
            Convert-ProjectConfigPaths (Join-Path $portableUser $relativeConfig) $session $projectRoot
        }
        $null=[IO.Directory]::CreateDirectory($persistent)
        foreach($item in Get-ChildItem -LiteralPath $portableUser){
            if($item.Name -notin @('home','cache','shader','log','temp','screenshots','captures','download','play_time.txt')){
                Copy-ProjectTree $projectRoot $item.FullName (Join-Path $persistent $item.Name)
            }
        }
    }
    $launch.progress_published=$publishProgress
    $launch.complete=$true
    Write-JsonUtf8 (Join-Path $session 'launch.json') $launch
    if($isDiagnostic){exit (Get-DiagnosticResultCode $process.ExitCode $diagnosticExitCode)}
    if($null -eq $process.ExitCode){throw 'Game exit code is unavailable; success cannot be confirmed.'}
    exit $process.ExitCode
}catch{
    $launchFailure=$_
    if($isDiagnostic -and $session){
        try{
            $errorDirectory=Resolve-ProjectChild $projectRoot ((Get-RelativePath $projectRoot $session)+'/diagnostics')
            $null=[IO.Directory]::CreateDirectory($errorDirectory)
            Write-JsonUtf8 (Join-Path $errorDirectory 'launcher-error.json') (Get-DiagnosticErrorInfo $launchFailure $projectRoot)
        }catch{Write-Warning ('Could not persist launcher error: '+$_.Exception.Message)}
    }
    throw $launchFailure
}finally{
    try{
        if($process -and -not $process.HasExited){
            if($isDiagnostic){Wait-DiagnosticAutomaticGame $projectRoot $sessionRelative $process $collector $diagnosticPlan}
            else{$process.WaitForExit()}
        }
        if($collector -and -not $collector.HasExited){Wait-DiagnosticAutomaticCollector $projectRoot $sessionRelative $collector $diagnosticPlan}
        if($diagnosticWrapper -and -not $diagnosticWrapper.HasExited -and -not $diagnosticWrapper.WaitForExit(30000)){
            $diagnosticWrapper.Kill();$null=$diagnosticWrapper.WaitForExit(5000)
        }
    }finally{
        if($collector){$collector.Dispose()}
        if($diagnosticWrapper){$diagnosticWrapper.Dispose()}
        if($windowBranding){$windowBranding.Dispose()}
        if($process){$process.Dispose()}
    }
}
