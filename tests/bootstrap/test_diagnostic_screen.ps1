# SPDX-License-Identifier: GPL-2.0-or-later
# Synthetic PNG/OCR check only. No game, desktop capture, or controller input.
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot '../../scripts/tools.ps1')
. (Join-Path $PSScriptRoot '../../scripts/diagnostic_automation.ps1')
$projectRoot=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
$testRoot=Resolve-ProjectChild $projectRoot ('state/offline-tests/'+[Guid]::NewGuid().ToString('N'))
$null=[IO.Directory]::CreateDirectory($testRoot)
try{
    Add-Type -AssemblyName System.Drawing
    $fixture=Join-Path $testRoot 'menu.png'
    $bitmap=[Drawing.Bitmap]::new(800,240)
    $graphics=$null;$font=$null
    try{
        $graphics=[Drawing.Graphics]::FromImage($bitmap)
        $font=[Drawing.Font]::new([Drawing.FontFamily]::GenericSansSerif,36,[Drawing.FontStyle]::Regular,[Drawing.GraphicsUnit]::Pixel)
        $graphics.Clear([Drawing.Color]::Black)
        $graphics.DrawString('CONTINUE',$font,[Drawing.Brushes]::White,[single]40,[single]40)
        $graphics.DrawString('NEW GAME',$font,[Drawing.Brushes]::White,[single]40,[single]120)
        $bitmap.Save($fixture,[Drawing.Imaging.ImageFormat]::Png)
    }finally{
        if($font){$font.Dispose()};if($graphics){$graphics.Dispose()};$bitmap.Dispose()
    }
    if(-not (Test-DiagnosticPngReady $fixture)){throw 'A completed PNG was rejected.'}
    $pending=Join-Path $testRoot 'pending.png'
    if(Test-DiagnosticPngReady $pending){throw 'An absent screenshot was accepted.'}
    $bytes=[IO.File]::ReadAllBytes($fixture)
    $stream=[IO.File]::Open($pending,[IO.FileMode]::Create,[IO.FileAccess]::Write,[IO.FileShare]::ReadWrite)
    try{
        $stream.Write($bytes,0,$bytes.Length);$stream.Flush()
        if(Test-DiagnosticPngReady $pending){throw 'A PNG still held by its writer was accepted.'}
    }finally{$stream.Dispose()}
    if(-not (Test-DiagnosticPngReady $pending)){throw 'A PNG was not accepted after its writer closed.'}
    $stream=[IO.File]::Open($pending,[IO.FileMode]::Truncate,[IO.FileAccess]::Write,[IO.FileShare]::ReadWrite)
    try{$stream.Write($bytes,0,$bytes.Length-12)}finally{$stream.Dispose()}
    if(Test-DiagnosticPngReady $pending){throw 'A truncated screenshot without IEND was accepted.'}
    [IO.File]::WriteAllBytes($pending,[byte[]]::new(0))
    if(Test-DiagnosticPngReady $pending){throw 'An empty screenshot was accepted.'}
    $screen=Read-DiagnosticScreen $fixture
    $decision=Get-DiagnosticMenuDecision $screen.text
    if($decision.action -ne 'button' -or $decision.reason -ne 'continue-save'){throw ('Windows OCR could not read the synthetic menu: '+$screen.text)}
    Write-Host 'PNG completion, writer sharing and actual Windows OCR checks passed.'
}finally{
    Assert-ProjectPath $projectRoot $testRoot
    foreach($name in @('menu.png','pending.png')){
        $path=Join-Path $testRoot $name
        Assert-ProjectPath $projectRoot $path
        if(Test-Path -LiteralPath $path){Remove-Item -LiteralPath $path -Force}
    }
    [IO.Directory]::Delete($testRoot)
}
