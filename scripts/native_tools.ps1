# SPDX-License-Identifier: GPL-2.0-or-later
# Existing Windows PowerShell/.NET only. Importing defines functions; it starts no tools or game.
# Keep this bootstrap independent of each branch's tools.ps1 implementation.
function Assert-NativeProjectPath([string]$Root,[string]$Path) {
    $base=[IO.Path]::GetFullPath($Root).TrimEnd('\')
    $full=[IO.Path]::GetFullPath($Path)
    if(-not $full.StartsWith($base+'\',[StringComparison]::OrdinalIgnoreCase)){throw 'Native helper path escaped the project.'}
    for($entry=$full;$entry -ne $base;$entry=[IO.Path]::GetDirectoryName($entry)){
        $item=Get-Item -LiteralPath $entry -Force -ErrorAction SilentlyContinue
        if($item -and ($item.Attributes -band [IO.FileAttributes]::ReparsePoint)){throw 'Native helper path contains a link.'}
    }
}
function Initialize-ProjectNativeTools {
    param([switch]$IncludeTests)
    if('Ifss.Native.Command' -as [type]){
        if($IncludeTests -and -not('Ifss.Native.BootstrapTests' -as [type])){
            throw 'Run test.bat in a fresh PowerShell process to include the native regression sources.'
        }
        return
    }
    if([IntPtr]::Size -ne 8){throw 'Use the existing Windows x64 PowerShell.'}
    $root=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
    $sources=@(Get-ChildItem -LiteralPath (Join-Path $PSScriptRoot 'native') -Filter *.cs -File|Sort-Object Name|ForEach-Object FullName)
    if($IncludeTests){$sources+=Join-Path $root 'tests/bootstrap/test_native.cs'}
    foreach($source in $sources){Assert-NativeProjectPath $root $source}
    $temporary=[IO.Path]::GetFullPath((Join-Path $root 'state/runtime/native-helper-temp'))
    Assert-NativeProjectPath $root $temporary
    $null=[IO.Directory]::CreateDirectory($temporary)
    $previousTemp=$env:TEMP;$previousTmp=$env:TMP
    try{
        $env:TEMP=$temporary;$env:TMP=$temporary
        $references=@('System.dll','System.Core.dll','System.Web.Extensions.dll','System.Xml.dll',
                      'System.Xml.Linq.dll','System.Drawing.dll','System.Windows.Forms.dll')
        Add-Type -Path $sources -ReferencedAssemblies $references -ErrorAction Stop
    }finally{$env:TEMP=$previousTemp;$env:TMP=$previousTmp}
}
function Invoke-ProjectNativeTool {
    param([Parameter(Mandatory)][string]$Module,[string[]]$Arguments=@())
    Initialize-ProjectNativeTools
    $root=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
    $result=[Ifss.Native.Command]::Run($root,$Module,$Arguments)
    $global:LASTEXITCODE=$result.ExitCode
    if($result.Error){[Console]::Error.WriteLine($result.Error)}
    if($result.Output){Write-Output $result.Output}
}
