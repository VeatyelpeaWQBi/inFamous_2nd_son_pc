# SPDX-License-Identifier: GPL-2.0-or-later
# Import defines functions only. No query, process, game, or file is opened on import.
function New-SuperEnvironment([string]$Directory) {
    if(-not ('IfssSuperEnvironment' -as [type])){
        # Keep compilation artifacts in the existing project-owned helper directory.
        $temporary=Join-Path $PSScriptRoot '../state/runtime/native-helper-temp'
        $null=[IO.Directory]::CreateDirectory($temporary)
        $priorTemp=$env:TEMP;$priorTmp=$env:TMP
        try{
            $env:TEMP=$temporary;$env:TMP=$temporary
            Add-Type -Path (Join-Path $PSScriptRoot 'super_environment.cs')
        }finally{$env:TEMP=$priorTemp;$env:TMP=$priorTmp}
    }
    $observer=New-Object IfssSuperEnvironment
    try{
        $raw=Join-Path $Directory 'environment-samples.jsonl'
        $writer=[IO.StreamWriter]::new($raw,$false,[Text.UTF8Encoding]::new($false))
        return [pscustomobject]@{Observer=$observer;Writer=$writer;Path=$raw
            Clock=[Diagnostics.Stopwatch]::StartNew();Next=2.0;Count=0;Complete=$false
            NextProcessPoll=0.0
            Policy=@{nongame_cpu_core_equiv=1.0;unrelated_gpu_engine_percent=10.0
                disk_bytes_per_second=20MB;disk_average_queue=1.0;preflight_idle_seconds=60}
            Recent=[Collections.Generic.List[object]]::new();SuspectMeasurements=0;MeasurementSamples=0
            Phase='preflight';TargetPid=0;TargetStartedUtc=$null
            Last=$null;Metadata=(Join-Path $Directory 'environment.json')}
    }catch{$observer.Dispose();throw}
}
function Read-SuperEnvironment($Owner,[switch]$Force) {
    if(-not $Owner -or $Owner.Complete){return $null}
    if($Owner.Clock.Elapsed.TotalSeconds -ge $Owner.NextProcessPoll){
        $Owner.Observer.PollProcesses();$Owner.NextProcessPoll=$Owner.Clock.Elapsed.TotalSeconds+0.25
    }
    if(-not $Force -and $Owner.Clock.Elapsed.TotalSeconds -lt $Owner.Next){return $null}
    if($Owner.Count -ge 2048 -or $Owner.Writer.BaseStream.Position -ge 2MB){throw 'Environment observation exceeded its bounded sample budget.'}
    $point=$Owner.Observer.Read()
    $point['sequence']=$Owner.Count
    $point['phase']=$Owner.Phase
    $Owner.Count++;$Owner.Next=$Owner.Clock.Elapsed.TotalSeconds+2.0
    $Owner.Last=$point
    $Owner.Recent.Add($point)
    if($Owner.Recent.Count -gt 3){$Owner.Recent.RemoveAt(0)}
    if($Owner.Phase -eq 'measurement'){
        $Owner.MeasurementSamples++
        if(-not (Test-SuperEnvironmentQuiet $point $Owner.Policy)){$Owner.SuspectMeasurements++}
    }
    $Owner.Writer.WriteLine((ConvertTo-Json -InputObject $point -Compress -Depth 8))
    $Owner.Writer.Flush()
    return $point
}
function Test-SuperEnvironmentQuiet($Point,$Policy,[switch]$Preflight) {
    return $null -ne $Point -and $Point.cpu_available -and $Point.gpu.available -and $Point.gpu.classifier_available -and
        $Point.disk_available -and $Point.nongame_core_equiv -le $Policy.nongame_cpu_core_equiv -and
        $Point.gpu.unrelated_max_engine_percent -le $Policy.unrelated_gpu_engine_percent -and
        $Point.disk_bytes_per_second -le $Policy.disk_bytes_per_second -and
        $Point.disk_average_queue -le $Policy.disk_average_queue -and
        $Point.read_wall_ms -le 200 -and $Point.monitor_cpu_ms -le 40 -and
        (-not $Preflight -or ($null -ne $Point.last_input_idle_seconds -and
                             $Point.last_input_idle_seconds -ge $Policy.preflight_idle_seconds))
}
function Wait-SuperEnvironmentQuiet($Owner,[int]$TimeoutSeconds=30,[bool]$Preflight=$true) {
    $wait=[Diagnostics.Stopwatch]::StartNew();$streak=0
    while($wait.Elapsed.TotalSeconds -lt $TimeoutSeconds){
        Start-Sleep -Milliseconds 200
        $point=Read-SuperEnvironment $Owner
        if($null -eq $point){continue}
        $streak=if(Test-SuperEnvironmentQuiet $point $Owner.Policy -Preflight:$Preflight){$streak+1}else{0}
        if($streak -ge 3){return}
    }
    $Owner | Add-Member NoteProperty PreflightQuiet $false -Force
    Write-Host 'External load is high or observation is unavailable; launching anyway. Environment samples are kept for diagnosis and do not hard-stop the run.'
}
function Assert-SuperEnvironmentBeforeRelease($Owner) {
    $null=Read-SuperEnvironment $Owner
    if($Owner.Recent.Count -ne 3 -or @($Owner.Recent|Where-Object {-not(Test-SuperEnvironmentQuiet $_ $Owner.Policy)}).Count -or
       $Owner.Clock.Elapsed.TotalSeconds -ge $Owner.Next+2){
        $Owner | Add-Member NoteProperty ReleaseEnvironmentQualified $false -Force
        Write-Host 'External load is high or observation is unavailable before release; sending the release anyway. Recorded for diagnosis, not a hard stop.'
    }
}
function Close-SuperEnvironment($Owner,[bool]$Complete) {
    if(-not $Owner -or $Owner.Complete){return}
    try{
        $Owner.Writer.Dispose()
        Write-JsonUtf8 $Owner.Metadata @{
            schema=2;complete=$Complete;sample_count=$Owner.Count;sample_period_seconds=2
            qpc_frequency=[Diagnostics.Stopwatch]::Frequency;policy=$Owner.Policy
            initialization=$Owner.Observer.Initialization;raw_file='environment-samples.jsonl'
            target_pid=$Owner.TargetPid;target_started_utc=$Owner.TargetStartedUtc
            measurement_qualified=($Owner.MeasurementSamples -gt 0 -and $Owner.SuspectMeasurements -eq 0)
            measurement_samples=$Owner.MeasurementSamples
            suspect_measurement_samples=$Owner.SuspectMeasurements
            limit_samples=2048;limit_bytes=2MB
            limitation='CPU core equivalents are time accounting, not P-core capacity; includes compositor and system work. GPU engines are grouped by hardware identity, never summed across engines. Current-session dwm is identified by OS process name/session and reported separately, since game presentation itself induces compositor work. All adapters are observed. Disk values are global pressure, not process attribution. No proof of absence of thermal/power/clock changes. Read-only idle time contains no input content.'
        }
    }finally{$Owner.Complete=$true;$Owner.Observer.Dispose()}
}
