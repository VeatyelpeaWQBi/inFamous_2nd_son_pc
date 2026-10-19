# SPDX-License-Identifier: GPL-2.0-or-later
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot '../../scripts/tools.ps1')
. (Join-Path $PSScriptRoot '../../scripts/diagnostic_sensors.ps1')
$root=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
$path=Resolve-ProjectChild $root ('state/offline-tests/sensors-'+[Guid]::NewGuid().ToString('N'))
$null=[IO.Directory]::CreateDirectory($path)
try{
    foreach($truncated in @($false,$true)){
        $sensor=New-DiagnosticSensors 'never-started.exe' $path
        $process=[pscustomobject]@{HasExited=$true;ExitCode=0;ExitTime=[DateTime]::Parse('2026-01-01T00:00:01Z').ToLocalTime()}
        $process|Add-Member ScriptMethod Dispose {}
        $sensor.process=$process
        $row=if($truncated){'2026/01/01 00:00:00,0,NVIDIA'}else{'2026/01/01 00:00:00,0,NVIDIA,40,10,2000,16000,100,2000,9000'}
        $sensor.output=[Threading.Tasks.Task]::FromResult([string]('timestamp,index,name,gpu,memory,used,total,power,sm,mem'+[Environment]::NewLine+$row))
        $sensor.error_output=[Threading.Tasks.Task]::FromResult([string]'')
        $sensor.started_qpc=[Diagnostics.Stopwatch]::GetTimestamp()
        Update-DiagnosticSensors $sensor -Stopping
        Stop-DiagnosticSensors $sensor
        $status=Read-JsonUtf8 (Join-Path $path 'sensors-status.json')
        $lines=@([IO.File]::ReadAllLines((Join-Path $path 'nvidia.csv')))
        if($truncated){
            if($status.complete -or $status.rows -ne 0 -or -not $status.errors.Count -or $lines.Count -ne 1){throw 'A partial sensor row was accepted or written.'}
        }else{
            if(-not $status.complete -or $status.rows -ne 1 -or $lines.Count -ne 2 -or $lines[1].Split(',').Count -ne 13 -or $lines[1].Split(',')[0] -ne '2026-01-01T00:00:01.0000000Z'){throw 'A complete sensor sample lost data or QPC/UTC clocks.'}
        }
    }
    Write-Host 'Complete sensor-row writes, shutdown and truncated-query rejection checks passed.'
}finally{
    foreach($name in @('nvidia.csv','sensors-status.json')){$file=Join-Path $path $name;if(Test-Path -LiteralPath $file){Remove-Item -LiteralPath $file -Force}}
    [IO.Directory]::Delete($path)
}
