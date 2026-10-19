Set-StrictMode -Version Latest
function Write-JsonUtf8 {
    param([string]$Path, $Value)
    $root=[IO.Path]::GetFullPath((Join-Path ([IO.Path]::GetDirectoryName($MyInvocation.MyCommand.ScriptBlock.File)) '..'))
    Assert-ProjectPath $root $Path
    $pending=$Path+'.'+[Guid]::NewGuid().ToString('N')+'.pending'
    try{
        [IO.File]::WriteAllText($pending,($Value|ConvertTo-Json -Depth 18),[Text.UTF8Encoding]::new($false))
        Publish-ProjectFile $pending $Path
    }finally{
        if(Test-Path -LiteralPath $pending){Remove-Item -LiteralPath $pending -Force}
    }
}
function Quote-WindowsArgument([string]$Value) {
    return '"' + (($Value -replace '(\\*)"', '$1$1\"') -replace '(\\+)$', '$1$1') + '"'
}
function Resolve-ProjectChild([string]$Root,[string]$Relative) {
    $result=Resolve-ProjectPath $Root $Relative
    Assert-ProjectPath $Root $result
    return $result
}

function Ensure-ProjectQuitMenu([string]$UserRoot) {
    $directory=Join-Path $UserRoot 'input_config'
    $null=[IO.Directory]::CreateDirectory($directory)
    $path=Join-Path $directory 'global.ini'
    $content=if(Test-Path -LiteralPath $path){[IO.File]::ReadAllText($path)}else{''}
    if($content -notmatch '(?im)^\s*hotkey_quit\s*=\s*f1\s*(?:#.*)?$'){
        [IO.File]::AppendAllText($path,"`r`n# Project exit menu (F1); previous bindings remain available.`r`nhotkey_quit = f1`r`n",[Text.UTF8Encoding]::new($false))
    }
}

function Resolve-ProjectPath([string]$Root,[string]$Relative) {
    if([IO.Path]::IsPathRooted($Relative)){throw 'Stored paths must be relative to the project directory.'}
    return [IO.Path]::GetFullPath((Join-Path $Root $Relative))
}
function Get-RelativePath([string]$Base,[string]$Path) {
    $fullBase=[IO.Path]::GetFullPath($Base).TrimEnd('\')+'\'
    $fullPath=[IO.Path]::GetFullPath($Path)
    if([IO.Path]::GetPathRoot($fullBase) -ne [IO.Path]::GetPathRoot($fullPath)){throw 'A relative path cannot cross drives.'}
    $baseUri=[Uri]::new($fullBase)
    return [Uri]::UnescapeDataString($baseUri.MakeRelativeUri([Uri]::new($fullPath)).ToString())
}
function Convert-ProjectConfigPaths([string]$ConfigPath,[string]$FromBase,[string]$ToBase) {
    $config=Get-Content -LiteralPath $ConfigPath -Raw -Encoding UTF8|ConvertFrom-Json
    $general=$config.General
    $properties=@(@('addon_install_dir','font_dir','home_dir','sys_modules_dir')|
        ForEach-Object{$general.PSObject.Properties[$_]}|Where-Object{$_})
    if($general.PSObject.Properties['install_dirs']){
        $properties+=@($general.install_dirs|ForEach-Object{$_.PSObject.Properties['path']})
    }
    foreach($property in $properties){
        if(-not $property.Value){continue}
        $full=if([IO.Path]::IsPathRooted([string]$property.Value)){[IO.Path]::GetFullPath([string]$property.Value)}else{Resolve-ProjectPath $FromBase ([string]$property.Value)}
        $property.Value=Get-RelativePath $ToBase $full
    }
    Write-JsonUtf8 $ConfigPath $config
}

function Assert-ProjectPath([string]$Root,[string]$Path) {
    $base=[IO.Path]::GetFullPath($Root).TrimEnd('\')
    $full=[IO.Path]::GetFullPath($Path)
    if(-not $full.StartsWith($base+'\',[StringComparison]::OrdinalIgnoreCase)){throw 'Path escaped the project.'}
    for($entry=$full;$entry -ne $base;$entry=[IO.Path]::GetDirectoryName($entry)){
        $item=Get-Item -LiteralPath $entry -Force -ErrorAction SilentlyContinue
        if($item -and ($item.Attributes -band [IO.FileAttributes]::ReparsePoint)){throw 'Project data path contains a link.'}
    }
}
function Publish-ProjectFile([string]$Pending,[string]$Destination) {
    if(Test-Path -LiteralPath $Destination){[IO.File]::Replace($Pending,$Destination,[System.Management.Automation.Language.NullString]::Value)}
    else{[IO.File]::Move($Pending,$Destination)}
}
function Copy-ProjectTree([string]$Root,[string]$Source,[string]$Destination) {
    Assert-ProjectPath $Root $Source
    Assert-ProjectPath $Root $Destination
    $item=Get-Item -LiteralPath $Source -Force
    if($item.PSIsContainer){
        $null=[IO.Directory]::CreateDirectory($Destination)
        foreach($child in Get-ChildItem -LiteralPath $Source -Force){
            Copy-ProjectTree $Root $child.FullName (Join-Path $Destination $child.Name)
        }
    }else{
        $pending=$Destination+'.'+[Guid]::NewGuid().ToString('N')+'.pending'
        try{
            [IO.File]::Copy($Source,$pending,$false)
            Publish-ProjectFile $pending $Destination
        }finally{
            if(Test-Path -LiteralPath $pending){Remove-Item -LiteralPath $pending -Force}
        }
    }
}
function Read-ProjectSession([string]$Root,[string]$Session) {
    if(-not $Session){
        $activePath=Resolve-ProjectChild $Root 'state/active.json'
        if(-not (Test-Path -LiteralPath $activePath)){throw 'No project session is recorded.'}
        $Session=(Get-Content -LiteralPath $activePath -Raw -Encoding UTF8|ConvertFrom-Json).session
    }
    $path=Resolve-ProjectChild $Root $Session
    $allowed=Resolve-ProjectChild $Root 'state/sessions'
    if(-not $path.StartsWith($allowed+'\',[StringComparison]::OrdinalIgnoreCase)){throw 'Untrusted session path.'}
    $launchPath=Resolve-ProjectChild $Root ($Session+'/launch.json')
    $launch=Get-Content -LiteralPath $launchPath -Raw -Encoding UTF8|ConvertFrom-Json
    $manifest=Get-Content -LiteralPath (Join-Path $Root 'project.json') -Raw -Encoding UTF8|ConvertFrom-Json
    if($launch.schema -ne 3 -or $launch.path_base -ne 'project' -or
       (Resolve-ProjectPath $Root $launch.session) -ne $path -or
       (Resolve-ProjectPath $Root $launch.runtime) -ne (Resolve-ProjectPath $Root $manifest.runtime_path) -or
       $launch.runtime_sha256 -ne $manifest.runtime_sha256){throw 'Session does not match the pinned project runtime.'}
    return [pscustomobject]@{Path=$path;Launch=$launch}
}
. (Join-Path $PSScriptRoot 'native_tools.ps1')
