# SPDX-License-Identifier: GPL-2.0-or-later
# One completed nvidia-smi query per sample. Never terminate a CSV writer mid-row.
function New-DiagnosticSensors([string]$Executable,[string]$Directory,[string[]]$QueryFields=@()) {
    if($QueryFields.Count -and ($QueryFields.Count -ne 10 -or @($QueryFields|Where-Object {$_ -notmatch '^[a-zA-Z0-9_.]+$'}).Count)){
        throw 'A sensor query requires ten valid NVIDIA field names.'
    }
    $arguments=if($QueryFields.Count){@(
        ('--query-gpu='+($QueryFields -join ','))
        '--format=csv,nounits'
    )}else{@(Get-DiagnosticSensorArguments $Directory)}
    return [pscustomobject]@{
        executable=$Executable;directory=$Directory;arguments=$arguments;process=$null;output=$null;error_output=$null
        writer=[IO.StreamWriter]::new((Join-Path $Directory 'nvidia.csv'),$false,[Text.UTF8Encoding]::new($false))
        clock=[Diagnostics.Stopwatch]::StartNew();last_started=-2.0;started_qpc=0L
        header=$null;rows=0;errors=[Collections.Generic.List[string]]::new()
    }
}
function Update-DiagnosticSensors($Sensor,[switch]$Stopping) {
    if($Sensor.process -and $Sensor.process.HasExited){
        try{
            $text=$Sensor.output.Result;$errorText=$Sensor.error_output.Result
            if($Sensor.process.ExitCode -ne 0){throw ('nvidia-smi returned '+$Sensor.process.ExitCode+': '+$errorText.Trim())}
            $lines=@($text.TrimEnd().Split([char]10)|ForEach-Object {$_.TrimEnd([char]13)})
            if($lines.Count -lt 2){throw 'nvidia-smi returned no complete sample.'}
            $fields=@($lines[0].Split(',')|ForEach-Object {$_.Trim()})
            if($fields.Count -ne 10){throw 'Unexpected NVIDIA sensor column count.'}
            if(-not $Sensor.header){
                $Sensor.header=$lines[0]
                $Sensor.writer.AutoFlush=$true
                $Sensor.writer.WriteLine('utc,query_start_qpc,query_observed_end_qpc,'+$Sensor.header)
            }elseif($Sensor.header -ne $lines[0]){throw 'NVIDIA sensor columns changed during collection.'}
            $rows=@($text|ConvertFrom-Csv)
            $end=[Diagnostics.Stopwatch]::GetTimestamp();$utc=$Sensor.process.ExitTime.ToUniversalTime().ToString('o')
            for($i=1;$i -lt $lines.Count;$i++){
                if($lines[$i].Split(',').Count -ne 10 -or @($rows[$i-1].PSObject.Properties).Count -ne 10){throw 'Incomplete NVIDIA sensor row.'}
                $Sensor.writer.WriteLine($utc+','+$Sensor.started_qpc+','+$end+','+$lines[$i])
                $Sensor.rows++
            }
        }catch{$Sensor.errors.Add($_.Exception.Message)}
        finally{$Sensor.process.Dispose();$Sensor.process=$null}
    }
    if(-not $Stopping -and -not $Sensor.process -and ($Sensor.clock.Elapsed.TotalSeconds-$Sensor.last_started) -ge 1){
        $start=[Diagnostics.ProcessStartInfo]::new()
        $start.FileName=$Sensor.executable
        $start.Arguments=($Sensor.arguments|ForEach-Object {Quote-WindowsArgument $_}) -join ' '
        $start.UseShellExecute=$false;$start.CreateNoWindow=$true
        $start.RedirectStandardOutput=$true;$start.RedirectStandardError=$true
        $Sensor.started_qpc=[Diagnostics.Stopwatch]::GetTimestamp()
        $Sensor.last_started=$Sensor.clock.Elapsed.TotalSeconds
        $Sensor.process=[Diagnostics.Process]::Start($start)
        $Sensor.output=$Sensor.process.StandardOutput.ReadToEndAsync()
        $Sensor.error_output=$Sensor.process.StandardError.ReadToEndAsync()
    }
}
function Stop-DiagnosticSensors($Sensor) {
    try{
        if($Sensor.process -and -not $Sensor.process.WaitForExit(10000)){
            $Sensor.process.Kill();$Sensor.process.WaitForExit()
            $Sensor.errors.Add('The last NVIDIA query timed out; no partial row was written.')
        }
        Update-DiagnosticSensors $Sensor -Stopping
    }finally{
        if($Sensor.process){$Sensor.process.Dispose();$Sensor.process=$null}
        $Sensor.writer.Dispose()
        Write-JsonUtf8 (Join-Path $Sensor.directory 'sensors-status.json') @{
            rows=$Sensor.rows;errors=@($Sensor.errors);complete=($Sensor.rows -gt 0 -and $Sensor.errors.Count -eq 0)
        }
    }
}
