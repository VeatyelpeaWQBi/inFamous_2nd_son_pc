# SPDX-License-Identifier: GPL-2.0-or-later
# Import only: no process starts until Start-SuperCpuHotspots is explicitly called.
function Save-SuperCpuHotspots($Owner) {
    $Owner.State.updated_utc=[DateTime]::UtcNow.ToString('o')
    Write-JsonUtf8 (Join-Path $Owner.Directory 'status.json') $Owner.State
}
function Start-SuperCpuHotspots([string]$Root,[string]$Session,[ref]$Owner,
                              [ValidateSet('hw','sw')][string]$SamplingMode='hw') {
    if($SamplingMode -eq 'hw' -and -not(Test-DiagnosticAdministrator)){throw 'CPU hardware sampling requires an elevated Windows PowerShell.'}
    $record=Read-ProjectSession $Root $Session
    if(-not $record.Launch.super_capture -or -not $record.Launch.super_capture_batch -or -not $record.Launch.cpu_hotspots){throw 'CPU sampling requires an owned isolated super CPU capture.'}
    $target=Get-DiagnosticTarget $record.Launch $Root
    if(-not $target){throw 'The owned target is not running.'}
    try{
        $tools=Get-DiagnosticTools
        if(-not $tools.vtune -or -not(Test-Path -LiteralPath $tools.vtune -PathType Leaf)){throw 'Installed VTune is unavailable.'}
        $dir=Join-Path $record.Path 'diagnostics/cpu-hotspots'
        $null=[IO.Directory]::CreateDirectory($dir)
        $null=[IO.Directory]::CreateDirectory((Join-Path $dir 'vtune-user'))
        $state=[ordered]@{schema=1;status='attaching';session=$Session;target_pid=$target.Id
            target_started=$record.Launch.process_started;runtime_sha256=$record.Launch.runtime_sha256
            analysis='hotspots';sampling_mode=$SamplingMode;sampling_interval_ms=$(if($SamplingMode -eq 'hw'){5}else{10});stack_collection=$true
            hardware_metrics=($SamplingMode -eq 'hw');software_raw_limit_mib=$(if($SamplingMode -eq 'sw'){64}else{$null})
            gpu_queries=$false;full_capture=$false;performance_acceptance=$false
            collection_requested_utc=[DateTime]::UtcNow.ToString('o');sampling_ack_utc=$null
            collector_pid=$null;vtune_exit_code=$null;errors=@();updated_utc=$null;phases=@()}
        $ownerValue=[pscustomobject]@{Root=$Root;Session=$Session;Directory=$dir;Tools=$tools
            Process=$null;State=$state;Exported=$false}
        $Owner.Value=$ownerValue
        $collectorArguments=@('-collect','hotspots','-knob','sampling-mode=hw','-knob','enable-stack-collection=true'
            '-knob','sampling-interval=5','-target-pid',[string]$target.Id,'-duration','60'
            '-data-limit','0','-finalization-mode','none','-result-dir',(Join-Path $dir 'vtune')
            '-user-data-dir',(Join-Path $dir 'vtune-user'))
        if($SamplingMode -eq 'sw'){
            $collectorArguments=@('-collect','hotspots','-target-pid',[string]$target.Id,'-duration','60'
                '-data-limit','64','-finalization-mode','none','-result-dir',(Join-Path $dir 'vtune')
                '-user-data-dir',(Join-Path $dir 'vtune-user'))
            $collectorArguments+=@(Get-DiagnosticVtuneKnobs @{vtune_sampling_mode='sw'} 'hotspots')
        }
        $options=@{FilePath=$tools.vtune;ArgumentList=(($collectorArguments|ForEach-Object{Quote-WindowsArgument $_}) -join ' ')
            WorkingDirectory=$record.Path;PassThru=$true;WindowStyle='Hidden'
            RedirectStandardOutput=(Join-Path $dir 'vtune-output.log');RedirectStandardError=(Join-Path $dir 'vtune-errors.log')}
        $ownerValue.Process=Start-DiagnosticProcess $options
        $state.collector_pid=$ownerValue.Process.Id
        Save-SuperCpuHotspots $ownerValue
        $wait=[Diagnostics.Stopwatch]::StartNew()
        while($true){
            if($ownerValue.Process.HasExited){throw 'CPU collector exited before acknowledging sampling; see vtune-errors.log.'}
            if($target.HasExited){throw 'Owned game exited while CPU collector was attaching.'}
            $log=Join-Path $dir 'vtune-errors.log'
            if((Test-Path -LiteralPath $log) -and (Read-DiagnosticSharedText $log).Contains('Collection started.')){break}
            if($wait.Elapsed.TotalSeconds -gt 90){throw 'CPU collector did not acknowledge sampling within 90 seconds.'}
            Start-Sleep -Milliseconds 250
        }
        $state.sampling_ack_utc=[DateTime]::UtcNow.ToString('o');$state.status='sampling'
        Save-SuperCpuHotspots $ownerValue
    }catch{
        if($Owner.Value){
            $Owner.Value.State.status='failed';$Owner.Value.State.errors+=('Attach: '+$_.Exception.Message)
            if($Owner.Value.Process -and $Owner.Value.Process.HasExited){$Owner.Value.State.vtune_exit_code=$Owner.Value.Process.ExitCode}
            Save-SuperCpuHotspots $Owner.Value
        }
        throw
    }finally{$target.Dispose()}
}
function Stop-SuperCpuHotspots($Owner) {
    if(-not $Owner -or -not $Owner.Process){return}
    try{
        Stop-DiagnosticVtune $Owner.Process $Owner.Tools $Owner.Directory $Owner.State $Owner.Root $Owner.Session 90000
        if($Owner.State.vtune_exit_code -ne 0){throw 'CPU collector did not exit successfully.'}
        $Owner.State.status='detached';Save-SuperCpuHotspots $Owner
    }catch{
        $Owner.State.status='failed';$Owner.State.errors+=('Detach: '+$_.Exception.Message)
        Save-SuperCpuHotspots $Owner;throw
    }
}
function Complete-SuperCpuHotspots($Owner,$Cycle) {
    if(-not $Owner -or $Owner.Exported){return}
    if(-not $Owner.Process.HasExited -or $Owner.State.vtune_exit_code -ne 0){throw 'CPU export requires a verified clean detach.'}
    $dir=$Owner.Directory;$tool=$Owner.Tools.vtune
    $result=Join-Path $dir 'vtune';$user=Join-Path $dir 'vtune-user'
    $Owner.State.status='finalizing';Save-SuperCpuHotspots $Owner
    try{
        $null=Invoke-DiagnosticVtuneFinalize $tool $result $user (Join-Path $dir 'finalize.log') 600000
        Invoke-DiagnosticNative $tool @('-report','summary','-format','csv','-csv-delimiter','comma','-result-dir',$result,'-user-data-dir',$user,
            '-report-output',(Join-Path $dir 'summary.csv')) (Join-Path $dir 'summary.log') 120000
        $summary=@(Import-Csv -LiteralPath (Join-Path $dir 'summary.csv'))
        $stamp=($summary|Where-Object {$_.'Metric Name' -eq 'Collection start time'}|Select-Object -First 1).'Metric Value'
        $style=[Globalization.DateTimeStyles]::AssumeUniversal -bor [Globalization.DateTimeStyles]::AdjustToUniversal
        $start=[DateTime]::ParseExact($stamp,"HH:mm:ss dd/MM/yyyy 'UTC'",[Globalization.CultureInfo]::InvariantCulture,$style)
        $release=[DateTimeOffset]::FromUnixTimeMilliseconds([long][Math]::Floor($Cycle.release.applied_unix_ns/1e6)).UtcDateTime
        $windows=@(@{name='baseline';lo=-8;hi=-1},@{name='front-middle';lo=0;hi=9},@{name='late';lo=9;hi=18})
        foreach($window in $windows){
            $bounds=Get-DiagnosticCpuTimeRange $start ($release.AddSeconds($window.lo).ToString('o')) ($release.AddSeconds($window.hi).ToString('o'))
            $file=Join-Path $dir ($window.name+'-hotspots.csv')
            Invoke-DiagnosticNative $tool @('-report','hotspots','-format','csv','-csv-delimiter','comma','-group-by','process,thread,function'
                '-time-filter',$bounds.filter,'-result-dir',$result,'-user-data-dir',$user,'-report-output',$file) (Join-Path $dir ($window.name+'-report.log')) 120000
            $rows=@(Import-Csv -LiteralPath $file)
            $target=@($rows|Where-Object {[string]$_.'PID' -eq [string]$Owner.State.target_pid})
            if(-not $target.Count){throw ('No explicitly matching target PID rows in '+$window.name)}
            $cp=@($target|Where-Object {$_.'Thread' -like '*GpuCommandProcessor*'})
            $identity='thread-name'
            if(-not $cp.Count -and $target[0].PSObject.Properties['TID']){
                $tids=@($target|Where-Object {$_.'Function' -eq 'AmdGpu::Liverpool::ProcessGraphics' -and $_.'TID'}|Select-Object -ExpandProperty TID -Unique)
                if($tids.Count -eq 1){
                    $cp=@($target|Where-Object {$_.'TID' -eq $tids[0]})
                    $identity='unique-ProcessGraphics-TID'
                }
            }
            if(-not $cp.Count){throw ('Command processor thread was not resolved in '+$window.name)}
            $cpu=0.0
            foreach($row in $cp){$cpu+=[double]::Parse($row.'CPU Time',[Globalization.CultureInfo]::InvariantCulture)}
            if($cpu -le 0){throw ('No command processor CPU samples in '+$window.name)}
            $Owner.State.phases+=@{name=$window.name;begin_seconds=$bounds.begin;end_seconds=$bounds.end
                timestamp_uncertainty_seconds=1;target_rows=$target.Count;cp_rows=$cp.Count;cp_cpu_seconds=$cpu;cp_identity=$identity
                csv=[IO.Path]::GetFileName($file)}
            Save-SuperCpuHotspots $Owner
        }
        $Owner.State.status='collected';$Owner.State.warm=[bool]$Cycle.warm
        $Owner.State.meter_consumed=[bool]$Cycle.meter_consumed;$Owner.Exported=$true
        Save-SuperCpuHotspots $Owner
    }catch{
        $Owner.State.status='failed';$Owner.State.errors+=('Export: '+$_.Exception.Message)
        Save-SuperCpuHotspots $Owner;throw
    }
}
