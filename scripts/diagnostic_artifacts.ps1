# SPDX-License-Identifier: GPL-2.0-or-later
# Parse saved artifacts with the installed tools after the target has exited.
function Get-DiagnosticCpuTimeRange([DateTime]$CollectionStart,[string]$WindowStart,[string]$WindowEnd) {
    $begin=[Math]::Max([double]0,([DateTime]::Parse($WindowStart).ToUniversalTime()-$CollectionStart).TotalSeconds+1)
    $end=([DateTime]::Parse($WindowEnd).ToUniversalTime()-$CollectionStart).TotalSeconds-1
    if($end -le $begin){throw 'VTune gameplay CPU range is too short.'}
    $culture=[Globalization.CultureInfo]::InvariantCulture
    return @{begin=$begin;end=$end;filter=($begin.ToString('F6',$culture)+':'+$end.ToString('F6',$culture))}
}
function Complete-DiagnosticArtifacts([string]$Root,[string]$Session,$Plan,$State) {
    $spec=Get-DiagnosticPlanSpec $Plan;$tools=Get-DiagnosticTools
    $dir=Join-Path $Session 'diagnostics';$validation=@{}
    if($State.wpr_saved){
        try{
            $report=Join-Path $dir 'wpr-statistics.txt'
            Invoke-DiagnosticNative $tools.xperf @('-i',(Join-Path $dir 'timeline.etl'),'-o',$report,
                '-a','tracestats','-timespan','actual','-detail') (Join-Path $dir 'wpr-parse.log') 1800000
            if(-not(Test-Path -LiteralPath $report) -or (Get-Item -LiteralPath $report).Length -eq 0){throw 'WPR event parser produced no statistics.'}
            if($Plan.profile -eq 'full' -and $Plan.cpu_sampling_backend -ne 'vtune-hardware' -and $State.window_started -and $State.window_finished){
                $text=[IO.File]::ReadAllText($report)
                $match=[regex]::Match($text,'Start time \(UTC\)\s*:\s*(\d{4}/\d\d/\d\d:\d\d:\d\d:\d\d\.\d{7})')
                if(-not $match.Success){throw 'WPR trace start time is unavailable.'}
                $style=[Globalization.DateTimeStyles]::AssumeUniversal -bor [Globalization.DateTimeStyles]::AdjustToUniversal
                $traceStart=[DateTime]::ParseExact($match.Groups[1].Value,'yyyy/MM/dd:HH:mm:ss.fffffff',[Globalization.CultureInfo]::InvariantCulture,$style)
                $begin=[long][Math]::Floor(([DateTime]::Parse($State.window_started).ToUniversalTime()-$traceStart).TotalMilliseconds*1000)
                $end=[long][Math]::Ceiling(([DateTime]::Parse($State.window_finished).ToUniversalTime()-$traceStart).TotalMilliseconds*1000)
                if($begin -lt 0 -or $end -le $begin){throw 'WPR gameplay range is invalid.'}
                Invoke-DiagnosticNative $tools.xperf @('-i',(Join-Path $dir 'timeline.etl'),'-o',(Join-Path $dir 'wpr-gameplay-cpu.txt'),
                    '-a','profile','-detail','-range',[string]$begin,[string]$end) (Join-Path $dir 'wpr-gameplay-parse.log') 1800000
                Write-JsonUtf8 (Join-Path $dir 'wpr-gameplay-cpu.json') @{
                    target_pid=$State.target_pid;window_started=$State.window_started;window_finished=$State.window_finished
                    start_us=$begin;end_us=$end;method='xperf-profile-gameplay-range'
                }
            }
            $validation['wpr']=@{validated=$true;method='xperf-event-parser';report=(Get-RelativePath $Root $report)}
        }catch{$validation['wpr']=@{validated=$false;error=$_.Exception.Message};$State.errors+=('WPR parsing: '+$_.Exception.Message)}
    }
    if($spec.VtuneAnalysis -and $State.vtune_started -and $State.vtune_exit_code -eq 0){
        try{
            $result=Join-Path $dir 'vtune';$user=Join-Path $dir 'vtune-user'
            Invoke-DiagnosticNative $tools.vtune @('-finalize','-finalization-mode','full',
                '-result-dir',$result,'-user-data-dir',$user) (Join-Path $dir 'vtune-finalize.log') 1800000
            $report=Join-Path $dir 'vtune-summary.csv'
            Invoke-DiagnosticNative $tools.vtune @('-report','summary','-format','csv','-csv-delimiter','comma',
                '-result-dir',$result,'-user-data-dir',$user,'-report-output',$report) (Join-Path $dir 'vtune-report.log') 1800000
            if(-not(Test-Path -LiteralPath $report) -or (Get-Item -LiteralPath $report).Length -eq 0){throw 'VTune returned no parsed summary.'}
            if($Plan.profile -eq 'full' -and $Plan.cpu_sampling_backend -eq 'vtune-hardware'){
                $summary=@(Import-Csv -LiteralPath $report)
                $stamp=($summary|Where-Object {$_.'Metric Name' -eq 'Collection start time'}|Select-Object -First 1).'Metric Value'
                $style=[Globalization.DateTimeStyles]::AssumeUniversal -bor [Globalization.DateTimeStyles]::AdjustToUniversal
                $collectionStart=[DateTime]::ParseExact($stamp,"HH:mm:ss dd/MM/yyyy 'UTC'",[Globalization.CultureInfo]::InvariantCulture,$style)
                # Summary timestamps have one-second resolution. Trim both edges,
                # so startup samples cannot leak into this gameplay-only CPU report.
                $bounds=Get-DiagnosticCpuTimeRange $collectionStart $State.window_started $State.window_finished
                $begin=$bounds.begin;$end=$bounds.end;$range=$bounds.filter
                Invoke-DiagnosticNative $tools.vtune @('-report','hotspots','-format','csv','-csv-delimiter','comma',
                    '-group-by','process,function','-time-filter',$range,'-result-dir',$result,'-user-data-dir',$user,
                    '-report-output',(Join-Path $dir 'vtune-gameplay-hotspots.csv')) (Join-Path $dir 'vtune-gameplay-report.log') 1800000
                Write-JsonUtf8 (Join-Path $dir 'vtune-gameplay-hotspots.json') @{
                    target_pid=$State.target_pid;window_started=$State.window_started;window_finished=$State.window_finished
                    collection_start_utc=$collectionStart.ToString('o');begin_seconds=$begin;end_seconds=$end
                    timestamp_uncertainty_seconds=1;method='vtune-time-filtered-target-hotspots'
                }
            }
            $validation['vtune']=@{validated=$true;method='vtune-full-finalization-and-summary';report=(Get-RelativePath $Root $report)}
        }catch{$validation['vtune']=@{validated=$false;error=$_.Exception.Message};$State.errors+=('VTune parsing: '+$_.Exception.Message)}
    }
    if($spec.RenderDoc){
        try{
            $capture=@(Get-ChildItem -LiteralPath (Join-Path $Session 'user/captures') -Filter '*.rdc' -File -Recurse -ErrorAction SilentlyContinue|Sort-Object LastWriteTimeUtc -Descending|Select-Object -First 1)
            if(-not $capture.Count){throw 'RenderDoc has no frame to parse.'}
            Assert-ProjectPath $Root $capture[0].FullName
            $command=Join-Path (Split-Path -Parent $tools.renderdoc) 'renderdoccmd.exe'
            $preview=Join-Path $dir 'renderdoc-thumbnail.png'
            Invoke-DiagnosticNative $command @('thumb','--out',$preview,'--max-size','512',$capture[0].FullName) (Join-Path $dir 'renderdoc-parse.log')
            if(-not(Test-DiagnosticPngReady $preview)){throw 'RenderDoc could not decode the saved frame thumbnail.'}
            $validation['renderdoc']=@{validated=$true;method='renderdoc-capture-parser-and-thumbnail';report=(Get-RelativePath $Root $preview)}
        }catch{$validation['renderdoc']=@{validated=$false;error=$_.Exception.Message};$State.errors+=('RenderDoc parsing: '+$_.Exception.Message)}
    }
    if($spec.Nsight){
        $captureRoot=Join-Path $Session 'user/captures/nsight'
        $reports=@()
        foreach($file in @(Get-ChildItem -LiteralPath $captureRoot -File -Recurse -ErrorAction SilentlyContinue)){
            if($file.Length -eq 0 -or $file.Extension -notin @('.xls','.csv','.json')){continue}
            Assert-ProjectPath $Root $file.FullName
            try{
                if($file.Extension -eq '.json'){
                    $null=Read-JsonUtf8 $file.FullName;$rows=1
                }else{
                    # Nsight exports text tables with an .xls suffix, not Excel workbooks.
                    $rows=$null
                    if($Plan.profile -ne 'full'){
                        $rows=0
                        foreach($line in [IO.File]::ReadLines($file.FullName)){
                            if($line -match '[\t,].*\d'){$rows++}
                        }
                        if($rows -eq 0){continue}
                    }
                    # Full uses the streaming content parser below. Do not scan GB tables
                    # twice with a PowerShell regex; exact row counts live in data-quality.json.
                }
                $reports+=@{path=(Get-RelativePath $Root $file.FullName);numeric_rows=$rows}
            }catch{$State.errors+=('Nsight export parsing: '+$_.Exception.Message)}
        }
        $frames=@(Get-ChildItem -LiteralPath $captureRoot -File -ErrorAction SilentlyContinue|
            Where-Object {$_.Extension -in @('.ngfx-gputrace','.nsight-gputrace') -and $_.Length -gt 0})
        $inScene=@($frames|Where-Object {$State.window_started -and $_.CreationTimeUtc -ge [DateTime]::Parse($State.window_started).ToUniversalTime()})
        $sdkAccepted=$State.Contains('nsight_sdk_accepted') -and $State.nsight_sdk_accepted
        $valid=($State.nsight_exit_code -eq 0 -and $reports.Count -gt 0 -and $inScene.Count -gt 0 -and $sdkAccepted)
        $validation['nsight']=@{validated=$valid;method='nsight-native-sdk-and-decoded-metrics';
            native_trigger_accepted=$sdkAccepted;scene_capture_files=@($inScene|ForEach-Object {Get-RelativePath $Root $_.FullName});reports=$reports}
        if(-not $valid){$State.errors+='Nsight metrics or an SDK-triggered gameplay capture could not be verified.'}
    }
    if($Plan.profile -eq 'full'){
        # Inspect the final clock/data records, not just tool exit codes and file presence.
        Write-JsonUtf8 (Join-Path $dir 'status.json') $State
        Invoke-DiagnosticNative (Resolve-InstalledPython) @('-B',(Join-Path $PSScriptRoot 'diagnostic_quality.py'),
            '--session',(Get-RelativePath $Root $Session)) (Join-Path $dir 'data-quality.log') 1800000
        $quality=Read-JsonUtf8 (Join-Path $dir 'data-quality.json')
        $validation['data_quality']=@{validated=[bool]$quality.complete;report=(Get-RelativePath $Root (Join-Path $dir 'data-quality.json'))}
        foreach($name in @('wpr','vtune','nsight')){
            if($validation.ContainsKey($name) -and $quality.checks.PSObject.Properties[$name]){
                $validation[$name].parsed=$validation[$name].validated
                $validation[$name].validated=[bool]$quality.checks.$name.validated
            }
        }
        foreach($name in @('frames','symbols','sensors','cpu_hotspots','diagnostic_exit')){
            if($quality.checks.PSObject.Properties[$name]){
                $validation[$name]=@{validated=[bool]$quality.checks.$name.validated;method='clock-and-content-quality-check';report=$validation.data_quality.report}
            }
        }
        $State.data_quality=@{complete=[bool]$quality.complete;report=$validation.data_quality.report;limitations=@($quality.limitations)}
        $State.errors+=@($quality.errors)
    }
    Write-JsonUtf8 (Join-Path $dir 'artifact-validation.json') $validation
    return $validation
}
