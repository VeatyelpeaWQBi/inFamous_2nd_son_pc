# SPDX-License-Identifier: GPL-2.0-or-later
# Real Windows PowerShell parameter-binding replay; no UAC, game or profiler.
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot '../../scripts/tools.ps1')
. (Join-Path $PSScriptRoot '../../scripts/diagnostic_tools.ps1')
$root=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
$fixture=Resolve-ProjectChild $root ('state/test-super-elevation-'+[Guid]::NewGuid().ToString('N'))
$child=$null
function Invoke-Receiver([string[]]$Arguments,[string]$Name) {
    $options=@{FilePath=(Join-Path $env:WINDIR 'System32/WindowsPowerShell/v1.0/powershell.exe')
        ArgumentList=(($Arguments|ForEach-Object{Quote-WindowsArgument $_}) -join ' ')
        WorkingDirectory=$root;WindowStyle='Hidden';PassThru=$true
        RedirectStandardOutput=(Join-Path $fixture ($Name+'-output.log'))
        RedirectStandardError=(Join-Path $fixture ($Name+'-error.log'))}
    $script:child=Start-DiagnosticProcess $options
    if(-not $child.WaitForExit(10000)){throw 'Offline argument receiver did not exit.'}
    $code=$child.ExitCode;$child.Dispose();$script:child=$null
    return $code
}
function Expect-EntryFailure($Code,$Index,[DateTime]$Start,[string]$Text) {
    $errors=@(Get-SuperFullEntryErrors $Code $Index $Start)
    if(-not @($errors|Where-Object {$_ -like ('*'+$Text+'*')}).Count){throw ('Entry outcome did not reject '+$Text)}
}
try{
    $null=[IO.Directory]::CreateDirectory($fixture)
    $receiver=Join-Path $fixture 'receiver with spaces.ps1'
    $source=@'
[CmdletBinding()]
param([switch]$FullCapture,[string]$Mode='pro4k',[string]$Profile='suite',[int]$Rounds=3,
      [int]$MinimumWarmup=5,[int]$MaximumWarmup=15,[int]$BaselineSeconds=10,
      [int]$ReleaseSeconds=30,[int]$PeakSeconds=5)
__FULL_DEFAULTS__
$result=@{full=[bool]$FullCapture;mode=$Mode;profile=$Profile;rounds=$Rounds;vtune_cpu_mode=$VtuneCpuMode
    minimum=$MinimumWarmup;maximum=$MaximumWarmup;baseline=$BaselineSeconds;release=$ReleaseSeconds;peak=$PeakSeconds}
[IO.File]::WriteAllText(($PSCommandPath+'.receipt.json'),($result|ConvertTo-Json),[Text.UTF8Encoding]::new($false))
exit 0
'@
    $tokens=$null;$parseErrors=$null
    $entry=[Management.Automation.Language.Parser]::ParseFile((Join-Path $root 'scripts/collect_super.ps1'),[ref]$tokens,[ref]$parseErrors)
    $fullDefault=$entry.Find({param($node)$node -is [Management.Automation.Language.IfStatementAst] -and $node.Extent.Text -eq 'if($FullCapture){if(-not $PSBoundParameters.ContainsKey(''Rounds'')){$Rounds=1};$MinimumWarmup=0;$MaximumWarmup=0}'},$true)
    if($parseErrors.Count -or -not $fullDefault){throw 'The real collector Full defaults are missing or invalid.'}
    $prefix=$source.Substring(0,$source.IndexOf('__FULL_DEFAULTS__'))
    $source=$source.Replace($prefix,("[CmdletBinding()]`r`n"+$entry.ParamBlock.Extent.Text+"`r`n"))
    $source=$source.Replace('__FULL_DEFAULTS__',$fullDefault.Extent.Text)
    [IO.File]::WriteAllText($receiver,$source,[Text.UTF8Encoding]::new($true))
    # Reproduce the exact old precedence bug against a real -File parameter binder.
    $oldBound=@{Rounds=3};$oldArguments=@('-NoLogo','-NoProfile','-ExecutionPolicy','Bypass','-File',$receiver,'-FullCapture')
    foreach($key in $oldBound.Keys){$oldArguments+=@('-'+$key,[string]$oldBound[$key])}
    if($oldArguments[-1] -ne '-Rounds 3'){throw 'The original Windows PowerShell concatenation bug was not reproduced.'}
    if((Invoke-Receiver $oldArguments 'old') -eq 0 -or (Test-Path -LiteralPath ($receiver+'.receipt.json'))){throw 'The malformed original elevated invocation unexpectedly bound parameters.'}
    if([IO.File]::ReadAllText((Join-Path $fixture 'old-error.log')) -notlike '*Rounds 3*'){throw 'The original failure was not the reproduced combined parameter.'}
    $bound=@{FullCapture=$true;Mode='1440p';Profile='suite';Rounds=3;MinimumWarmup=3;MaximumWarmup=6;BaselineSeconds=5;ReleaseSeconds=25;PeakSeconds=1;VtuneCpuMode='hardware-memory'}
    $arguments=@(Get-SuperElevatedArguments $receiver $bound)
    if($arguments -notcontains '-Rounds' -or $arguments -contains '-Rounds 3'){throw 'The fixed elevation arguments are still concatenated.'}
    if((Invoke-Receiver $arguments 'fixed') -ne 0){throw 'The fixed invocation failed in installed Windows PowerShell.'}
    $actual=Read-JsonUtf8 ($receiver+'.receipt.json')
    if(-not $actual.full -or $actual.mode -ne '1440p' -or $actual.rounds -ne 3 -or $actual.minimum -ne 0 -or
       $actual.maximum -ne 0 -or $actual.baseline -ne 5 -or $actual.release -ne 25 -or $actual.peak -ne 1 -or $actual.vtune_cpu_mode -ne 'hardware-memory'){throw 'The elevated receiver lost a collection parameter.'}
    $defaults=@(Get-SuperElevatedArguments $receiver @{FullCapture=$true})
    if((Invoke-Receiver $defaults 'defaults') -ne 0){throw 'A Full invocation without explicit options must preserve defaults.'}
    $actual=Read-JsonUtf8 ($receiver+'.receipt.json')
    if(-not $actual.full -or $actual.mode -ne 'pro4k' -or $actual.rounds -ne 1 -or $actual.minimum -ne 0 -or $actual.maximum -ne 0 -or $actual.vtune_cpu_mode -ne 'hardware-memory'){throw 'Full must default to hardware Memory Access, one release without super warmup.'}
    $one=@(Get-SuperElevatedArguments $receiver @{FullCapture=$true;Rounds=1})
    if((Invoke-Receiver $one 'explicit-one') -ne 0){throw 'Explicit Full single round failed.'}
    $actual=Read-JsonUtf8 ($receiver+'.receipt.json')
    if($actual.rounds -ne 1){throw 'One Full round must remain one release after elevation.'}
    $downgrade=@(Get-SuperElevatedArguments $receiver @{FullCapture=$true;VtuneCpuMode='stable'})
    if((Invoke-Receiver $downgrade 'rejected-downgrade') -eq 0){throw 'Full must reject software sampling instead of silently reducing scope.'}
    $native=@('-NoLogo','-NoProfile','-ExecutionPolicy','Bypass','-File',$receiver)
    if((Invoke-Receiver $native 'native-defaults') -ne 0){throw 'Native defaults failed.'}
    $actual=Read-JsonUtf8 ($receiver+'.receipt.json')
    if($actual.full -or $actual.rounds -ne 2){throw 'Native comparison must default to two releases.'}
    $start=[DateTime]::UtcNow.AddSeconds(-1)
    Expect-EntryFailure 0 $null $start 'did not publish'
    $index=[pscustomobject]@{batch='state/super-captures/offline';status='collected';error=$null;updated_utc=$start.AddSeconds(-5).ToString('o')}
    Expect-EntryFailure 0 $index $start 'earlier run'
    $index.updated_utc=[DateTime]::UtcNow.ToString('o')
    Expect-EntryFailure $null $index $start 'unavailable'
    Expect-EntryFailure 7 $index $start 'exit code 7'
    $index.status='failed';$index.error='offline startup error'
    Expect-EntryFailure 0 $index $start 'offline startup error'
    $index.status='collected';$index.error=$null
    if(@(Get-SuperFullEntryErrors 0 $index $start).Count){throw 'A new, collected Full result with a known successful exit was rejected.'}
    Write-Host 'PASS: original combined -Rounds 3 failure reproduced; fixed/default arguments bind in real PS5; missing/stale/failed/unknown-exit results cannot report success. No UAC or game started.'
}finally{
    if($child){$child.Dispose()}
    $absolute=[IO.Path]::GetFullPath($fixture)
    $allowed=[IO.Path]::GetFullPath((Join-Path $root 'state')).TrimEnd('\')+'\'
    if(-not $absolute.StartsWith($allowed,[StringComparison]::OrdinalIgnoreCase) -or [IO.Path]::GetFileName($absolute) -notmatch '^test-super-elevation-[a-f0-9]{32}$'){throw 'Refusing cleanup outside the elevation fixture.'}
    if(Test-Path -LiteralPath $absolute){Remove-Item -LiteralPath $absolute -Recurse -Force}
}
