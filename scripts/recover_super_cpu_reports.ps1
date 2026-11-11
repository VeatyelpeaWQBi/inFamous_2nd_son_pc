# SPDX-License-Identifier: GPL-2.0-or-later
# Recover an existing CPU result; preserve original failure receipts and all raw samples.
[CmdletBinding()]
param([Parameter(Mandatory)][string]$Session)
Set-StrictMode -Version Latest
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot 'tools.ps1')
. (Join-Path $PSScriptRoot 'diagnostic_tools.ps1')
. (Join-Path $PSScriptRoot 'diagnostic_artifacts.ps1')
$root=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$record=Read-ProjectSession $root $Session
if(-not $record.Launch.complete){throw 'Offline CPU recovery requires the game to have exited.'}
$dir=Join-Path $record.Path 'diagnostics/cpu-hotspots'
$source=Read-JsonUtf8 (Join-Path $dir 'status.json')
if($source.vtune_exit_code -ne 0){throw 'CPU recovery requires verified successful collection and detach.'}
$area=Join-Path $dir 'recovery';$null=[IO.Directory]::CreateDirectory($area)
$lock=[IO.File]::Open((Join-Path $area 'recovery.lock'),[IO.FileMode]::OpenOrCreate,[IO.FileAccess]::ReadWrite,[IO.FileShare]::None)
$state=[ordered]@{schema=1;status='recovering';session=$Session;source_status=$source.status;source_errors=@($source.errors)
    sampling_mode=$source.sampling_mode;target_pid=$source.target_pid;runtime_sha256=$source.runtime_sha256
    performance_acceptance=$false;game_started=$false;collection_started=$false;phases=@();error=$null;raw_unchanged=$null}
function Save-Recovery { $state.updated_utc=[DateTime]::UtcNow.ToString('o');Write-JsonUtf8 (Join-Path $area 'status.json') $state }
$result=Join-Path $dir 'vtune';$user=Join-Path $dir 'vtune-user'
$raw=Join-Path $result 'data.0'
$before=@(Get-ChildItem -LiteralPath $raw -File -Recurse|Sort-Object FullName|Get-FileHash -Algorithm SHA256|Select-Object Path,Hash)
Write-JsonUtf8 (Join-Path $area 'raw-before.json') $before
try{
    Save-Recovery
    $summary=@(Import-Csv -LiteralPath (Join-Path $dir 'summary.csv'))
    $stamp=($summary|Where-Object {$_.'Metric Name' -eq 'Collection start time'}|Select-Object -First 1).'Metric Value'
    $style=[Globalization.DateTimeStyles]::AssumeUniversal -bor [Globalization.DateTimeStyles]::AdjustToUniversal
    $start=[DateTime]::ParseExact($stamp,"HH:mm:ss dd/MM/yyyy 'UTC'",[Globalization.CultureInfo]::InvariantCulture,$style)
    $cycle=@(Read-JsonUtf8 (Join-Path $record.Path 'super-rounds.json'))[0]
    if(-not $cycle.release -or -not $cycle.warm -or -not $cycle.meter_consumed){throw 'The existing release is not confirmed warm and consumed.'}
    $release=[DateTimeOffset]::FromUnixTimeMilliseconds([long][Math]::Floor($cycle.release.applied_unix_ns/1e6)).UtcDateTime
    $tool=(Get-DiagnosticTools).vtune
    foreach($phase in @(@{name='baseline';lo=-8;hi=-1},@{name='front-middle';lo=0;hi=9},@{name='late';lo=9;hi=18})){
        $bounds=Get-DiagnosticCpuTimeRange $start ($release.AddSeconds($phase.lo).ToString('o')) ($release.AddSeconds($phase.hi).ToString('o'))
        $file=Join-Path $area ($phase.name+'-hotspots.csv');$log=Join-Path $area ($phase.name+'-report.log')
        Invoke-DiagnosticNative $tool @('-report','hotspots','-format','csv','-csv-delimiter','comma','-group-by','process,thread,function',
            '-time-filter',$bounds.filter,'-result-dir',$result,'-user-data-dir',$user,'-report-output',$file) $log 120000
        if((Read-DiagnosticSharedText $log) -notmatch 'XML_RNG_LIFETIME_GUARD installed'){throw 'Required compatibility receipt missing.'}
        $rows=@(Import-Csv -LiteralPath $file)
        $target=@($rows|Where-Object {[string]$_.PID -eq [string]$source.target_pid})
        $tids=@($target|Where-Object {$_.Function -eq 'AmdGpu::Liverpool::ProcessGraphics'}|Select-Object -ExpandProperty TID -Unique)
        if($tids.Count -ne 1){throw 'Unique command processor TID not resolved in the recovered report.'}
        $cp=@($target|Where-Object {$_.TID -eq $tids[0]})
        $cpu=0.0
        foreach($row in $cp){$cpu+=[double]::Parse($row.'CPU Time',[Globalization.CultureInfo]::InvariantCulture)}
        if($cpu -le 0 -or $cpu -gt ($bounds.end-$bounds.begin)*1.1+0.25){throw 'Recovered command processor CPU duration is invalid.'}
        $state.phases+=@{name=$phase.name;begin_seconds=$bounds.begin;end_seconds=$bounds.end;cp_tid=$tids[0]
            target_rows=$target.Count;cp_rows=$cp.Count;cp_cpu_seconds=$cpu;export_exit_code=0
            csv=(Get-RelativePath $root $file);sha256=(Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash.ToLowerInvariant()}
        Save-Recovery
    }
    $state.status='recovered'
}catch{
    $state.status='failed';$state.error=$_.Exception.Message
}finally{
    $after=@(Get-ChildItem -LiteralPath $raw -File -Recurse|Sort-Object FullName|Get-FileHash -Algorithm SHA256|Select-Object Path,Hash)
    Write-JsonUtf8 (Join-Path $area 'raw-after.json') $after
    $state.raw_unchanged=($before|ConvertTo-Json -Depth 4 -Compress) -eq ($after|ConvertTo-Json -Depth 4 -Compress)
    if(-not $state.raw_unchanged){$state.status='failed';$state.error='Original CPU raw samples changed during report recovery.'}
    Save-Recovery;$lock.Dispose()
}
Write-Host ('CPU report recovery: '+$state.status+'; '+(Get-RelativePath $root $area))
if($state.status -ne 'recovered'){throw $state.error}
