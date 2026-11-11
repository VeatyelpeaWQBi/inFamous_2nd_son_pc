# SPDX-License-Identifier: GPL-2.0-or-later
# Explicit offline report replay; no game/collection, dependency install or original trace edit.
[CmdletBinding()]
param([Parameter(Mandatory)][string]$Session,
      [ValidateSet('baseline','front-middle','late')][string]$Phase='front-middle',
      [switch]$XmlRngLifetimeGuard)
Set-StrictMode -Version Latest
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot 'tools.ps1')
. (Join-Path $PSScriptRoot 'build_tools.ps1')
. (Join-Path $PSScriptRoot 'diagnostic_tools.ps1')
. (Join-Path $PSScriptRoot 'diagnostic_artifacts.ps1')
$root=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$record=Read-ProjectSession $root $Session
if(-not $record.Launch.complete){throw 'Offline report probe requires the game to have exited.'}
$dir=Join-Path $record.Path 'diagnostics/cpu-hotspots'
$result=Join-Path $dir 'vtune';$user=Join-Path $dir 'vtune-user'
if(-not(Test-Path -LiteralPath (Join-Path $result 'data.0') -PathType Container)){throw 'Existing CPU raw data is required.'}
$run=Join-Path $dir ('report-probe-'+(Get-Date -Format 'yyyyMMdd-HHmmss')+'-'+[Guid]::NewGuid().ToString('N').Substring(0,8))
$null=[IO.Directory]::CreateDirectory($run)
$before=@(Get-ChildItem -LiteralPath (Join-Path $result 'data.0') -File -Recurse|Get-FileHash -Algorithm SHA256|Select-Object Path,Hash)
Write-JsonUtf8 (Join-Path $run 'raw-before.json') $before
$buildTools=Get-ProjectBuildTools $root
$environment=Get-ProjectBuildEnvironment $buildTools.CMAKE_CXX_COMPILER $root
$object=Join-Path $run 'probe.obj';$exe=Join-Path $run 'report-probe.exe'
Invoke-ProjectBuildProcess $buildTools.CMAKE_CXX_COMPILER @('/nologo','/c','/O2','/Z7','/EHsc','/std:c++20',
    (Join-Path $root 'tests/bootstrap/vtune_report_probe.cpp'),('/Fo'+$object)) $root $environment -Capture | Out-Null
$linker=Join-Path (Split-Path -Parent $buildTools.CMAKE_CXX_COMPILER) 'lld-link.exe'
Invoke-ProjectBuildProcess $linker @($object,'kernel32.lib','dbghelp.lib','bcrypt.lib',('/out:'+$exe),'/debug',('/pdb:'+(Join-Path $run 'probe.pdb')),'/subsystem:console') $root $environment -Capture | Out-Null
if($XmlRngLifetimeGuard){Invoke-ProjectBuildProcess $exe @('--self-test') $root $environment -Capture | Write-Host}
$summary=@(Import-Csv -LiteralPath (Join-Path $dir 'summary.csv'))
$stamp=($summary|Where-Object {$_.'Metric Name' -eq 'Collection start time'}|Select-Object -First 1).'Metric Value'
$style=[Globalization.DateTimeStyles]::AssumeUniversal -bor [Globalization.DateTimeStyles]::AdjustToUniversal
$start=[DateTime]::ParseExact($stamp,"HH:mm:ss dd/MM/yyyy 'UTC'",[Globalization.CultureInfo]::InvariantCulture,$style)
$cycles=Read-JsonUtf8 (Join-Path $record.Path 'super-rounds.json')
$release=[DateTimeOffset]::FromUnixTimeMilliseconds([long][Math]::Floor($cycles[0].release.applied_unix_ns/1e6)).UtcDateTime
$lo=if($Phase -eq 'baseline'){-8}elseif($Phase -eq 'front-middle'){0}else{9}
$hi=if($Phase -eq 'baseline'){-1}elseif($Phase -eq 'front-middle'){9}else{18}
$bounds=Get-DiagnosticCpuTimeRange $start ($release.AddSeconds($lo).ToString('o')) ($release.AddSeconds($hi).ToString('o'))
$vtune=(Get-DiagnosticTools).vtune
$report=Join-Path $run ($Phase+'-hotspots.csv')
$arguments=@((Join-Path $run 'crash.dmp'),$vtune,'-report','hotspots','-format','csv','-csv-delimiter','comma',
    '-group-by','process,thread,function','-time-filter',$bounds.filter,'-result-dir',$result,'-user-data-dir',$user,'-report-output',$report)
if($XmlRngLifetimeGuard){$arguments=@('--xml-rng-lifetime')+$arguments}
Write-JsonUtf8 (Join-Path $run 'probe.json') @{status='starting';phase=$Phase;time_filter=$bounds.filter;session=$Session;game_started=$false;collection_started=$false}
$code=125;$probe=$null
try{
    $start=[Diagnostics.ProcessStartInfo]::new($exe)
    $start.Arguments=($arguments|ForEach-Object{Quote-WindowsArgument $_}) -join ' '
    $start.WorkingDirectory=$root;$start.UseShellExecute=$false;$start.CreateNoWindow=$true
    $start.RedirectStandardOutput=$true;$start.RedirectStandardError=$true
    $start.StandardOutputEncoding=[Text.UTF8Encoding]::new($false)
    $start.StandardErrorEncoding=[Text.UTF8Encoding]::new($false)
    $probe=[Diagnostics.Process]::Start($start)
    $stdout=$probe.StandardOutput.ReadToEndAsync();$stderr=$probe.StandardError.ReadToEndAsync()
    if(-not $probe.WaitForExit(200000)){
        $probe.Kill();$probe.WaitForExit();throw 'Owned offline debugger exceeded 200 seconds.'
    }
    $code=$probe.ExitCode
    [IO.File]::WriteAllText((Join-Path $run 'probe.log'),$stdout.Result,[Text.UTF8Encoding]::new($false))
    [IO.File]::WriteAllText((Join-Path $run 'probe-stderr.log'),$stderr.Result,[Text.UTF8Encoding]::new($false))
}catch{
    $_ | Out-String | Set-Content -LiteralPath (Join-Path $run 'probe-error.log') -Encoding UTF8
}finally{
    if($probe){$probe.Dispose()}
    $after=@(Get-ChildItem -LiteralPath (Join-Path $result 'data.0') -File -Recurse|Get-FileHash -Algorithm SHA256|Select-Object Path,Hash)
    Write-JsonUtf8 (Join-Path $run 'raw-after.json') $after
    $unchanged=($before|ConvertTo-Json -Depth 4 -Compress) -eq ($after|ConvertTo-Json -Depth 4 -Compress)
    Write-JsonUtf8 (Join-Path $run 'probe.json') @{status='finished';phase=$Phase;time_filter=$bounds.filter;session=$Session;game_started=$false;collection_started=$false;xml_rng_lifetime_guard=[bool]$XmlRngLifetimeGuard;exit_code=$code;raw_unchanged=$unchanged;report=$report}
}
Write-Host ('Offline probe: '+(Get-RelativePath $root $run))
