# SPDX-License-Identifier: GPL-2.0-or-later
# A short PowerShell child checks helper handles, redirection and nonzero exit status. No engine.
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot '../../scripts/tools.ps1')
. (Join-Path $PSScriptRoot '../../scripts/diagnostic_tools.ps1')
$projectRoot=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
$testRoot=Resolve-ProjectChild $projectRoot ('state/offline-tests/'+[Guid]::NewGuid().ToString('N'))
$null=[IO.Directory]::CreateDirectory($testRoot)
try{
    $outputPath=Join-Path $testRoot 'output.log';$errorPath=Join-Path $testRoot 'errors.log'
    $arguments=@('-NoLogo','-NoProfile','-NonInteractive','-Command',"[Console]::Out.WriteLine('phase-output'); [Console]::Error.WriteLine('phase-error'); exit 7")
    $options=@{
        FilePath=(Join-Path $env:WINDIR 'System32/WindowsPowerShell/v1.0/powershell.exe')
        ArgumentList=(($arguments|ForEach-Object {Quote-WindowsArgument $_}) -join ' ')
        WorkingDirectory=$projectRoot;PassThru=$true;WindowStyle='Hidden'
        RedirectStandardOutput=$outputPath;RedirectStandardError=$errorPath
    }
    $process=Start-DiagnosticProcess $options
    try{$process.WaitForExit();$code=$process.ExitCode}finally{$process.Dispose()}
    if($code -ne 7){throw ('A nonzero phase exit code was lost: '+$code)}
    if([IO.File]::ReadAllText($outputPath).Trim() -ne 'phase-output'){throw 'Phase stdout was not redirected.'}
    if([IO.File]::ReadAllText($errorPath).Trim() -ne 'phase-error'){throw 'Phase stderr was not redirected.'}
    $self=[Diagnostics.Process]::GetCurrentProcess()
    try{
        $identity=Get-ProjectProcessIdentity $self
        if($identity.Path -ne $self.MainModule.FileName -or $identity.CreationFileTime -ne $self.StartTime.ToFileTimeUtc()){
            throw 'Native process identity disagrees with the current process.'
        }
        $started=$self.StartTime.ToUniversalTime().ToString('o')
        if(-not(Test-ProjectProcessIdentity $self $identity.Path $started)){throw 'The actual process identity was rejected.'}
        if(Test-ProjectProcessIdentity $self $identity.Path $self.StartTime.AddSeconds(1).ToUniversalTime().ToString('o')){
            throw 'A different process creation time was accepted.'
        }
    }finally{$self.Dispose()}
    Write-Host 'Windows process identity, exit code and output redirection checks passed.'
}finally{
    Assert-ProjectPath $projectRoot $testRoot
    foreach($name in @('output.log','errors.log')){
        $path=Join-Path $testRoot $name
        Assert-ProjectPath $projectRoot $path
        if(Test-Path -LiteralPath $path){Remove-Item -LiteralPath $path -Force}
    }
    [IO.Directory]::Delete($testRoot)
}
