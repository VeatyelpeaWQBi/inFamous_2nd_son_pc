# SPDX-License-Identifier: GPL-2.0-or-later
# Replay the production collector block with fake tools; no profiler/game starts.
Set-StrictMode -Version Latest
$ErrorActionPreference='Stop'
$root=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
$fixture=Join-Path $root ('state/test-super-collector-start-'+[Guid]::NewGuid().ToString('N'))
try{
    $tokens=$null;$errors=$null
    $ast=[Management.Automation.Language.Parser]::ParseFile((Join-Path $root 'scripts/diagnostic_tools.ps1'),[ref]$tokens,[ref]$errors)
    if($errors.Count){throw ($errors.Message -join '; ')}
    $knobFunction=$ast.Find({param($node)$node -is [Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -eq 'Get-DiagnosticVtuneKnobs'},$true)
    Invoke-Expression $knobFunction.Extent.Text
    $assignment=$ast.Find({param($node)$node -is [Management.Automation.Language.AssignmentStatementAst] -and $node.Left.Extent.Text -eq '$startCollectors'},$true)
    Invoke-Expression $assignment.Extent.Text
    $sessionPath=$fixture;$dir=$fixture
    $spec=@{Nsight=$true;VtuneAnalysis='memory-access'}
    $tools=@{ngfx='fake-ngfx';wpr='fake-wpr';vtune='fake-vtune';nvidia_smi=$null}
    $target=@{Id=42};$plan=@{profile='full';wpr_profile='IFSSConcurrent';wpr_instance='offline'
        collector_timing=@{nsight_timeout_seconds=4000;vtune_seconds=4000};storage=@{vtune_data_limit_mib=0}}
    $state=@{helpers=@();errors=@();collector_start_errors=@{};vtune_started=$false}
    $statusPath=Join-Path $fixture 'unused.json';$nsight=$null;$vtune=$null;$smi=$null;$wprAttempted=$false;$wprOwned=$false
    $script:started=@();$script:wprCalls=0
    function Assert-ProjectPath {}
    function Quote-WindowsArgument($Value){return [string]$Value}
    function Write-JsonUtf8 {}
    function Get-DiagnosticWprArguments {return @('offline')}
    function Invoke-DiagnosticNative {$script:wprCalls++}
    function Start-DiagnosticProcess($Options){
        $script:started+=@($Options.FilePath)
        return [pscustomobject]@{Id=(100+$script:started.Count);StartTime=[DateTime]::UtcNow}
    }
    . $startCollectors -BootstrapOnly
    if(($script:started -join ',') -ne 'fake-ngfx' -or $wprAttempted -or $state.vtune_started){throw 'Startup must arm Nsight alone, without WPR/VTune.'}
    . $startCollectors
    if(($script:started -join ',') -ne 'fake-ngfx,fake-vtune' -or $script:wprCalls -ne 1 -or -not $state.vtune_started){throw 'Formal collection must start WPR/VTune without restarting Nsight.'}
    . $startCollectors
    if($script:started.Count -ne 2 -or $script:wprCalls -ne 1){throw 'Repeated readiness polling restarted a collector.'}
    Write-Host 'PASS: production block arms Nsight at boot, defers WPR/VTune, and starts each tool once. No game or profiler started.'
}finally{
    $allowed=[IO.Path]::GetFullPath((Join-Path $root 'state')).TrimEnd('\')+'\'
    if(-not $fixture.StartsWith($allowed,[StringComparison]::OrdinalIgnoreCase) -or [IO.Path]::GetFileName($fixture) -notmatch '^test-super-collector-start-[a-f0-9]{32}$'){throw 'Refusing cleanup outside fixture.'}
    if(Test-Path -LiteralPath $fixture){Remove-Item -LiteralPath $fixture -Recurse -Force}
}
