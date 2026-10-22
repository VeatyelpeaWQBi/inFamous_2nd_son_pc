# SPDX-License-Identifier: GPL-2.0-or-later
# BAT files use Windows PowerShell. Load its own built-in module explicitly:
# inherited PowerShell 7 module paths can otherwise shadow Get-FileHash.
Import-Module (Join-Path $PSHOME 'Modules/Microsoft.PowerShell.Utility/Microsoft.PowerShell.Utility.psd1') -ErrorAction Stop
Set-StrictMode -Version Latest
function Resolve-ProjectChild([string]$Root,[string]$Relative) {
    $base=[IO.Path]::GetFullPath($Root).TrimEnd('\')+'\'
    if([IO.Path]::IsPathRooted($Relative)){throw 'Expected a project-relative output path.'}
    $path=[IO.Path]::GetFullPath((Join-Path $base $Relative))
    if(-not $path.StartsWith($base,[StringComparison]::OrdinalIgnoreCase)){throw 'Output leaves the project.'}
    $walk=$path
    while($walk.Length -ge $base.TrimEnd('\').Length){
        if(Test-Path -LiteralPath $walk){
            if((Get-Item -LiteralPath $walk -Force).Attributes -band [IO.FileAttributes]::ReparsePoint){
                throw ('Output path contains a directory link: '+$walk)
            }
        }
        $parent=Split-Path -Parent $walk
        if(-not $parent -or $parent -eq $walk){break};$walk=$parent
    }
    return $path
}
function Read-JsonUtf8([string]$Path) {
    return [IO.File]::ReadAllText($Path,[Text.Encoding]::UTF8)|ConvertFrom-Json
}
function Write-JsonUtf8([string]$Path,$Value) {
    [IO.File]::WriteAllText($Path,($Value|ConvertTo-Json -Depth 16),[Text.UTF8Encoding]::new($false))
}
function Quote-WindowsArgument([string]$Value) {
    if($Value -notmatch '[\s"]' -and $Value.Length){return $Value}
    return '"'+([regex]::Replace([regex]::Replace($Value,'(\\*)"','$1$1\"'),'(\\+)$','$1$1'))+'"'
}
function Get-SourceFingerprint([string]$Root) {
    $files=@()
    foreach($directory in @('src','cmake','scripts')){
        $files+=Get-ChildItem -LiteralPath (Join-Path $Root $directory) -File -Recurse
    }
    $files+=Get-Item -LiteralPath (Join-Path $Root 'CMakeLists.txt'),(Join-Path $Root 'project.json'),(Join-Path $Root 'inFAMOUS Second Son.ico')
    $records=foreach($file in $files|Sort-Object FullName){
        $relative=$file.FullName.Substring($Root.TrimEnd('\').Length+1).Replace('\','/')
        $relative+':'+(Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash
    }
    $hash=[Security.Cryptography.SHA256]::Create()
    try{return ([BitConverter]::ToString($hash.ComputeHash([Text.Encoding]::UTF8.GetBytes(($records -join [char]10))))).Replace('-','').ToLowerInvariant()}
    finally{$hash.Dispose()}
}
