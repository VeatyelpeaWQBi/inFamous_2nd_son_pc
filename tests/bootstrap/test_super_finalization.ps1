# SPDX-License-Identifier: GPL-2.0-or-later
# Exercise the real redirected helper lifecycle with the installed Windows
# PowerShell, plus pure completion policy. No game, profiler or controller input.
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot '../../scripts/tools.ps1')
. (Join-Path $PSScriptRoot '../../scripts/diagnostic_tools.ps1')
. (Join-Path $PSScriptRoot '../../scripts/super_capture_tools.ps1')
$root=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
$fixture=Resolve-ProjectChild $root ('state/test-super-finalization-'+[Guid]::NewGuid().ToString('N'))
$child=$null
function Final-Launch {
    return [pscustomobject]@{schema=3;path_base='project';session='state/sessions/finalization-test'
        super_capture=$true;super_capture_batch='state/super-captures/finalization-test'
        complete=$true;exit_code=0;progress_published=$false;source_game_written=$false}
}
function Errors($Code,$Launch) {
    return @(Get-SuperFinalizationErrors $Code $Launch 'state/super-captures/finalization-test' 'state/sessions/finalization-test')
}
try{
    $null=[IO.Directory]::CreateDirectory($fixture)
    foreach($expected in @(0,7)){
        $arguments=@('-NoLogo','-NoProfile','-NonInteractive','-Command',
            ("[Console]::Out.WriteLine('output-"+$expected+"'); Start-Sleep -Milliseconds 300; exit "+$expected))
        $output=Join-Path $fixture ('output-'+$expected+'.log')
        $options=@{
            FilePath=(Join-Path $env:WINDIR 'System32/WindowsPowerShell/v1.0/powershell.exe')
            ArgumentList=(($arguments|ForEach-Object{Quote-WindowsArgument $_}) -join ' ')
            WorkingDirectory=$root;WindowStyle='Hidden';PassThru=$true
            RedirectStandardOutput=$output;RedirectStandardError=(Join-Path $fixture ('error-'+$expected+'.log'))
        }
        $child=Start-DiagnosticProcess $options
        while(-not $child.HasExited){Start-Sleep -Milliseconds 20}
        if(-not $child.WaitForExit(10000)){throw 'Offline helper did not exit.'}
        $code=$child.ExitCode
        if($null -eq $code -or $code -ne $expected){throw ('Redirected helper lost exit code '+$expected)}
        if([IO.File]::ReadAllText($output).Trim() -ne ('output-'+$expected)){throw 'Redirected helper output was not flushed.'}
        $child.Dispose();$child=$null
        $errors=@(Errors $code (Final-Launch))
        if($expected -eq 0 -and $errors.Count){throw 'Successful launcher/game/isolation evidence was rejected.'}
        if($expected -ne 0 -and -not @($errors|Where-Object {$_ -like 'Launcher returned*'}).Count){throw 'Nonzero launcher exit was accepted.'}
    }
    if(-not @(Errors $null (Final-Launch)).Count){throw 'An unknown launcher exit code cannot establish ordinary success.'}
    $collector=[pscustomobject]@{status='partial';errors=@('VTune parsing: internal assertion at 57%')}
    $details=@(Get-SuperFinalizationErrors 1 (Final-Launch) 'state/super-captures/finalization-test' 'state/sessions/finalization-test' $collector)
    if(-not @($details|Where-Object {$_ -like '*VTune parsing: internal assertion*'}).Count){throw 'Finalization must surface the actual collector failure, not only the launcher exit.'}
    $launch=Final-Launch;$launch.progress_published=$true
    if(-not @((Errors 0 $launch)|Where-Object {$_ -like 'Save isolation failed*'}).Count){throw 'Actual save publication must fail explicitly.'}
    $launch=Final-Launch;$launch.complete=$false
    if(-not @(Errors 0 $launch).Count){throw 'An incomplete launcher record was accepted.'}
    $launch=Final-Launch;$launch.exit_code=9
    if(-not @((Errors 0 $launch)|Where-Object {$_ -like 'Game returned*'}).Count){throw 'Actual game failure must fail explicitly.'}
    $launch=Final-Launch;$launch.source_game_written=$true
    if(-not @(Errors 0 $launch).Count){throw 'Written source game must not be accepted.'}
    $launch=Final-Launch;$launch.super_capture_batch='state/super-captures/another-batch'
    if(-not @(Errors 0 $launch).Count){throw 'A different batch cannot finalize this capture.'}
    if(-not @(Errors 0 ([pscustomobject]@{complete=$true})).Count){throw 'Missing isolation evidence cannot establish success.'}
    Write-Host 'PASS: redirected helper exit 0/7 retained; unknown/nonzero exit, incomplete/mismatched records and save failures rejected. No game started.'
}finally{
    if($child){$child.Dispose()}
    $absolute=[IO.Path]::GetFullPath($fixture)
    $allowed=[IO.Path]::GetFullPath((Join-Path $root 'state')).TrimEnd('\')+'\'
    if(-not $absolute.StartsWith($allowed,[StringComparison]::OrdinalIgnoreCase) -or
        [IO.Path]::GetFileName($absolute) -notmatch '^test-super-finalization-[a-f0-9]{32}$'){
        throw 'Refusing cleanup outside the isolated finalization fixture.'
    }
    if(Test-Path -LiteralPath $absolute){Remove-Item -LiteralPath $absolute -Recurse -Force}
}
