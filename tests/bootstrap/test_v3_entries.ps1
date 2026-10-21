# SPDX-License-Identifier: GPL-2.0-or-later
# Explicit offline entry audit: creates isolated prepared sessions, never starts the game.
$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest
$taskRoot=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
. (Join-Path $taskRoot 'scripts/tools.ps1')
. (Join-Path $taskRoot 'scripts/v3_profiles.ps1')
$taskShell=Join-Path $env:WINDIR 'System32/WindowsPowerShell/v1.0/powershell.exe'
$taskProject=Read-JsonUtf8 (Join-Path $taskRoot 'project.json')
$taskCases=@(@{case='baseline';mode='pro4k';kind='normal'},@{case='control';mode='pro4k';kind='v3'})
foreach($taskName in @((Get-V3ExperimentCatalog).Keys)+@('combined-2','combined-4')){
    $taskCases+=@{case=$taskName;mode='pro4k';kind='v3'}
}
$taskCases+=@(@{case='auxiliary';mode='pro4k';kind='timing'},
    @{case='control';mode='1080p';kind='v3'},@{case='combined-2';mode='1080p';kind='v3'},
    @{case='control';mode='pro4k';kind='full'},@{case='combined-2';mode='pro4k';kind='full'},
    @{case='combined-4';mode='pro4k';kind='full'})
$taskResults=@()
foreach($taskCase in $taskCases){
    $taskArguments=@('-NoLogo','-NoProfile','-ExecutionPolicy','Bypass','-File',(Join-Path $taskRoot 'scripts/launch.ps1'),'-PrepareOnly','-NoMenu','-Mode',$taskCase.mode)
    $taskChosen=@()
    if($taskCase.kind -in @('v3','full')){
        if($taskCase.case -eq 'control'){$taskArguments+=@('-OptimizationV3','control')}
        else{
            $taskChosen=@(if($taskCase.case -eq 'combined-2'){Get-V3CombinedCandidates 2}elseif($taskCase.case -eq 'combined-4'){Get-V3CombinedCandidates 4}else{$taskCase.case})
            $taskArguments+=@('-OptimizationV3','custom','-V3CandidateList',($taskChosen -join ','))
        }
        if($taskCase.kind -eq 'full'){$taskArguments+=@('-Diagnostic','full')}
    }elseif($taskCase.kind -eq 'timing'){$taskArguments+=@('-TimingProfile','auxiliary')}
    $taskLines=@(& $taskShell @taskArguments 2>&1)
    if($LASTEXITCODE -ne 0){throw ('Prepare failed: '+$taskCase.case+': '+($taskLines -join "`n"))}
    $taskPrepared=@($taskLines|Where-Object {$_ -like 'Prepared session: *'})
    if($taskPrepared.Count -ne 1){throw 'Prepared session identity missing.'}
    $taskSession=$taskPrepared[0].Substring('Prepared session: '.Length)
    Assert-ProjectPath $taskRoot $taskSession
    $taskLaunch=Read-JsonUtf8 (Join-Path $taskSession 'launch.json')
    if($taskLaunch.game_launched -or $taskLaunch.complete -or $taskLaunch.runtime_sha256 -ne $taskProject.runtime_sha256){throw 'Prepared entry launched a game or changed runtime.'}
    if(@($taskLaunch.v3_candidates).Count -ne $taskChosen.Count -or ($taskLaunch.v3_candidates -join ',') -ne ($taskChosen -join ',')){throw 'Candidate array changed in the launcher.'}
    $taskExpected=Get-V3CandidateOverrides $taskChosen
    foreach($taskIni in @('perf_tuning.ini','user/perf_tuning.ini')){
        $taskText=[IO.File]::ReadAllText((Join-Path $taskSession $taskIni))
        foreach($taskKey in $taskExpected.Keys){
            $taskMatches=[regex]::Matches($taskText,('(?m)^'+[regex]::Escape($taskKey)+'\s*=\s*(.*?)\r?$'))
            if($taskMatches.Count -ne 1 -or [int]$taskMatches[0].Groups[1].Value -ne $taskExpected[$taskKey]){throw ('Effective ini mismatch: '+$taskCase.case+'/'+$taskKey)}
        }
        if($taskCase.kind -eq 'timing'){
            foreach($taskPair in @(@('auxiliary_evidence_limit',16),@('consumer_evidence_limit',0),@('consumer_trace_only',1),@('path_trace',1),@('gpu_profile',0),@('perf_log',0))){
                if($taskText -notmatch ('(?m)^'+$taskPair[0]+'\s*=\s*'+$taskPair[1]+'\r?$')){throw ('Auxiliary profile mismatch: '+$taskPair[0])}
            }
        }elseif($taskText -notmatch '(?m)^auxiliary_evidence_limit\s*=\s*0\r?$'){throw 'Auxiliary evidence leaked into another profile.'}
    }
    foreach($taskConfigName in @('user/config.json','user/custom_configs/CUSA00309.json','custom_configs/CUSA00309.json')){
        $taskConfig=Read-JsonUtf8 (Join-Path $taskSession $taskConfigName)
        if($taskCase.kind -ne 'normal' -and ($taskConfig.General.volume_slider -ne 0 -or -not $taskLaunch.audio_muted)){throw 'A performance entry is not muted.'}
        if($taskConfigName -ne 'user/config.json' -and ($taskConfig.GPU.readbacks_mode -ne 2 -or $taskConfig.GPU.hdr_allowed)){throw 'Precise readback/SDR baseline changed.'}
    }
    $taskResults+=@{case=$taskCase.case;mode=$taskCase.mode;kind=$taskCase.kind;session=(Get-RelativePath $taskRoot $taskSession);passed=$true;game_started=$false}
    Write-Host ($taskCase.case+'/'+$taskCase.kind+'/'+$taskCase.mode+': prepared and verified')
}
Write-JsonUtf8 (Join-Path $taskRoot 'state/runtime/r15-entry-audit.json') @{schema=1;runtime_sha256=$taskProject.runtime_sha256;complete=$true;game_started=$false;entries=$taskResults}
Write-Host ('Offline entry audit passed: '+$taskResults.Count+' prepared sessions; no game started.')
