# SPDX-License-Identifier: GPL-2.0-or-later
[CmdletBinding()]
param([string]$Configuration='diagnostics/v3-suite.json',[switch]$Run,[switch]$SkipBuild)
Set-StrictMode -Version Latest
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot 'tools.ps1')
. (Join-Path $PSScriptRoot 'diagnostic_tools.ps1')
. (Join-Path $PSScriptRoot 'v3_profiles.ps1')
. (Join-Path $PSScriptRoot 'v3_matrix_policy.ps1')
$taskRoot=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$taskConfig=Read-JsonUtf8 (Resolve-ProjectChild $taskRoot $Configuration)
if($taskConfig.schema -ne 1 -or -not $taskConfig.modes.Count -or @($taskConfig.modes|Where-Object {$_ -notin @('1080p','pro4k')}).Count){throw 'Invalid suite configuration.'}
foreach($taskSetting in @('warmup_seconds','measure_seconds','short_warmup_seconds','short_measure_seconds','maximum_group_attempts','confirmation_groups','matrix_timeout_seconds','full_timeout_seconds')){
    if([int]$taskConfig.$taskSetting -le 0){throw ('Invalid suite setting: '+$taskSetting)}
}
if($taskConfig.confirmation_groups -lt 3 -or $taskConfig.maximum_group_attempts -gt 3){throw 'Confirmation requires at least three groups; retries must be bounded.'}
if(-not $Run){Write-Host 'No build or game started. Launch test-v3-all.bat when you want the unattended suite to run.';exit 0}
$taskShell=Join-Path $env:WINDIR 'System32/WindowsPowerShell/v1.0/powershell.exe'
if($taskConfig.include_full -and -not(Test-DiagnosticAdministrator)){
    $taskArguments=@('-NoLogo','-NoProfile','-ExecutionPolicy','Bypass','-File',$PSCommandPath,'-Configuration',$Configuration,'-Run')
    if($SkipBuild){$taskArguments+='-SkipBuild'}
    Write-Host 'Windows administrator approval is required once for the configured full collectors.'
    $taskElevated=Start-Process -FilePath $taskShell -ArgumentList (($taskArguments|ForEach-Object {Quote-WindowsArgument $_}) -join ' ') -WorkingDirectory $taskRoot -WindowStyle Hidden -Verb RunAs -Wait -PassThru
    try{$taskElevated.Refresh();if($null -ne $taskElevated.ExitCode){exit $taskElevated.ExitCode};exit 1}finally{$taskElevated.Dispose()}
}
$taskId=(Get-Date).ToString('yyyyMMdd-HHmmss')+'-'+[Guid]::NewGuid().ToString('N').Substring(0,8)
$taskDirectory=Resolve-ProjectChild $taskRoot ('state/runtime/v3-suite-'+$taskId)
$null=[IO.Directory]::CreateDirectory($taskDirectory)
$taskStatePath=Join-Path $taskDirectory 'suite.json'
$taskStop=Join-Path $taskRoot 'state/runtime/v3-suite.stop'
$taskPython=$null
$taskState=[ordered]@{schema=1;id=$taskId;configuration=$taskConfig;configuration_path=$Configuration;started_utc=[DateTime]::UtcNow.ToString('o')
    finished_utc=$null;status='preparing';runtime_sha256=$null;source_sha256=$null;tasks=@();failure=$null;exit_code=1
    transcript=(Get-RelativePath $taskRoot (Join-Path $taskDirectory 'console.log'));report=(Get-RelativePath $taskRoot (Join-Path $taskDirectory 'report.log'))
    ai_calls=0;whole_game_acceptance=$false;launches_automatically_muted=$true}
$taskState.planned_scope=@{modes=@($taskConfig.modes);baseline_groups_per_mode=1
    correctness_cases=@('recording-2','recording-4','copy-coalesce','completion-elision')
    screening_cases=@((Get-V3ExperimentCatalog).Keys)+@('combined-2','combined-4')
    confirmation_groups_for_numerical_candidates=$taskConfig.confirmation_groups
    full_cases=if($taskConfig.include_full){@($taskConfig.full_cases)}else{@()}
    auxiliary_requested=[bool]$taskConfig.include_auxiliary_evidence}
$taskLock=$null;$taskTranscript=$false
function Save-V3Suite {
    Write-JsonUtf8 $taskStatePath $taskState
    Write-JsonUtf8 (Join-Path $taskRoot 'state/runtime/v3-suite-last.json') @{schema=1;id=$taskId;status=$taskState.status;exit_code=$taskState.exit_code
        state=(Get-RelativePath $taskRoot $taskStatePath);report=$taskState.report;transcript=$taskState.transcript}
}
function Get-V3TreeBytes([string]$Path){
    if(-not(Test-Path -LiteralPath $Path)){return 0L}
    $taskStack=[Collections.Generic.Stack[IO.DirectoryInfo]]::new();$taskStack.Push([IO.DirectoryInfo]::new($Path));$taskBytes=0L
    while($taskStack.Count){foreach($taskEntry in $taskStack.Pop().EnumerateFileSystemInfos()){
        if($taskEntry.Attributes -band [IO.FileAttributes]::ReparsePoint){continue}
        if($taskEntry -is [IO.DirectoryInfo]){$taskStack.Push($taskEntry)}else{$taskBytes+=$taskEntry.Length}
    }}
    return $taskBytes
}
function Assert-V3Resources {
    if(Test-Path -LiteralPath $taskStop){throw 'Local stop request received.'}
    if($taskState.Contains('frozen_files')){Assert-MatrixFrozenFiles $taskRoot $taskState.frozen_files}
    if([IO.DriveInfo]::new([IO.Path]::GetPathRoot($taskRoot)).AvailableFreeSpace -lt ([long]$taskConfig.minimum_free_gib*1GB)){throw 'Low free disk space; suite stopped.'}
    $taskNow=Get-V3TreeBytes (Join-Path $taskRoot 'state')
    if($taskNow-$script:V3InitialStateBytes -ge ([long]$taskConfig.total_additional_gib*1GB)){throw 'Suite-wide additional state/cache budget reached; no next run started.'}
}
function Stop-V3OwnedGame([string[]]$Sessions){
    foreach($taskOwnedSession in @($Sessions|Where-Object {$_}|Select-Object -Unique)){
        try{
            $taskOwned=Read-ProjectSession $taskRoot $taskOwnedSession
            $taskGame=Get-DiagnosticTarget $taskOwned.Launch $taskRoot
            if(-not $taskGame){continue}
            try{
                if($taskOwned.Launch.diagnostic){
                    try{Send-DiagnosticControl $taskRoot $taskOwnedSession 'stop' | Out-Null}catch{Write-Warning ('Owned collector stop request: '+$_.Exception.Message)}
                }
                & (Join-Path $PSScriptRoot 'control.ps1') -Action quit -Session $taskOwnedSession | Out-Null
                if(-not $taskGame.WaitForExit(30000)){
                    if(Test-ProjectProcessIdentity $taskGame (Resolve-ProjectChild $taskRoot $taskOwned.Launch.runtime) $taskOwned.Launch.process_started){
                        Write-Warning ('Terminating only the hung isolated test process: '+$taskOwnedSession)
                        $taskGame.Kill();$null=$taskGame.WaitForExit(10000)
                    }
                }
            }finally{$taskGame.Dispose()}
        }catch{Write-Warning ('Owned session cleanup: '+$_.Exception.Message)}
    }
}
function Invoke-V3Child([string[]]$Arguments,[string]$Prefix,[int]$TimeoutSeconds,[string]$MatrixPlan='', [string]$LaunchResult=''){
    $taskStdout=Join-Path $taskDirectory ($Prefix+'.log');$taskStderr=Join-Path $taskDirectory ($Prefix+'.stderr.log')
    $taskChild=$null;$taskFailure=$null;$taskExit=$null;$taskStarted=[DateTime]::UtcNow;$taskCleanupPending=$false
    try{
        $taskChild=Start-Process -FilePath $taskShell -ArgumentList (($Arguments|ForEach-Object {Quote-WindowsArgument $_}) -join ' ') -WorkingDirectory $taskRoot -WindowStyle Hidden -PassThru -RedirectStandardOutput $taskStdout -RedirectStandardError $taskStderr
        $null=$taskChild.Handle
        while(-not $taskChild.WaitForExit(1000)){
            if(Test-Path -LiteralPath $taskStop){throw 'Local stop request received.'}
            if([DateTime]::UtcNow-$taskStarted -gt [TimeSpan]::FromSeconds($TimeoutSeconds)){throw 'Owned task timeout exceeded.'}
            if([IO.DriveInfo]::new([IO.Path]::GetPathRoot($taskRoot)).AvailableFreeSpace -lt ([long]$taskConfig.minimum_free_gib*1GB)){throw 'Low free space during owned task.'}
        }
        $taskChild.Refresh();$taskExit=$taskChild.ExitCode
        if($taskExit -ne 0){$taskFailure='Task returned nonzero or unavailable exit code: '+$taskExit}
    }catch{$taskFailure=$_.Exception.Message}
    finally{
        if($taskChild -and -not $taskChild.HasExited -and -not $taskFailure){$taskFailure='Owned helper interrupted before completion.'}
        $taskSessions=@()
        if($MatrixPlan -and (Test-Path -LiteralPath (Resolve-ProjectChild $taskRoot $MatrixPlan))){
            $taskMatrix=Read-JsonUtf8 (Resolve-ProjectChild $taskRoot $MatrixPlan);$taskSessions+=@($taskMatrix.owned_sessions)
        }
        if($LaunchResult -and (Test-Path -LiteralPath (Resolve-ProjectChild $taskRoot $LaunchResult))){
            $taskLaunch=Read-JsonUtf8 (Resolve-ProjectChild $taskRoot $LaunchResult);$taskSessions+=@($taskLaunch.session)
        }
        if($taskFailure){Stop-V3OwnedGame $taskSessions}
        if($taskChild){
            if($taskFailure -and $LaunchResult -and -not $taskChild.HasExited){
                Write-Host 'Waiting for this owned diagnostic task to finalize its local exports.'
                if(-not $taskChild.WaitForExit(60000)){$taskCleanupPending=$true}
            }
            if(-not $taskChild.HasExited){$taskChild.Kill();$null=$taskChild.WaitForExit(10000)}
            $taskChild.Dispose()
        }
    }
    return @{status=if($taskFailure){'failed'}else{'collected'};exit_code=$taskExit;failure=$taskFailure
        stdout=(Get-RelativePath $taskRoot $taskStdout);stderr=(Get-RelativePath $taskRoot $taskStderr)
        started_utc=$taskStarted.ToString('o');finished_utc=[DateTime]::UtcNow.ToString('o');sessions=$taskSessions;cleanup_pending=$taskCleanupPending}
}
function Update-V3Report {
    Save-V3Suite
    & $taskPython -B (Join-Path $PSScriptRoot 'summarize_v3_suite.py') --suite (Get-RelativePath $taskRoot $taskStatePath) | ForEach-Object {Write-Host $_}
    if($LASTEXITCODE -ne 0){throw 'Suite report generation failed; raw task logs remain available.'}
}
function Invoke-V3MatrixTask([string]$Phase,[string]$Mode,[string]$Case,[int]$Repeat,[int]$Warmup,[int]$Measure){
    Assert-V3Resources
    $taskItem=@{index=$taskState.tasks.Count;phase=$Phase;mode=$Mode;case=$Case;repetition=$Repeat;status='running';attempts=@()}
    $taskState.tasks+=,$taskItem
    $taskPlan='state/runtime/v3-matrix-suite-'+$taskId+'-'+$taskItem.index+'.json'
    for($taskTry=1;$taskTry -le $taskConfig.maximum_group_attempts;$taskTry++){
        Write-Host ('Running '+$Phase+' '+$Mode+' '+$Case+' repeat '+$Repeat+' attempt '+$taskTry)
        $taskArgs=@('-NoLogo','-NoProfile','-ExecutionPolicy','Bypass','-File',(Join-Path $PSScriptRoot 'test-v3-matrix.ps1'),'-Run')
        if($taskTry -gt 1 -and (Test-Path -LiteralPath (Resolve-ProjectChild $taskRoot $taskPlan))){$taskArgs+=@('-ResumePlan',$taskPlan)}else{
            $taskArgs+=@('-PlanPath',$taskPlan,'-Mode',$Mode,'-Cases',$Case,'-WarmupSeconds',$Warmup,'-MeasureSeconds',$Measure,
                '-MaxAdditionalGiB',$taskConfig.matrix_additional_gib,'-MaximumImageDistance',$taskConfig.maximum_image_distance)
        }
        $taskAttempt=Invoke-V3Child $taskArgs ('task-'+$taskItem.index+'-try'+$taskTry) $taskConfig.matrix_timeout_seconds -MatrixPlan $taskPlan
        $taskAttempt.number=$taskTry;$taskAttempt.plan=$taskPlan
        if(Test-Path -LiteralPath (Resolve-ProjectChild $taskRoot $taskPlan)){
            $taskPlanSnapshot=Join-Path $taskDirectory ('task-'+$taskItem.index+'-try'+$taskTry+'-plan.json')
            if(Test-Path -LiteralPath $taskPlanSnapshot){throw 'An existing attempt plan snapshot cannot be replaced.'}
            Write-JsonUtf8 $taskPlanSnapshot (Read-JsonUtf8 (Resolve-ProjectChild $taskRoot $taskPlan))
            $taskAttempt.plan_snapshot=Get-RelativePath $taskRoot $taskPlanSnapshot
        }
        $taskSummary=$taskPlan.Substring(0,$taskPlan.Length-5)+'-summary.json'
        if(Test-Path -LiteralPath (Resolve-ProjectChild $taskRoot $taskSummary)){
            $taskSummarySnapshot=Join-Path $taskDirectory ('task-'+$taskItem.index+'-try'+$taskTry+'-summary.json')
            if(Test-Path -LiteralPath $taskSummarySnapshot){throw 'An existing attempt summary snapshot cannot be replaced.'}
            Write-JsonUtf8 $taskSummarySnapshot (Read-JsonUtf8 (Resolve-ProjectChild $taskRoot $taskSummary))
            $taskAttempt.summary=Get-RelativePath $taskRoot $taskSummarySnapshot
        }
        $taskItem.attempts+=,$taskAttempt
        $taskItem.status=$taskAttempt.status
        Update-V3Report
        if($taskAttempt.status -eq 'collected'){break}
        Assert-V3Resources
    }
    return $taskItem
}
function Invoke-V3EvidenceTask([string]$Phase,[string]$Mode,[string]$Case){
    Assert-V3Resources
    $taskItem=@{index=$taskState.tasks.Count;phase=$Phase;mode=$Mode;case=$Case;repetition=1;status='running';attempts=@()}
    $taskState.tasks+=,$taskItem
    $taskResult=Get-RelativePath $taskRoot (Join-Path $taskDirectory ('task-'+$taskItem.index+'-launch.json'))
    $taskScript=if($Phase -eq 'full'){'v3-test.ps1'}else{'collect-v3-auxiliary.ps1'}
    $taskArgs=@('-NoLogo','-NoProfile','-ExecutionPolicy','Bypass','-File',(Join-Path $PSScriptRoot $taskScript),'-Mode',$Mode,'-ResultPath',$taskResult)
    if($Phase -eq 'full'){$taskArgs+=@('-Case',$Case,'-Diagnostic','full')}
    Write-Host ('Running '+$Phase+' evidence '+$Mode+' '+$Case)
    $taskAttempt=Invoke-V3Child $taskArgs ('task-'+$taskItem.index) $taskConfig.full_timeout_seconds -LaunchResult $taskResult
    $taskAttempt.number=1;$taskAttempt.launch_result=$taskResult
    if(Test-Path -LiteralPath (Resolve-ProjectChild $taskRoot $taskResult)){
        $taskLaunch=Read-JsonUtf8 (Resolve-ProjectChild $taskRoot $taskResult)
        if($taskLaunch.session){
            $taskEvidence=if($Phase -eq 'full'){$taskLaunch.session+'/diagnostics/coverage.json'}else{$taskLaunch.session+'/auxiliary-summary.json'}
            if(Test-Path -LiteralPath (Resolve-ProjectChild $taskRoot $taskEvidence)){$taskAttempt.summary=$taskEvidence}
        }
    }
    $taskItem.attempts+=,$taskAttempt;$taskItem.status=$taskAttempt.status;Update-V3Report
    if($taskAttempt.cleanup_pending){throw 'Owned collector export cleanup is still pending; no next game will be started.'}
}
try{
    $taskLock=[IO.File]::Open((Join-Path $taskRoot 'state/runtime/v3-suite.lock'),[IO.FileMode]::OpenOrCreate,[IO.FileAccess]::ReadWrite,[IO.FileShare]::None)
    if(Test-Path -LiteralPath $taskStop){Remove-Item -LiteralPath $taskStop}
    $script:V3InitialStateBytes=Get-V3TreeBytes (Join-Path $taskRoot 'state')
    Start-Transcript -LiteralPath (Join-Path $taskDirectory 'console.log') | Out-Null;$taskTranscript=$true
    Save-V3Suite
    $taskPython=Resolve-InstalledPython
    if($taskConfig.include_full){Assert-DiagnosticTools (Get-DiagnosticSpec 'full') (Get-DiagnosticTools)}
    $taskNvidia=Find-DiagnosticExecutable 'nvidia-smi.exe' @((Join-Path $env:WINDIR 'System32/nvidia-smi.exe'))
    if(-not $taskNvidia){throw 'The existing NVIDIA sensor tool is unavailable; nothing was installed.'}
    $taskGpuInfo=@(& $taskNvidia '--query-gpu=index,name,uuid,driver_version,memory.total' '--format=csv,nounits')
    if($LASTEXITCODE -ne 0){throw 'Existing GPU/driver inventory query failed.'}
    $taskState.gpu_inventory=$taskGpuInfo;Save-V3Suite
    $taskBase=@('-NoLogo','-NoProfile','-ExecutionPolicy','Bypass','-File')
    if(-not $SkipBuild){
        $taskBuild=Invoke-V3Child ($taskBase+@((Join-Path $PSScriptRoot 'build.ps1'))) 'build' 7200
        $taskState.build=$taskBuild;Save-V3Suite
        if($taskBuild.status -ne 'collected'){throw 'Build failed; no game was started.'}
        $taskBuildText=[IO.File]::ReadAllText((Join-Path $taskRoot 'Build/RelWithDebInfo/build.log'))
        if($taskBuildText -match '(?im)CMake Warning|\b(?:warning|error)\s*(?:[A-Z]+\d+)?:'){throw 'Build contains warnings/errors; no game was started.'}
    }
    foreach($taskCheck in @('scripts/test.ps1','tests/bootstrap/test_v3_matrix_gates.ps1','scripts/test-v3-native.ps1')){
        $taskArgs=$taskBase+@((Join-Path $taskRoot $taskCheck));if($taskCheck -eq 'scripts/test.ps1'){$taskArgs+='-RuntimeOnly'}
        $taskCheckRun=Invoke-V3Child $taskArgs ('check-'+[IO.Path]::GetFileNameWithoutExtension($taskCheck)) 1800
        if($taskCheckRun.status -ne 'collected'){throw ('Offline/native preparation failed: '+$taskCheck)}
    }
    & $taskPython -B -m unittest discover -s (Join-Path $taskRoot 'tests') -p 'test_*analysis.py'
    if($LASTEXITCODE -ne 0){throw 'Offline analysis regressions failed; no game was started.'}
    $taskManifest=Read-JsonUtf8 (Join-Path $taskRoot 'project.json')
    $taskReceipt=Read-JsonUtf8 (Join-Path $taskRoot 'Build/RelWithDebInfo/build-receipt.json')
    $taskState.runtime_sha256=$taskManifest.runtime_sha256;$taskState.source_sha256=$taskReceipt.source_sha256
    $taskSuiteInputs=@($Configuration,'project.json','Build/RelWithDebInfo/build-receipt.json',
        'scripts/run-v3-suite.ps1','scripts/test-v3-matrix.ps1','scripts/v3_matrix_policy.ps1','scripts/v3_profiles.ps1',
        'scripts/summarize_v3_suite.py','scripts/analyze_v3_matrix.py','scripts/analyze_ordinary.py','scripts/metrics.py',
        'scripts/automatic_scene.ps1','scripts/ordinary_scene.ps1','scripts/observe-ordinary.ps1','scripts/launch.ps1',
        'scripts/collect-v3-auxiliary.ps1','scripts/v3-test.ps1','scripts/diagnostic_automation.ps1','scripts/diagnostic_tools.ps1',
        'state/play/launcher.json','state/play/user/config.json','state/play/user/custom_configs/CUSA00309.json','state/play/user/perf_tuning.ini',
        'profiles/1080p.json','profiles/pro4k.json','profiles/CUSA00309.xml')
    $taskState.frozen_files=Get-MatrixFrozenFiles $taskRoot $taskSuiteInputs
    $taskState.status='running';Save-V3Suite
    foreach($taskMode in $taskConfig.modes){
        $taskPilot=Invoke-V3MatrixTask 'baseline' $taskMode 'baseline-pilot' 1 $taskConfig.warmup_seconds $taskConfig.measure_seconds
        if($taskPilot.status -ne 'collected'){throw 'Repeated all-off baseline did not stabilize; ordinary comparisons were stopped.'}
        foreach($taskCase in @('recording-2','recording-4','copy-coalesce','completion-elision')){
            $null=Invoke-V3MatrixTask 'correctness' $taskMode $taskCase 1 $taskConfig.short_warmup_seconds $taskConfig.short_measure_seconds
        }
        foreach($taskCase in @((Get-V3ExperimentCatalog).Keys)+@('combined-2','combined-4')){
            $null=Invoke-V3MatrixTask 'screening' $taskMode $taskCase 1 $taskConfig.warmup_seconds $taskConfig.measure_seconds
        }
    }
    Update-V3Report
    $taskReport=Read-JsonUtf8 (Join-Path $taskDirectory 'summary.json')
    foreach($taskCandidate in @($taskReport.tasks|Where-Object {$_.phase -eq 'screening' -and $_.numerical_status -eq 'numerical-candidate'})){
        for($taskRepeat=1;$taskRepeat -le $taskConfig.confirmation_groups;$taskRepeat++){
            $null=Invoke-V3MatrixTask 'confirmation' $taskCandidate.mode $taskCandidate.case $taskRepeat $taskConfig.warmup_seconds $taskConfig.measure_seconds
        }
    }
    if($taskConfig.include_auxiliary_evidence){Invoke-V3EvidenceTask 'auxiliary' 'pro4k' 'auxiliary'}
    if($taskConfig.include_full){foreach($taskMode in $taskConfig.modes){foreach($taskCase in $taskConfig.full_cases){Invoke-V3EvidenceTask 'full' $taskMode $taskCase}}}
    $taskFailures=@($taskState.tasks|Where-Object {$_.status -ne 'collected'})
    $taskState.status=if($taskFailures.Count){'completed-with-failures'}else{'completed'}
    $taskState.exit_code=if($taskFailures.Count){1}else{0}
}catch{
    $taskState.failure=$_.Exception.Message;$taskState.status=if(Test-Path -LiteralPath $taskStop){'stopped-by-user'}else{'failed'}
    Write-Warning $taskState.failure
}finally{
    $taskState.finished_utc=[DateTime]::UtcNow.ToString('o')
    try{
        if(-not $taskPython){throw 'The existing Windows Python could not be resolved.'}
        Update-V3Report
    }catch{
        Save-V3Suite
        [IO.File]::AppendAllText((Join-Path $taskDirectory 'report.log'),("`r`nSuite status: "+$taskState.status+"`r`nFailure: "+$taskState.failure+"`r`nReport generation: "+$_.Exception.Message+"`r`nState: "+(Get-RelativePath $taskRoot $taskStatePath)),[Text.UTF8Encoding]::new($false))
        Write-Warning ('Summary generation failed; console and per-task logs were retained: '+$_.Exception.Message)
    }
    Write-Host ('Full local report: '+$taskState.report)
    if($taskTranscript){Stop-Transcript | Out-Null}
    if($taskLock){$taskLock.Dispose()}
}
exit $taskState.exit_code
