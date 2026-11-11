# SPDX-License-Identifier: GPL-2.0-or-later
# Replay the real recovery runner with tiny installed PowerShell children only.
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot '../../scripts/tools.ps1')
. (Join-Path $PSScriptRoot '../../scripts/diagnostic_tools.ps1')
$root=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
$area=Join-Path $root ('state/test-super-recovery-'+[Guid]::NewGuid().ToString('N'))
$null=[IO.Directory]::CreateDirectory($area)
$tokens=$null;$errors=$null
$ast=[Management.Automation.Language.Parser]::ParseFile((Join-Path $root 'scripts/resume_super_full.ps1'),[ref]$tokens,[ref]$errors)
if($errors.Count){throw ($errors.Message -join '; ')}
foreach($name in @('Save-RecoveryProgress','Get-RecoveryKey','Invoke-DiagnosticNative')){
    $definition=$ast.Find({param($node)$node -is [Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -eq $name},$true)
    Invoke-Expression $definition.Extent.Text
}
$script:recoveryInput='offline-fixture';$RebuildVtune=$false
$script:recoveryMemory=[pscustomobject]@{AvailablePhysicalMemory=64GB};$script:recoveryMemoryFloor=2GB
$progressPath=Join-Path $area 'status.json';$script:recoveryProgress=[ordered]@{status='testing';updated_utc=$null;stage=$null;pid=$null}
$preflight=[pscustomobject]@{checks=[pscustomobject]@{wpr=[pscustomobject]@{validated=$false}}}
try{
    $child=Join-Path $area 'child.ps1';$counter=Join-Path $area 'counter.txt';$output=Join-Path $area 'report.csv'
    $source=@'
param([Alias('o')][string]$Output,[string]$Counter,[int]$Sleep=0)
$count=if(Test-Path -LiteralPath $Counter){[int][IO.File]::ReadAllText($Counter)}else{0}
[IO.File]::WriteAllText($Counter,[string]($count+1))
[Console]::Out.WriteLine('child-progress')
if($Sleep){Start-Sleep -Seconds $Sleep}
[IO.File]::WriteAllText($Output,'metric,value'+[Environment]::NewLine+'test,1')
'@
    [IO.File]::WriteAllText($child,$source,[Text.UTF8Encoding]::new($true))
    $exe=Join-Path $env:WINDIR 'System32/WindowsPowerShell/v1.0/powershell.exe'
    $arguments=@('-NoProfile','-ExecutionPolicy','Bypass','-File',$child,'-o',$output,'-Counter',$counter)
    $log=Join-Path $area 'step.log'
    Invoke-DiagnosticNative $exe $arguments $log 10000
    Invoke-DiagnosticNative $exe $arguments $log 10000
    if([IO.File]::ReadAllText($counter) -ne '1'){throw 'Completed, hash-verified report was executed twice.'}
    [IO.File]::WriteAllText($output,'corrupted')
    Invoke-DiagnosticNative $exe $arguments $log 10000
    if([IO.File]::ReadAllText($counter) -ne '2'){throw 'A modified report incorrectly reused its checkpoint.'}
    $timedOut=$false
    try{Invoke-DiagnosticNative $exe ($arguments+@('-Sleep','5')) (Join-Path $area 'timeout.log') 1000}catch{
        if($_.Exception.Message -notlike '*bounded timeout*'){throw};$timedOut=$true
    }
    if(-not $timedOut -or [IO.File]::ReadAllText((Join-Path $area 'timeout.log')) -notmatch 'child-progress'){throw 'Interrupted parser lost its output or did not stop.'}
    $key=Get-RecoveryKey $exe ($arguments+@('-Sleep','5'))
    if(Test-Path -LiteralPath (Join-Path $area ($key+'.complete.json'))){throw 'An interrupted parser was marked complete.'}
    $script:recoveryMemory.AvailablePhysicalMemory=1GB;$lowMemory=$false
    $memoryArguments=$arguments+@('-Sleep','6')
    try{Invoke-DiagnosticNative $exe $memoryArguments (Join-Path $area 'memory.log') 10000}catch{
        if($_.Exception.Message -notlike '*physical memory reached the recovery reserve*'){throw};$lowMemory=$true
    }
    if(-not $lowMemory -or (Test-Path -LiteralPath (Join-Path $area ((Get-RecoveryKey $exe $memoryArguments)+'.complete.json')))){
        throw 'Low memory must stop the owned parser without writing a success checkpoint.'
    }
    Write-Host 'PASS: real recovery checkpoint reuse, modified-output rerun, bounded owned-parser stop and retained logs. No game or profiler started.'
}finally{
    $absolute=[IO.Path]::GetFullPath($area)
    $allowed=[IO.Path]::GetFullPath((Join-Path $root 'state')).TrimEnd('\')+'\'
    if(-not $absolute.StartsWith($allowed,[StringComparison]::OrdinalIgnoreCase) -or [IO.Path]::GetFileName($absolute) -notmatch '^test-super-recovery-[a-f0-9]{32}$'){throw 'Refusing cleanup outside the recovery fixture.'}
    Remove-Item -LiteralPath $absolute -Recurse -Force
}
