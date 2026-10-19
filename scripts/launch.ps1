[CmdletBinding()]
param([ValidateSet('auto','720p','900p','1080p','1440p','1800p','pro4k')][string]$Mode='auto',
      [switch]$WaitForDebugger,[switch]$Benchmark,[switch]$CheckOnly,[switch]$PrepareOnly,[switch]$NoMenu)
Set-StrictMode -Version Latest
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot 'tools.ps1')
$projectRoot=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$manifest=Get-Content -LiteralPath (Join-Path $projectRoot 'project.json') -Raw -Encoding UTF8|ConvertFrom-Json

if($Benchmark){
    if($Mode -eq 'auto'){$Mode='pro4k'}
    if($Mode -notin @('1080p','pro4k')){throw 'Frozen benchmarks support only 1080p and pro4k.'}
}
if($CheckOnly){Invoke-ProjectNativeTool 'preflight' @('--mode',$Mode); exit $LASTEXITCODE}
$runtimeState=Resolve-ProjectChild $projectRoot 'state\runtime'
$null=[IO.Directory]::CreateDirectory($runtimeState)
$lock=$null;$process=$null;$windowBranding=$null
try{
    $lock=[IO.File]::Open((Resolve-ProjectChild $projectRoot 'state/runtime/run.lock'),[IO.FileMode]::OpenOrCreate,[IO.FileAccess]::ReadWrite,[IO.FileShare]::None)
    $runtime=Resolve-ProjectPath $projectRoot $manifest.runtime_path
    if(Get-Process -Name ([IO.Path]::GetFileNameWithoutExtension($runtime)) -ErrorAction SilentlyContinue){throw 'The pinned engine is already running.'}
    if(-not $Benchmark -and -not $PrepareOnly -and -not $NoMenu){
        $selection=@(Invoke-ProjectNativeTool 'launch-menu' @('--mode',$Mode))
        if($LASTEXITCODE -eq 1){exit 0}
        if($LASTEXITCODE -ne 0){throw 'Launch settings could not be opened; game was not started.'}
        $Mode=($selection -join '').Trim()
    }
    $sessionName=(Get-Date).ToString('yyyyMMdd-HHmmss-fff')+'-'+$Mode+'-'+[Guid]::NewGuid().ToString('N').Substring(0,8)
    $session=Resolve-ProjectChild $projectRoot ('state\sessions\'+$sessionName)
    $sessionRelative=Get-RelativePath $projectRoot $session
    $null=[IO.Directory]::CreateDirectory($session)
    $verifyArguments=@('--mode',$Mode,'--session',$sessionRelative)
    if($Benchmark){$verifyArguments+='--frozen'}
    $result=@(Invoke-ProjectNativeTool 'preflight' $verifyArguments)
    if($LASTEXITCODE -ne 0){throw 'Preflight failed; game was not started.'}
    $verified=($result -join [Environment]::NewLine)|ConvertFrom-Json
    $Mode=$verified.mode
    $runtime=Resolve-ProjectPath $projectRoot $verified.runtime
    $baseline=Resolve-ProjectChild $projectRoot 'state/baseline/user'
    $portableUser=Resolve-ProjectChild $projectRoot ($sessionRelative+'/user')
    $null=[IO.Directory]::CreateDirectory($portableUser)
    $persistent=Resolve-ProjectChild $projectRoot 'state\play\user'
    $configSource=if(-not $Benchmark -and (Test-Path -LiteralPath $persistent)){$persistent}else{$baseline}
    foreach($item in Get-ChildItem -LiteralPath $configSource){
        if($item.Name -notin @('home','custom_configs','cache','shader','log','temp','screenshots','captures','download','play_time.txt')){
            Copy-ProjectTree $projectRoot $item.FullName (Join-Path $portableUser $item.Name)
        }
    }
    Copy-ProjectTree $projectRoot (Join-Path $session 'custom_configs') (Join-Path $portableUser 'custom_configs')
    Ensure-ProjectQuitMenu $portableUser
    Convert-ProjectConfigPaths (Join-Path $portableUser 'config.json') $projectRoot $session
    if($null -ne $verified.launcher_settings){
        $globalPath=Join-Path $portableUser 'config.json'
        $globalConfig=Get-Content -LiteralPath $globalPath -Raw -Encoding UTF8|ConvertFrom-Json
        $globalConfig.General|Add-Member -MemberType NoteProperty -Name 'show_fps_counter' -Value ([bool]$verified.launcher_settings.show_fps) -Force
        foreach($key in @('full_screen','full_screen_mode','window_width','window_height')){
            $globalConfig.GPU|Add-Member -MemberType NoteProperty -Name $key -Value $verified.gpu_config.$key -Force
        }
        Write-JsonUtf8 $globalPath $globalConfig
    }
    $liveHome=Resolve-ProjectChild $projectRoot 'state\play\home'
    $homeSource=if($Benchmark -or -not (Test-Path -LiteralPath $liveHome)){Join-Path $baseline 'home'}else{$liveHome}
    Copy-ProjectTree $projectRoot $homeSource (Join-Path $session 'home')
    $null=New-Item -ItemType Junction -Path (Join-Path $portableUser 'home') -Target (Join-Path $session 'home')
    Copy-ProjectTree $projectRoot (Join-Path $configSource 'perf_tuning.ini') (Join-Path $session 'perf_tuning.ini')
    foreach($name in @('cache','shader')){
        $shared=Resolve-ProjectChild $projectRoot ('state/runtime/'+$name)
        $null=[IO.Directory]::CreateDirectory($shared)
        $link=Join-Path $portableUser $name
        if(Test-Path -LiteralPath $link){throw 'Unexpected pre-existing cache directory in a fresh session.'}
        $null=New-Item -ItemType Junction -Path $link -Target $shared
    }
    $configPath=Join-Path $portableUser 'custom_configs\CUSA00309.json'
    if($Benchmark){
        $config=Get-Content -LiteralPath $configPath -Raw -Encoding UTF8|ConvertFrom-Json
        $config.General.volume_slider=0
        Write-JsonUtf8 $configPath $config
        Write-JsonUtf8 (Join-Path $session 'custom_configs\CUSA00309.json') $config
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
        source_game_written=$false;prepared_at=(Get-Date).ToString('o')
        source_config_preserved=$true;acceptance_verified=$false
        counter_kind='guest_flip';native_benchmark_cli=$verified.runtime_capabilities.benchmark_dir
        shared_cache=(Get-RelativePath $projectRoot $runtimeState);launcher_settings=$verified.launcher_settings;quit_menu_key='F1'
    }
    Write-JsonUtf8 (Join-Path $session 'launch.json') $launch
    if($PrepareOnly){Write-Host ('Prepared session: '+$session);exit 0}
    if(-not (Test-Path -LiteralPath (Join-Path $portableUser 'config.json'))){throw 'Portable config missing: never allow AppData fallback.'}
    $start=[Diagnostics.ProcessStartInfo]::new()
    $start.FileName=$runtime;$start.WorkingDirectory=$session
    $start.Arguments=($arguments|ForEach-Object{Quote-WindowsArgument $_}) -join ' '
    $start.UseShellExecute=$false
    $start.EnvironmentVariables['SHADPS4_GAME_PROFILE']='1'
    $start.EnvironmentVariables['IFSS_USER_ROOT']=Get-RelativePath $projectRoot $portableUser
    $running=Get-Process -Name ([IO.Path]::GetFileNameWithoutExtension($runtime)) -ErrorAction SilentlyContinue
    if($running){throw 'Another instance of the pinned engine is running. Close it before this session.'}
    $process=[Diagnostics.Process]::Start($start)
    $launch.game_launched=$true;$launch.pid=$process.Id
    $launch.process_started=$process.StartTime.ToUniversalTime().ToString('o')
    Write-JsonUtf8 (Join-Path $session 'launch.json') $launch
    Write-JsonUtf8 (Join-Path $projectRoot 'state\active.json') ([ordered]@{
        schema=2;path_base='project';session=$sessionRelative;pid=$process.Id;runtime=$verified.runtime
        process_started=$launch.process_started;mode=$Mode;resolution=$verified.resolution
    })
    $brandingStart=[Diagnostics.ProcessStartInfo]::new()
    $brandingStart.FileName=[IO.Path]::Combine([Environment]::GetFolderPath('System'),'WindowsPowerShell/v1.0/powershell.exe');$brandingStart.WorkingDirectory=$projectRoot
    $brandingArguments=@('-NoLogo','-NoProfile','-ExecutionPolicy','Bypass','-File',(Join-Path $PSScriptRoot 'native.ps1'),'-Module','window-branding','-ArgumentsJson',(ConvertTo-Json -InputObject @('--session',$sessionRelative) -Compress))
    $brandingStart.Arguments=($brandingArguments|ForEach-Object{Quote-WindowsArgument $_}) -join ' '
    $brandingStart.UseShellExecute=$false;$brandingStart.CreateNoWindow=$true
    $brandingStart.WindowStyle=[Diagnostics.ProcessWindowStyle]::Hidden
    try{$windowBranding=[Diagnostics.Process]::Start($brandingStart)}
    catch{Write-Warning ('Game window identity could not be applied: '+$_.Exception.Message)}
    Write-Host ('Game PID: '+$process.Id+'; session: '+$session)
    Write-Host 'F1: exit confirmation; Enter: quit; Escape: return to game. You can also open quit.bat.'
    if($WaitForDebugger){Write-Host 'Attach your existing native Windows debugger to this PID, then continue.'}
    $process.WaitForExit()
    if($windowBranding){$null=$windowBranding.WaitForExit(2000)}
    $launch.exit_code=$process.ExitCode
    $launch.finished_at=(Get-Date).ToString('o')
    $finalGameHash=(Get-FileHash -LiteralPath (Join-Path $projectRoot 'game\CUSA00309\eboot.bin') -Algorithm SHA256).Hash.ToLowerInvariant()
    if($finalGameHash -ne $manifest.game_sha256){throw 'Game executable changed; session progress was not published.'}
    if(-not $Benchmark -and $process.ExitCode -eq 0){
        $null=[IO.Directory]::CreateDirectory((Split-Path -Parent $liveHome))
        Copy-ProjectTree $projectRoot (Join-Path $session 'home') $liveHome
        Copy-ProjectTree $projectRoot (Join-Path $session 'custom_configs\CUSA00309.json') (Join-Path $portableUser 'custom_configs\CUSA00309.json')
        Copy-ProjectTree $projectRoot (Join-Path $session 'perf_tuning.ini') (Join-Path $portableUser 'perf_tuning.ini')
        Convert-ProjectConfigPaths (Join-Path $portableUser 'config.json') $session $projectRoot
        $null=[IO.Directory]::CreateDirectory($persistent)
        foreach($item in Get-ChildItem -LiteralPath $portableUser){
            if($item.Name -notin @('home','cache','shader','log','temp','screenshots','captures','download','play_time.txt')){
                Copy-ProjectTree $projectRoot $item.FullName (Join-Path $persistent $item.Name)
            }
        }
    }
    $launch.complete=$true
    Write-JsonUtf8 (Join-Path $session 'launch.json') $launch
    exit $process.ExitCode
}finally{
    # Keep ownership until the engine exits, including after a launcher error.
    if($process -and -not $process.HasExited){$process.WaitForExit()}
    if($windowBranding){$windowBranding.Dispose()}
    if($process){$process.Dispose()}
    if($lock){$lock.Dispose()}
}
