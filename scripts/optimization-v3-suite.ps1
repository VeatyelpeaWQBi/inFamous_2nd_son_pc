[CmdletBinding()]
param(
    [ValidateSet('1080p','pro4k')][string]$Mode='pro4k',
    [ValidateRange(15,600)][int]$Seconds=60,
    [ValidateRange(5,120)][int]$WarmupSeconds=20,
    [ValidateRange(1,10)][int]$Runs=1,
    [string[]]$Profiles=@('baseline','v3-01','v3-02','v3-03','v3-04','v3-05','v3-06',
                         'v3-07','v3-08','v3-09','v3-10','v3-11','v3-12','baseline-final'),
    [switch]$StopOnFailure
)
Set-StrictMode -Version Latest
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot 'tools.ps1')
. (Join-Path $PSScriptRoot 'diagnostic_tools.ps1')

$root=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$tuningPath=Join-Path $root 'state/play/user/perf_tuning.ini'
$expectedSaveSource=Get-RelativePath $root (Resolve-ProjectChild $root 'state/play/home')
if(-not(Test-Path -LiteralPath $tuningPath -PathType Leaf)){
    throw 'state/play/user/perf_tuning.ini is missing. The suite does not create a replacement play environment.'
}
$originalTuning=[IO.File]::ReadAllBytes($tuningPath)
$stamp=(Get-Date).ToString('yyyyMMdd-HHmmss')
$suite=Resolve-ProjectChild $root ('state/optimization-v3/'+$stamp)
$null=[IO.Directory]::CreateDirectory($suite)
$powershell=[IO.Path]::Combine([Environment]::GetFolderPath('System'),'WindowsPowerShell/v1.0/powershell.exe')
$results=[Collections.Generic.List[object]]::new()

function Get-Percentile([double[]]$Sorted,[double]$P) {
    if(-not $Sorted.Count){return $null}
    $index=[Math]::Max(0,[Math]::Min($Sorted.Count-1,[Math]::Ceiling($P*$Sorted.Count)-1))
    return $Sorted[$index]
}

function Get-FrameMetrics([string]$FramesPath,[DateTime]$StartUtc,[int]$DurationSeconds) {
    if(-not(Test-Path -LiteralPath $FramesPath -PathType Leaf)){return $null}
    $startNs=[DateTimeOffset]::new($StartUtc).ToUnixTimeMilliseconds()*1000000L
    $endNs=$startNs+[long]$DurationSeconds*1000000000L
    [double[]]$milliseconds=@(Import-Csv -LiteralPath $FramesPath | ForEach-Object {
        $utc=[long]$_.utc_ns;$interval=[long]$_.interval_ns
        if($utc -ge $startNs -and $utc -lt $endNs -and $interval -gt 0){$interval/1000000.0}
    })
    if($milliseconds.Count -lt 30){return $null}
    [Array]::Sort($milliseconds)
    $mean=($milliseconds|Measure-Object -Average).Average
    return [ordered]@{
        samples=$milliseconds.Count
        fps=[Math]::Round(1000.0/$mean,3)
        mean_ms=[Math]::Round($mean,4)
        median_ms=[Math]::Round((Get-Percentile $milliseconds 0.50),4)
        p95_ms=[Math]::Round((Get-Percentile $milliseconds 0.95),4)
        p99_ms=[Math]::Round((Get-Percentile $milliseconds 0.99),4)
        max_ms=[Math]::Round($milliseconds[-1],4)
        one_percent_low_fps=[Math]::Round(1000.0/(Get-Percentile $milliseconds 0.99),3)
        frames_over_33ms=@($milliseconds|Where-Object {$_ -gt 33.333}).Count
    }
}

function Stop-V3Game([string]$Session,[Diagnostics.Process]$Launcher) {
    try{& (Join-Path $PSScriptRoot 'control.ps1') -Action axis -Axis 1 -Value 128 -Session $Session | Out-Null}catch{}
    try{& (Join-Path $PSScriptRoot 'control.ps1') -Action axis -Axis 2 -Value 128 -Session $Session | Out-Null}catch{}
    try{& (Join-Path $PSScriptRoot 'control.ps1') -Action quit -Session $Session | Out-Null}catch{}
    if($Launcher -and -not $Launcher.HasExited -and -not $Launcher.WaitForExit(45000)){
        $record=Read-ProjectSession $root $Session
        $target=Get-DiagnosticTarget $record.Launch $root
        if($target){
            try{$target.Kill();$null=$target.WaitForExit(10000)}finally{$target.Dispose()}
        }
        $null=$Launcher.WaitForExit(15000)
    }
}

function Save-LogExtract([string]$SessionPath,[string]$Destination) {
    $logPaths=@(Get-ChildItem -LiteralPath $SessionPath -File -Recurse -ErrorAction SilentlyContinue |
        Where-Object {$_.Extension -in @('.log','.txt') -and $_.Length -le 256MB})
    $writer=[IO.StreamWriter]::new($Destination,$false,[Text.UTF8Encoding]::new($false))
    try{
        foreach($log in $logPaths){
            $writer.WriteLine('===== '+(Get-RelativePath $SessionPath $log.FullName)+' =====')
            Get-Content -LiteralPath $log.FullName -ErrorAction SilentlyContinue | Where-Object {
                $_ -match 'V3 |Perf |<Error>|<Critical>|device lost|assert|pipeline|shader'
            } | Select-Object -First 20000 | ForEach-Object {$writer.WriteLine($_)}
        }
    }finally{$writer.Dispose()}
    return @($logPaths|ForEach-Object {Get-RelativePath $root $_.FullName})
}

Write-JsonUtf8 (Join-Path $suite 'plan.json') ([ordered]@{
    schema=1;mode=$Mode;seconds=$Seconds;warmup_seconds=$WarmupSeconds;runs=$Runs
    profiles=$Profiles;created_utc=[DateTime]::UtcNow.ToString('o')
    expected_save_source=$expectedSaveSource
    policy='one isolated game process per profile; fixed high-pressure save copy; profile-compatible cache reuse; identical OCR navigation, warmup and input schedule'
})

try{
    for($run=1;$run -le $Runs;$run++){
        foreach($profile in $Profiles){
            $runName=('run-{0:D2}-{1}' -f $run,$profile.ToLowerInvariant())
            $runDir=Join-Path $suite $runName
            $null=[IO.Directory]::CreateDirectory($runDir)
            $launcher=$null;$session=$null;$captureStart=$null;$auto=$null
            $failure=$null;$started=[DateTime]::UtcNow
            try{
                & $powershell -NoLogo -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot 'optimization-v3.ps1') -Action Set -Profile $profile -Automation | Out-File -LiteralPath (Join-Path $runDir 'switches.log') -Encoding utf8
                if($LASTEXITCODE -ne 0){throw ('Could not apply profile '+$profile)}
                [IO.File]::Copy($tuningPath,(Join-Path $runDir 'perf_tuning.ini'),$true)

                $arguments=@('-NoLogo','-NoProfile','-ExecutionPolicy','Bypass','-File',
                    (Join-Path $PSScriptRoot 'launch.ps1'),'-Mode',$Mode,'-V3Test','-NoMenu')
                $launcher=Start-Process -FilePath $powershell -ArgumentList (($arguments|ForEach-Object {Quote-WindowsArgument $_}) -join ' ') -WorkingDirectory $root -WindowStyle Hidden -PassThru -RedirectStandardOutput (Join-Path $runDir 'launcher-output.log') -RedirectStandardError (Join-Path $runDir 'launcher-error.log')

                $deadline=[DateTime]::UtcNow.AddSeconds(180)
                while([DateTime]::UtcNow -lt $deadline){
                    if($launcher.HasExited){throw ('Launcher exited before the game started: '+$launcher.ExitCode)}
                    $activePath=Join-Path $root 'state/active.json'
                    if(Test-Path -LiteralPath $activePath){
                        $active=Read-JsonUtf8 $activePath
                        if([DateTime]::Parse($active.process_started).ToUniversalTime() -ge $started.AddSeconds(-2)){
                            $session=[string]$active.session
                            break
                        }
                    }
                    Start-Sleep -Milliseconds 500
                }
                if(-not $session){throw 'Timed out waiting for the V3 game session.'}
                $record=Read-ProjectSession $root $session
                if([string]$record.Launch.save_source -ne $expectedSaveSource){
                    throw ('V3 session used the wrong save source: '+[string]$record.Launch.save_source+
                           '; expected '+$expectedSaveSource)
                }
                $automationDir=Join-Path $record.Path 'diagnostics'
                $null=[IO.Directory]::CreateDirectory($automationDir)
                $plan=[ordered]@{
                    profile='memory';workload_profile='v3-performance';seconds=$Seconds;delay_seconds=0;wpr_profile=''
                    gpu_capture_backend='engine';validation_features=@{gpu_assisted=$false}
                    capture_schedule=@{guest_trace_seconds=2147483647;renderdoc_seconds=2147483647}
                    automation=@{enabled=$true;boot_timeout_seconds=240;warmup_seconds=$WarmupSeconds;session_timeout_seconds=(420+$WarmupSeconds+$Seconds);language='en-US';pause_probe=$false}
                }
                $auto=New-DiagnosticAutomation $plan
                $clock=[Diagnostics.Stopwatch]::StartNew()
                while($true){
                    $record=Read-ProjectSession $root $session
                    $game=Get-DiagnosticTarget $record.Launch $root
                    if(-not $game){break}
                    try{
                        $elapsed=$clock.Elapsed.TotalSeconds
                        Update-DiagnosticAutomation $root $session $plan $auto $elapsed
                        if($auto.ready -and $null -eq $auto.capture_at -and ($elapsed-$auto.ready_at) -ge $WarmupSeconds){
                            $auto.capture_at=$elapsed;$auto.phase='recording'
                            $captureStart=[DateTime]::UtcNow
                            # Dispatch the time-zero workload command in the same loop that starts
                            # the measurement window, instead of waiting for the next 750 ms poll.
                            Update-DiagnosticAutomation $root $session $plan $auto $elapsed
                        }
                        if($null -ne $auto.capture_at -and ($elapsed-$auto.capture_at) -ge $Seconds){
                            Stop-V3Game $session $launcher
                            break
                        }
                        if($elapsed -gt $plan.automation.session_timeout_seconds){throw 'V3 automation timed out.'}
                    }finally{$game.Dispose()}
                    Start-Sleep -Milliseconds 750
                }
                if($launcher -and -not $launcher.HasExited){$null=$launcher.WaitForExit(60000)}
                if($launcher -and -not $launcher.HasExited){throw 'Launcher did not finish after the game exited.'}
                $launcher.WaitForExit();$launcher.Refresh()
                $launcherExitCode=$launcher.ExitCode
                if($null -eq $launcherExitCode){
                    $record=Read-ProjectSession $root $session
                    $recordedExitCode=$record.Launch.PSObject.Properties['exit_code']
                    if($record.Launch.complete -and $null -ne $recordedExitCode){
                        $launcherExitCode=[int]$recordedExitCode.Value
                    }
                }
                if($null -eq $launcherExitCode){throw 'Launcher exit code was unavailable.'}
                if($launcherExitCode -ne 0){throw ('Launcher returned '+$launcherExitCode)}
                if(-not $captureStart){throw 'No stable gameplay measurement window was reached.'}

                $record=Read-ProjectSession $root $session
                $metrics=Get-FrameMetrics (Join-Path $record.Path 'frames.csv') $captureStart $Seconds
                if(-not $metrics){throw 'The fixed window did not contain enough frame samples.'}
                $logs=Save-LogExtract $record.Path (Join-Path $runDir 'engine-perf.log')
                $result=[ordered]@{
                    run=$run;profile=$profile;status='complete';session=$session
                    save_source=[string]$record.Launch.save_source
                    capture_started_utc=$captureStart.ToString('o');duration_seconds=$Seconds
                    startup_to_ready_seconds=[Math]::Round([double]$auto.ready_at,3)
                    scene_evidence=$auto.scene_evidence;input_suspended=[bool]$auto.input_suspended
                    metrics=$metrics;logs=$logs
                }
            }catch{
                $failure=$_.Exception.Message
                if($session){Stop-V3Game $session $launcher}
                $result=[ordered]@{run=$run;profile=$profile;status='failed';session=$session;error=$failure}
            }finally{
                if($launcher){$launcher.Dispose()}
            }
            Write-JsonUtf8 (Join-Path $runDir 'result.json') $result
            $results.Add([pscustomobject]$result)
            if($failure -and $StopOnFailure){throw $failure}
        }
    }
}finally{
    [IO.File]::WriteAllBytes($tuningPath,$originalTuning)
}

$baselineByRun=@{}
foreach($item in $results){
    if($item.status -eq 'complete' -and $item.profile -eq 'baseline' -and
       -not $baselineByRun.ContainsKey([string]$item.run)){
        $baselineByRun[[string]$item.run]=$item.metrics
    }
}
$summary=@($results|ForEach-Object {
    $row=[ordered]@{run=$_.run;profile=$_.profile;status=$_.status;session=$_.session}
    if($_.status -eq 'complete'){
        foreach($key in @('fps','mean_ms','median_ms','p95_ms','p99_ms','max_ms','one_percent_low_fps','frames_over_33ms','samples')){$row[$key]=$_.metrics[$key]}
        $row['startup_to_ready_seconds']=$_.startup_to_ready_seconds
        $runBaseline=$baselineByRun[[string]$_.run]
        if($runBaseline){
            $row['fps_gain_percent']=[Math]::Round(100*($_.metrics.fps-$runBaseline.fps)/$runBaseline.fps,3)
            $row['p95_improvement_percent']=[Math]::Round(100*($runBaseline.p95_ms-$_.metrics.p95_ms)/$runBaseline.p95_ms,3)
        }
    }else{$row['error']=$_.error}
    [pscustomobject]$row
})
$summary|Export-Csv -LiteralPath (Join-Path $suite 'summary.csv') -NoTypeInformation -Encoding utf8
Write-JsonUtf8 (Join-Path $suite 'summary.json') ([ordered]@{
    schema=1;completed_utc=[DateTime]::UtcNow.ToString('o');baseline_profile='baseline';results=$summary
    interpretation='Positive fps_gain_percent and p95_improvement_percent are improvements. Automated results are screening evidence; image and gameplay correctness still require human review.'
})
$failedCount=@($results|Where-Object status -eq 'failed').Count
if($failedCount){
    Write-Host ('V3 suite failed: '+$failedCount+' of '+$results.Count+' runs failed. Results: '+$suite) -ForegroundColor Red
    exit 1
}
Write-Host ('V3 suite complete: '+$suite)
exit 0
