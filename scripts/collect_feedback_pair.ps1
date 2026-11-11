# SPDX-License-Identifier: GPL-2.0-or-later
[CmdletBinding()]
param(
    [ValidateSet('feedback-checkpoint','feedback-writers','feedback-word','feedback-async','gds-counter','cpu-placement','compute-recorder','recorder-spin','reg-dump-fast','feedback-word-prefetch','feedback-word-bulk','particle-poll-pause','c73-scan','written-sync','image-sync-once','feedback-page-words','access-union','readback-copy-stage','image-read-transition','buffer-prep-union','feedback-arena','vertex-input-key','feedback-arena-group','feedback-arena-pool','feedback-arena-tiled','writer-footprint')][string]$Candidate='feedback-checkpoint',
    [ValidateSet('1080p','1440p','pro4k')][string]$Mode='pro4k',
    [ValidateSet('lightweight','counters')][string]$Profile='lightweight',
    [ValidateSet('AB','BA')][string]$Order='AB',
    [ValidatePattern('^[a-f0-9]{32}$')][string]$PairId,
    [switch]$CriticalCapture,
    [switch]$NormalRendering,
    [switch]$ObserveEnvironment
)
Set-StrictMode -Version Latest
$ErrorActionPreference='Stop'
if($NormalRendering -and ($Candidate -ne 'feedback-arena-pool' -or $CriticalCapture -or $Profile -ne 'lightweight')){
    throw 'Normal rendering comparison requires the compact pool and lightweight capture.'
}
if($PSVersionTable.PSVersion.Major -ne 5){throw 'Use diagnostics/super_feedback.bat with installed Windows PowerShell 5.1.'}
. (Join-Path $PSScriptRoot 'tools.ps1')
. (Join-Path $PSScriptRoot 'super_capture_tools.ps1')
$root=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$area=Join-Path $root 'state/super-captures'
$null=[IO.Directory]::CreateDirectory($area)
$pairSuffix=if($PairId){$PairId}else{[Guid]::NewGuid().ToString('N').Substring(0,8)}
$pairPath=Join-Path $area ((Get-Date -Format 'yyyyMMdd-HHmmss')+'-'+$Candidate+'-pair-'+$pairSuffix+'.json')
$output=@{schema=1;candidate=$Candidate;mode=$Mode;profile=$Profile;status='collecting';error=$null
    capture_order=$Order
    critical_capture=[bool]$CriticalCapture
    normal_rendering=[bool]$NormalRendering
    observe_environment=[bool]$ObserveEnvironment
    sessions=@();release_count=0;automatic_analysis=$false;performance_acceptance=$false
    scene_policy='fresh process and original save each trial; review camera/images and baseline workload'
    pair_file=(Get-RelativePath $root $pairPath);updated_utc=$null}
function Save-FeedbackPair {
    $output.updated_utc=[DateTime]::UtcNow.ToString('o')
    Write-JsonUtf8 $pairPath $output
    Write-JsonUtf8 (Join-Path $area 'last-feedback-pair.json') $output
}
$pairLock=$null
try{
$pairLock=[IO.File]::Open((Join-Path $area 'feedback-pair.lock'),[IO.FileMode]::OpenOrCreate,[IO.FileAccess]::ReadWrite,[IO.FileShare]::None)
Save-FeedbackPair
$performanceCoreMask=''
if($Candidate -eq 'cpu-placement'){
    $topology=(& (Join-Path $PSScriptRoot 'read_cpu_topology.ps1'))|ConvertFrom-Json
    $performanceCoreMask=Get-SuperPerformanceCoreMask $topology
    $output.cpu_topology=$topology
    $output.performance_core_mask_hex=$performanceCoreMask
    Save-FeedbackPair
    Write-Host ('CPU placement: only renderer translation threads use performance-core mask 0x'+$performanceCoreMask+' in B; process/guest affinities and priorities unchanged.')
}
Write-Host ('Two releases total: '+$Order+', separate game processes, original save reloaded, persistent shader cache reused.')
Write-Host 'No Full capture, no extra super warmup, no automatic retries or result analysis.'
$baselines=if($Order -eq 'AB'){@($true,$false)}else{@($false,$true)}
foreach($baseline in $baselines){
    $trialId=[Guid]::NewGuid().ToString('N')
    $arguments=@('-NoLogo','-NoProfile','-ExecutionPolicy','Bypass','-File',
        (Join-Path $PSScriptRoot 'collect_super.ps1'),'-Candidate',$Candidate,
        '-Mode',$Mode,'-Profile',$Profile,'-Rounds','1','-Experiment','none','-TrialId',$trialId)
    if($baseline){$arguments+='-CandidateBaseline'}
    if($CriticalCapture){$arguments+='-CriticalCapture'}
    if($NormalRendering){$arguments+='-NormalRendering'}
    if($ObserveEnvironment){$arguments+='-ObserveEnvironment'}
    if($performanceCoreMask){$arguments+=@('-PerformanceCoreMask',$performanceCoreMask)}
    & (Join-Path $PSHOME 'powershell.exe') @arguments
    if($LASTEXITCODE -ne 0){throw 'An independent trial failed; stopped without adding another release.'}
    $trial=@(Get-ChildItem -LiteralPath $area -Directory -Filter "*-$trialId")
    if($trial.Count -ne 1){throw 'Could not identify the result directory for this trial.'}
    $capture=Read-JsonUtf8 (Join-Path $trial[0].FullName 'capture.json')
    if($capture.status -ne 'collected'){throw 'Trial did not publish a collected result.'}
    if($capture.candidate -ne $Candidate -or [bool]$capture.candidate_baseline -ne $baseline){
        throw 'Trial result does not match the requested candidate state.'
    }
    $output.sessions+=@(@{batch=$capture.batch;session=$capture.session;baseline=$baseline})
    $output.release_count++
    Save-FeedbackPair
    if($ObserveEnvironment -and $capture.comparison_qualified -ne $true){
        Write-Host 'External load or unavailable observation marked this trial unqualified; continuing the pair. This is not evidence against the candidate.'
    }
}
$output.status='collected'
Save-FeedbackPair
Write-Host ('Pair collected. Index: '+$pairPath)
}catch{
    if($pairLock){$output.status='failed';$output.error=$_.Exception.Message;Save-FeedbackPair}
    Write-Error $_
    exit 1
}finally{
    if($pairLock){$pairLock.Dispose()}
}
