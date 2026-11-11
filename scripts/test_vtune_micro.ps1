# SPDX-License-Identifier: GPL-2.0-or-later
[CmdletBinding()]
param([ValidateRange(3,10)][int]$Seconds=10,
    [ValidateSet('memory-access','hotspots-sw')][string]$Analysis='memory-access',[switch]$MapEngine)
Set-StrictMode -Version Latest
$ErrorActionPreference='Stop'
# Do not substitute a software test when hardware driver access is unavailable.
if($Analysis -eq 'memory-access' -and -not ([Security.Principal.WindowsPrincipal]::new([Security.Principal.WindowsIdentity]::GetCurrent())).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)){
    throw 'Hardware Memory Access micro validation requires an elevated Windows PowerShell. No software fallback or game is started.'
}
. (Join-Path $PSScriptRoot 'tools.ps1')
. (Join-Path $PSScriptRoot 'build_tools.ps1')
. (Join-Path $PSScriptRoot 'diagnostic_tools.ps1')
$root=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$area=Resolve-ProjectChild $root 'state/vtune-micro'
$run=Join-Path $area ((Get-Date -Format 'yyyyMMdd-HHmmss')+'-'+$Analysis+'-full'+$(if($MapEngine){'-symbols'}else{''}))
$null=[IO.Directory]::CreateDirectory($run)
$null=[IO.Directory]::CreateDirectory((Join-Path $run 'temp'))
$buildTools=Get-ProjectBuildTools $root
$environment=Get-ProjectBuildEnvironment $buildTools.CMAKE_CXX_COMPILER $root
$vtune=(Get-DiagnosticTools).vtune
$sdk=Join-Path (Split-Path -Parent (Split-Path -Parent $vtune)) 'sdk'
$exe=Join-Path $run 'vtune-micro.exe'
$pdb=Join-Path $run 'vtune-micro.pdb'
$source=Join-Path $root 'tests/bootstrap/vtune_micro.cpp'
$object=Join-Path $run 'vtune-micro.obj'
Invoke-ProjectBuildProcess $buildTools.CMAKE_CXX_COMPILER @('/nologo','/c','/O2','/Z7','/EHsc',('/I'+(Join-Path $sdk 'include')),$source,('/Fo'+$object)) $area $environment -Capture | Out-Null
$linker=Join-Path (Split-Path -Parent $buildTools.CMAKE_CXX_COMPILER) 'lld-link.exe'
Invoke-ProjectBuildProcess $linker @($object,(Join-Path $sdk 'lib64/libittnotify.lib'),'kernel32.lib',('/out:'+$exe),'/debug',('/pdb:'+$pdb),'/subsystem:console') $area $environment -Capture | Out-Null
$target=$null;$collector=$null
$receipt=[ordered]@{finalization_mode='full';analysis=$Analysis;seconds=$Seconds;run=(Get-RelativePath $root $run);status='starting';error=$null;game_started=$false}
function Save-Micro {Write-JsonUtf8 (Join-Path $run 'receipt.json') $receipt}
try{
    Save-Micro
    $start=[Diagnostics.ProcessStartInfo]::new($exe)
    $detach=Join-Path $run 'detach';$ack=Join-Path $run 'detach-ack'
    $start.Arguments=(@($detach,$ack)|ForEach-Object{Quote-WindowsArgument $_}) -join ' '
    if($MapEngine){
        $project=Read-JsonUtf8 (Join-Path $root 'project.json');$engine=Resolve-ProjectChild $root $project.runtime_path
        if((Get-FileHash $engine -Algorithm SHA256).Hash.ToLowerInvariant() -ne $project.runtime_sha256){throw 'Micro symbol mapping requires the verified current engine.'}
        $receipt.mapped_engine_sha256=$project.runtime_sha256
        $start.Arguments+=' '+(Quote-WindowsArgument $engine)
    }
    $start.UseShellExecute=$false;$start.CreateNoWindow=$true
    $start.EnvironmentVariables['INTEL_LIBITTNOTIFY64']=Join-Path (Split-Path -Parent $vtune) 'runtime/ittnotify_collector.dll'
    $start.EnvironmentVariables['TEMP']=Join-Path $run 'temp';$start.EnvironmentVariables['TMP']=Join-Path $run 'temp'
    $target=[Diagnostics.Process]::Start($start)
    $receipt.target_pid=$target.Id
    $result=Join-Path $run 'vtune';$user=Join-Path $run 'vtune-user'
    $null=[IO.Directory]::CreateDirectory($user)
    $arguments=@('-collect','memory-access','-target-pid',[string]$target.Id,'-duration','30','-data-limit','32',
        '-finalization-mode','none','-result-dir',$result,'-user-data-dir',$user)
    $arguments+=@(Get-DiagnosticVtuneKnobs @{} 'memory-access')
    if($Analysis -eq 'hotspots-sw'){
        $arguments=@('-collect','hotspots','-target-pid',[string]$target.Id,'-duration','30','-data-limit','32',
            '-finalization-mode','none','-result-dir',$result,'-user-data-dir',$user)
        $arguments+=@(Get-DiagnosticVtuneKnobs @{vtune_sampling_mode='sw'} 'hotspots')
    }
    $options=@{FilePath=$vtune;ArgumentList=(($arguments|ForEach-Object{Quote-WindowsArgument $_}) -join ' ');WorkingDirectory=$run;WindowStyle='Hidden';PassThru=$true
        RedirectStandardOutput=(Join-Path $run 'collect-output.log');RedirectStandardError=(Join-Path $run 'collect-errors.log')}
    $collector=Start-DiagnosticProcess $options
    $receipt.collector_pid=$collector.Id;$receipt.status='attaching';Save-Micro
    $wait=[Diagnostics.Stopwatch]::StartNew()
    do{
        if($collector.HasExited){throw ('Micro collector exited before sampling: '+(Read-DiagnosticSharedText (Join-Path $run 'collect-errors.log')))}
        if($wait.Elapsed.TotalSeconds -gt 90){throw 'Micro attach timed out.'}
        Start-Sleep -Milliseconds 250
    }while((Read-DiagnosticSharedText (Join-Path $run 'collect-errors.log')) -notmatch 'Collection started\.')
    $receipt.sampling_started_utc=[DateTime]::UtcNow.ToString('o');$receipt.status='sampling';Save-Micro
    Start-Sleep -Seconds $Seconds
    $receipt.sampling_finished_utc=[DateTime]::UtcNow.ToString('o')
    [IO.File]::WriteAllText($detach,'detach')
    if(-not $collector.WaitForExit(30000) -or $collector.ExitCode -ne 0){throw 'Micro collector detach did not finish successfully.'}
    if(-not $target.WaitForExit(5000) -or $target.ExitCode -ne 0 -or -not(Test-Path $ack)){throw 'Micro ITT detach acknowledgement missing.'}
    $receipt.status='finalizing';Save-Micro
    $arguments=@('-finalize','-finalization-mode','full','-result-dir',$result,'-user-data-dir',$user)
    Invoke-DiagnosticNative $vtune $arguments (Join-Path $run 'finalize.log') 180000
    Invoke-DiagnosticNative $vtune @('-report','summary','-format','csv','-csv-delimiter','comma','-result-dir',$result,'-user-data-dir',$user,
        '-report-output',(Join-Path $run 'summary.csv')) (Join-Path $run 'summary.log') 60000
    Invoke-DiagnosticNative $vtune @('-report','hotspots','-format','csv','-csv-delimiter','comma','-group-by','process,function','-result-dir',$result,'-user-data-dir',$user,
        '-report-output',(Join-Path $run 'hotspots.csv')) (Join-Path $run 'hotspots.log') 60000
    $hotspots=Get-Content (Join-Path $run 'hotspots.csv') -Raw
    if($hotspots -notmatch 'MemoryPass'){throw 'Micro report did not resolve the sampled native function.'}
    if($Seconds -ge 9){
        # Execute the production exporter itself, including the three real
        # time-filtered reports, rather than a hand-written approximation.
        $tokens=$null;$parseErrors=$null
        $ast=[Management.Automation.Language.Parser]::ParseFile((Join-Path $PSScriptRoot 'diagnostic_artifacts.ps1'),[ref]$tokens,[ref]$parseErrors)
        if($parseErrors.Count){throw 'Production artifact exporter syntax error.'}
        $block=$ast.Find({param($node)$node -is [Management.Automation.Language.IfStatementAst] -and $node.Extent.Text.StartsWith('if($spec.VtuneAnalysis -and $State.vtune_started')},$true)
        if(-not $block){throw 'Production VTune exporter not found.'}
        $backend=if($Analysis -eq 'memory-access'){'vtune-hardware'}else{'vtune-software'}
        $Plan=[pscustomobject]@{profile='full';cpu_sampling_backend=$backend;super_capture=@{rounds=3}}
        $a=[DateTime]::Parse($receipt.sampling_started_utc).ToUniversalTime();$b=[DateTime]::Parse($receipt.sampling_finished_utc).ToUniversalTime()
        $State=[ordered]@{vtune_started=$true;vtune_exit_code=0;target_pid=$receipt.target_pid;window_started=$a.ToString('o');window_finished=$b.ToString('o');errors=@();super_qualified_windows=@()}
        foreach($i in 0..2){$State.super_qualified_windows+=@{cycle=($i+1);release_sequence=($i+1);window_started=$a.AddSeconds(($b-$a).TotalSeconds*$i/3).ToString('o');window_finished=$a.AddSeconds(($b-$a).TotalSeconds*($i+1)/3).ToString('o')}}
        $spec=@{VtuneAnalysis=$(if($Analysis -eq 'memory-access'){'memory-access'}else{'hotspots'})};$tools=@{vtune=$vtune};$dir=$run;$Root=$root;$validation=@{}
        Invoke-Expression $block.Extent.Text
        if($State.errors.Count){throw ($State.errors -join '; ')}
        $State.cpu_sampling_backend=$backend
        Write-JsonUtf8 (Join-Path $run 'production-state.json') $State
        Copy-Item -LiteralPath (Join-Path $run 'collect-errors.log') -Destination (Join-Path $run 'vtune-errors.log')
        $python=Resolve-InstalledPython
        Invoke-DiagnosticNative $python @('-X','utf8','-B',(Join-Path $root 'tests/bootstrap/verify_vtune_micro.py'),$run) (Join-Path $run 'verify.log') 30000
        $receipt.production_export_verified=$true
    }
    if($MapEngine -and (Get-FileHash $engine -Algorithm SHA256).Hash.ToLowerInvariant() -ne $receipt.mapped_engine_sha256){throw 'The original engine file changed during micro validation.'}
    $receipt.status='passed';Save-Micro
    Write-Host ('Micro validation passed: '+$receipt.run)
}catch{$receipt.status='failed';$receipt.error=$_.Exception.Message;Save-Micro;throw}
finally{
    foreach($owned in @($collector,$target)){
        if($owned){if(-not $owned.HasExited){$owned.Kill();$owned.WaitForExit()};$owned.Dispose()}
    }
}
