$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest
$taskRoot=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
. (Join-Path $taskRoot 'scripts/v3_profiles.ps1')
$taskCatalog=Get-V3ExperimentCatalog
$taskBaseline=Get-V3CandidateOverrides
if($taskCatalog.Count -ne 19 -or @($taskBaseline.Values|Where-Object {$_ -ne 0}).Count){throw 'Catalog/default coverage mismatch.'}
$taskParser=[IO.File]::ReadAllText((Join-Path $taskRoot 'src/core/perf_sampler.cpp'))
foreach($taskName in $taskCatalog.Keys){
    $taskSelected=Get-V3CandidateOverrides @($taskName)
    foreach($taskKey in $taskBaseline.Keys){
        if(-not $taskParser.Contains('key == "'+$taskKey+'"')){throw ('No native parser for '+$taskKey)}
        $taskExpected=if($taskCatalog[$taskName].ContainsKey($taskKey)){$taskCatalog[$taskName][$taskKey]}else{0}
        if($taskSelected[$taskKey] -ne $taskExpected){throw ('Independent selection leaked: '+$taskName+'/'+$taskKey)}
    }
}
foreach($taskWorkers in @(2,4)){
    $taskCombined=@(Get-V3CombinedCandidates $taskWorkers)
    $taskOverrides=Get-V3CandidateOverrides $taskCombined
    if($taskOverrides.v3_recording_workers -ne $taskWorkers -or $taskOverrides.v3_disable_params_lookup -or $taskOverrides.v3_disable_flatbuf_reuse){throw 'Combined experiment changed baseline ablations or worker count.'}
    if($taskCombined.Count -ne 16){throw 'Combined candidate coverage mismatch.'}
}
foreach($taskBad in @(@('recording-2','recording-4'),@('lighting-vector','lighting-vector'),@('bogus'))){
    $taskRejected=$false
    try{$null=Get-V3CandidateOverrides $taskBad}catch{$taskRejected=$true}
    if(-not $taskRejected){throw 'Invalid candidate selection was accepted.'}
}
Write-Host '19 independent profiles, both 16-feature combinations, default isolation and native keys verified.'
