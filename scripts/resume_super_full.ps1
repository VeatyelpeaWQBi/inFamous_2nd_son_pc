# SPDX-License-Identifier: GPL-2.0-or-later
[CmdletBinding()]
param([Parameter(Mandatory)][string]$Session,[switch]$ScanOnly,[switch]$RebuildVtune)
Set-StrictMode -Version Latest
$ErrorActionPreference='Stop'
if($PSVersionTable.PSVersion.Major -ne 5){throw 'Use the installed Windows PowerShell 5.1.'}
. (Join-Path $PSScriptRoot 'tools.ps1')
. (Join-Path $PSScriptRoot 'diagnostic_tools.ps1')
$root=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$record=Read-ProjectSession $root $Session;$sessionPath=$record.Path;$launch=$record.Launch
$dir=Join-Path $sessionPath 'diagnostics';$plan=Read-JsonUtf8 (Join-Path $dir 'plan.json')
if($plan.profile -ne 'full' -or -not $plan.PSObject.Properties['super_capture']){throw 'This is not a Full super session.'}
if(Get-DiagnosticTarget $launch $root){throw 'The original game is still running; offline recovery is not allowed.'}
if(Get-Process -Name vtune,xperf,ngfx -ErrorAction SilentlyContinue){throw 'Another profiler is running; finish it before offline recovery.'}
$gameExit=Read-JsonUtf8 (Join-Path $dir 'game-exit.json')
if($gameExit.target_pid -ne $launch.pid -or $gameExit.target_started_utc -ne $launch.process_started -or $gameExit.exit_code -ne 0){throw 'A matching successful game-exit receipt is required; no exit code will be invented.'}
$area=Join-Path $dir 'recovery';$null=[IO.Directory]::CreateDirectory($area)
$lock=[IO.File]::Open((Join-Path $area 'recovery.lock'),[IO.FileMode]::OpenOrCreate,[IO.FileAccess]::ReadWrite,[IO.FileShare]::None)
$run=Join-Path $area (Get-Date -Format 'yyyyMMdd-HHmmss-fff');$null=[IO.Directory]::CreateDirectory($run)
$progressPath=Join-Path $area 'status.json';$preflight=$null
$script:recoveryProgress=[ordered]@{session=$Session;status='scanning';stage=$null;pid=$null;started_utc=[DateTime]::UtcNow.ToString('o');updated_utc=$null;error=$null}
function Save-RecoveryProgress {
    $script:recoveryProgress.updated_utc=[DateTime]::UtcNow.ToString('o')
    Write-JsonUtf8 $progressPath $script:recoveryProgress
}
function Get-RecoveryKey([string]$Executable,[string[]]$Arguments) {
    $sha=[Security.Cryptography.SHA256]::Create()
    try{return ([BitConverter]::ToString($sha.ComputeHash([Text.Encoding]::UTF8.GetBytes(($Executable+'|'+($Arguments|ConvertTo-Json -Compress)+'|'+$script:recoveryInput))))).Replace('-','').ToLowerInvariant()}
    finally{$sha.Dispose()}
}
# Local override: only this recovery process uses low priority, live log files,
# resource guards and durable per-command completion receipts.
function Invoke-DiagnosticNative([string]$Executable,[string[]]$Arguments,[string]$Log,[int]$TimeoutMilliseconds=120000) {
    $key=Get-RecoveryKey $Executable $Arguments;$checkpoint=Join-Path $area ($key+'.complete.json')
    $outputs=@()
    foreach($option in @('-report-output','-o')){
        $index=[Array]::IndexOf($Arguments,$option)
        if($index -ge 0 -and $index+1 -lt $Arguments.Count){$outputs+=@($Arguments[$index+1])}
    }
    $force=$RebuildVtune -and $Arguments -contains '-finalize'
    if(-not $force -and (Test-Path -LiteralPath $checkpoint)){
        $saved=Read-JsonUtf8 $checkpoint;$valid=$saved.key -eq $key
        foreach($output in @($saved.outputs)){
            if(-not(Test-Path -LiteralPath $output.path) -or (Get-FileHash -LiteralPath $output.path -Algorithm SHA256).Hash -ne $output.sha256){$valid=$false;break}
        }
        if($valid){Write-Host ('reuse completed step: '+[IO.Path]::GetFileName($Log));return}
    }
    if([IO.Path]::GetFileName($Executable) -eq 'xperf.exe' -and $Arguments -contains 'tracestats' -and $preflight.checks.wpr.validated){
        Write-Host 'reuse validated WPR statistics; skip full ETL reread.'
        return
    }
    $mayFallback=[IO.Path]::GetFileName($Executable) -in @('vtune.exe','amplxe-cl.exe') -and
        $Arguments -contains '-report' -and $Arguments -contains '-report-output'
    $attemptPlan=@(@{label='primary';unguarded=$false})
    if($mayFallback){ $attemptPlan+=@{label='unguarded-after-guard-crash';unguarded=$true} }
    $lastFailure=$null
    foreach($attempt in $attemptPlan){
        $stdout=Join-Path $area ($key+'.'+$attempt.label+'.stdout.log')
        $stderr=Join-Path $area ($key+'.'+$attempt.label+'.stderr.log')
        $invocation=Resolve-DiagnosticNativeInvocation $Executable $Arguments $Log -ForceUnguarded:$($attempt.unguarded)
        if($attempt.unguarded -and $invocation.Guarded){ throw 'Internal error: unguarded fallback remained guarded.' }
        $argLine=(($invocation.Arguments|ForEach-Object{Quote-WindowsArgument $_}) -join ' ')
        $options=@{FilePath=$invocation.Executable;ArgumentList=$argLine;WorkingDirectory=$root;WindowStyle='Hidden';PassThru=$true;RedirectStandardOutput=$stdout;RedirectStandardError=$stderr}
        $process=Start-DiagnosticProcess $options;$clock=[Diagnostics.Stopwatch]::StartNew();$failure=$null;$last=-10
        try{
            $process.PriorityClass=[Diagnostics.ProcessPriorityClass]::BelowNormal
            $script:recoveryProgress.status='running';$script:recoveryProgress.stage=[IO.Path]::GetFileName($Log)
            $script:recoveryProgress.pid=$process.Id;Save-RecoveryProgress
            $stageMsg='recovery step: '+$script:recoveryProgress.stage
            if($attempt.unguarded){ $stageMsg=$stageMsg+' (unguarded fallback)' }
            Write-Host $stageMsg
            while(-not $process.WaitForExit(1000)){
                $available=[long]$script:recoveryMemory.AvailablePhysicalMemory
                $free=([IO.DriveInfo]::new([IO.Path]::GetPathRoot($root))).AvailableFreeSpace
                if($available -lt $script:recoveryMemoryFloor){$failure='Available physical memory reached the recovery reserve; the owned parser was stopped. Raw data and completed checkpoints are retained.';break}
                if($free -lt 2GB){$failure='Disk free space reached the 2 GiB reserve; the owned parser was stopped.';break}
                if($clock.Elapsed.TotalMilliseconds -gt $TimeoutMilliseconds){$failure='Recovery tool exceeded its bounded timeout.';break}
                if($clock.Elapsed.TotalSeconds-$last -ge 5){
                    $process.Refresh();$script:recoveryProgress.elapsed_seconds=$clock.Elapsed.TotalSeconds
                    $script:recoveryProgress.cpu_seconds=$process.TotalProcessorTime.TotalSeconds
                    $script:recoveryProgress.working_set_bytes=$process.WorkingSet64
                    $script:recoveryProgress.available_memory_bytes=$available
                    Save-RecoveryProgress;$last=$clock.Elapsed.TotalSeconds
                }
            }
            if($failure){if(-not $process.HasExited){$process.Kill()};$process.WaitForExit();throw $failure}
            $process.WaitForExit()
            $text=''
            foreach($file in @($stdout,$stderr)){if(Test-Path -LiteralPath $file){$text+=[IO.File]::ReadAllText($file)}}
            if($attempt.unguarded){ $text=('VTune report fallback: ifss_vtune_report_guard crashed; retried unguarded vtune.exe.'+[Environment]::NewLine+$text) }
            [IO.File]::WriteAllText($Log,$text,[Text.UTF8Encoding]::new($false))
            if($process.ExitCode -eq 0){
                $evidence=@()
                foreach($output in $outputs){
                    if(-not(Test-Path -LiteralPath $output) -or (Get-Item -LiteralPath $output).Length -eq 0){throw ('Missing recovered output: '+$output)}
                    $evidence+=@{path=$output;sha256=(Get-FileHash -LiteralPath $output -Algorithm SHA256).Hash}
                }
                Write-JsonUtf8 $checkpoint @{key=$key;completed_utc=[DateTime]::UtcNow.ToString('o');outputs=$evidence;exit_code=0;attempt=$attempt.label}
                return
            }
            $lastFailure='Recovery tool returned '+$process.ExitCode+'; see '+$stderr
            if($invocation.Guarded -and (Test-DiagnosticNativeCrashExit $process.ExitCode) -and $mayFallback){
                Write-Host ('Guard crashed ('+$process.ExitCode+'); retrying unguarded report.')
                continue
            }
            throw $lastFailure
        }finally{
            if($process -and -not $process.HasExited){$process.Kill();$process.WaitForExit()}
            if($process){$process.Dispose()}
            $script:recoveryProgress.pid=$null
        }
    }
    throw $lastFailure
}
try{
    foreach($name in @('status.json','automation.json','coverage.json','artifact-validation.json','data-quality.json')){
        $path=Join-Path $dir $name;if(Test-Path -LiteralPath $path){Copy-Item -LiteralPath $path -Destination (Join-Path $run $name)}
    }
    Copy-Item -LiteralPath (Join-Path $sessionPath 'launch.json') -Destination (Join-Path $run 'launch.json')
    $inputs=@(Get-Item -LiteralPath (Join-Path $dir 'timeline.etl'))+@(Get-ChildItem -LiteralPath (Join-Path $dir 'vtune/data.0') -File)
    $script:recoveryInput=@($inputs|Sort-Object FullName|ForEach-Object{@{path=$_.FullName;length=$_.Length;modified_utc=$_.LastWriteTimeUtc.ToString('o')}})|ConvertTo-Json -Compress
    Write-JsonUtf8 (Join-Path $run 'raw-inputs.json') ($script:recoveryInput|ConvertFrom-Json)
    Add-Type -AssemblyName Microsoft.VisualBasic
    $script:recoveryMemory=New-Object Microsoft.VisualBasic.Devices.ComputerInfo
    $script:recoveryMemoryFloor=[long][Math]::Min(8GB,[Math]::Max(2GB,($script:recoveryMemory.TotalPhysicalMemory/8)))
    $scanPath=Join-Path $run 'preflight.json'
    $scanSource='import sys,json; from pathlib import Path; sys.path.insert(0,"scripts"); import diagnostic_quality as q; result=q.inspect(Path(sys.argv[1]),Path.cwd()); Path(sys.argv[2]).write_text(json.dumps(result,ensure_ascii=False,indent=2),encoding="utf-8")'
    Invoke-DiagnosticNative (Resolve-InstalledPython) @('-B','-c',$scanSource,$sessionPath,$scanPath) (Join-Path $run 'preflight.log') 120000
    $preflight=Read-JsonUtf8 $scanPath
    Write-Host ('扫描结果：'+($preflight.checks|ConvertTo-Json -Depth 3 -Compress))
    if($ScanOnly){$script:recoveryProgress.status='scan-complete';Save-RecoveryProgress;exit 0}
    foreach($required in @('wpr','frames','symbols','nsight','diagnostic_exit','vtune_control')){
        if(-not $preflight.checks.PSObject.Properties[$required] -or -not $preflight.checks.$required.validated){throw ('Recovery preflight failed: '+$required+'; see '+$scanPath)}
    }
    $state=[ordered]@{};(Read-JsonUtf8 (Join-Path $dir 'status.json')).PSObject.Properties|ForEach-Object{$state[$_.Name]=$_.Value}
    $state.recovery=@{method='offline-after-host-restart';started_utc=$script:recoveryProgress.started_utc;original_launcher_exit_code=$null;backup=(Get-RelativePath $root $run)}
    $state.errors=@();$state.complete=$false
    $state.artifact_validation=Complete-DiagnosticArtifacts $root $sessionPath $plan $state
    $state.complete=$true;$state.finished_at=[DateTime]::UtcNow.ToString('o')
    $state.status=if($state.data_quality.complete -and -not $state.errors.Count){'collected'}else{'partial'}
    $state.automation_phase=if($state.status -eq 'collected'){'finished'}else{'failed'}
    Write-JsonUtf8 (Join-Path $dir 'status.json') $state
    $auto=Read-JsonUtf8 (Join-Path $dir 'automation.json')
    $coverage=@(Get-DiagnosticCoverage $root $sessionPath $plan $state $auto)
    Write-JsonUtf8 (Join-Path $area 'recovered-coverage.json') @{target_pid=$launch.pid;collectors=$coverage;recovery=$state.recovery}
    if($state.status -ne 'collected'){throw ('Recovered exports remain incomplete: '+($state.errors -join '; '))}
    $launch|Add-Member NoteProperty recovery $state.recovery -Force
    $launch.complete=$true;$launch.exit_code=$gameExit.exit_code
    Write-JsonUtf8 (Join-Path $sessionPath 'launch.json') $launch
    $batchPath=Resolve-ProjectChild $root $plan.super_capture.batch
    $batch=Read-JsonUtf8 (Join-Path $batchPath 'capture.json')
    $batch.status='recovered';$batch.error=$null;$batch.finished_utc=$state.finished_at;$batch.updated_utc=$state.finished_at
    $batch|Add-Member NoteProperty recovery $state.recovery -Force
    Write-JsonUtf8 (Join-Path $batchPath 'capture.json') $batch
    foreach($indexName in @('last-full-run.json','last-run.json')){
        $index=Join-Path $root ('state/super-captures/'+$indexName)
        if((Test-Path -LiteralPath $index) -and (Read-JsonUtf8 $index).session -eq $Session){
            Write-JsonUtf8 $index @{session=$Session;batch=$plan.super_capture.batch;status='recovered';updated_utc=$state.finished_at;error=$null}
        }
    }
    $script:recoveryProgress.status='recovered';Save-RecoveryProgress
    Write-Host 'Full 离线收尾已恢复并通过校验；原启动器退出码保持未知，没有重新采集。'
}catch{
    $script:recoveryProgress.status='interrupted';$script:recoveryProgress.error=$_.Exception.Message
    Save-RecoveryProgress;Write-Host $_.Exception.Message -ForegroundColor Red;exit 1
}finally{$lock.Dispose()}
