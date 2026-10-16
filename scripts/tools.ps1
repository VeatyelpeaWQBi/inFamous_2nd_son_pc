Set-StrictMode -Version Latest
function Resolve-InstalledPython {
    $candidates = [Collections.Generic.List[string]]::new()
    $command = Get-Command python.exe -CommandType Application -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($command) { $candidates.Add($command.Source) }
    foreach ($scope in @('User','Machine')) {
        foreach ($directory in ([Environment]::GetEnvironmentVariable('Path',$scope) -split ';')) {
            if ($directory) { $candidates.Add((Join-Path ([Environment]::ExpandEnvironmentVariables($directory.Trim().Trim('"'))) 'python.exe')) }
        }
    }
    $candidates.Add((Join-Path $env:LOCALAPPDATA 'Programs\Python\Python312\python.exe'))
    foreach ($candidate in $candidates) {
        if ($candidate -match 'WindowsApps|[\\/]\.codex[\\/]|[\\/]msys64[\\/]') { continue }
        if (Test-Path -LiteralPath $candidate -PathType Leaf) { return (Resolve-Path -LiteralPath $candidate).Path }
    }
    throw 'Cannot locate the installed Windows Python. Configure its existing PATH; no runtime is installed automatically.'
}
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
function Read-JsonUtf8([string]$Path) {
    # A reader keeps one complete snapshot open while allowing atomic replacement.
    $stream=[IO.File]::Open($Path,[IO.FileMode]::Open,[IO.FileAccess]::Read,([IO.FileShare]::ReadWrite -bor [IO.FileShare]::Delete))
    $reader=$null
    try{
        $reader=[IO.StreamReader]::new($stream,[Text.UTF8Encoding]::new($false,$true),$true)
        $text=$reader.ReadToEnd()
    }finally{
        if($reader){$reader.Dispose()}else{$stream.Dispose()}
    }
    try{return (ConvertFrom-Json -InputObject $text -ErrorAction Stop)}
    catch{throw [IO.InvalidDataException]::new(('Invalid JSON in '+$Path+'.'),$_.Exception)}
}
function Get-ProjectProcessIdentity($Process) {
    if(-not ('Ifss.ProcessIdentity' -as [type])){
        $projectRoot=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
        $temporary=Resolve-ProjectChild $projectRoot 'state/runtime/native-helper-temp'
        $null=[IO.Directory]::CreateDirectory($temporary)
        $previousTemp=$env:TEMP;$previousTmp=$env:TMP
        try{
            $env:TEMP=$temporary;$env:TMP=$temporary
            Add-Type -Path (Join-Path $PSScriptRoot 'process_identity.cs') -ErrorAction Stop
        }finally{$env:TEMP=$previousTemp;$env:TMP=$previousTmp}
    }
    # Query path and creation time through one minimal-rights kernel handle.
    # Matching the creation time rejects PID reuse without full-access/module enumeration.
    return [Ifss.ProcessIdentity]::ReadLimited($Process.Id)
}
function Test-ProjectProcessIdentity($Process,[string]$ExpectedPath,[string]$Started) {
    $identity=Get-ProjectProcessIdentity $Process
    $created=[DateTime]::Parse($Started,[Globalization.CultureInfo]::InvariantCulture,[Globalization.DateTimeStyles]::RoundtripKind).ToFileTimeUtc()
    return ($identity.Path -eq $ExpectedPath -and $identity.CreationFileTime -eq $created)
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
    $projectRoot=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
    Assert-ProjectPath $projectRoot $ConfigPath
    $config=Read-JsonUtf8 $ConfigPath
    $general=$config.General
    $properties=@(@('addon_install_dir','font_dir','home_dir','sys_modules_dir')|
        ForEach-Object{$general.PSObject.Properties[$_]}|Where-Object{$_})
    if($general.PSObject.Properties['install_dirs']){
        $properties+=@($general.install_dirs|ForEach-Object{$_.PSObject.Properties['path']})
    }
    foreach($property in $properties){
        if(-not $property.Value){continue}
        $full=if([IO.Path]::IsPathRooted([string]$property.Value)){[IO.Path]::GetFullPath([string]$property.Value)}else{Resolve-ProjectPath $FromBase ([string]$property.Value)}
        Assert-ProjectPath $projectRoot $full
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
    # Existing engine/file readers may briefly deny deletion. Never remove the old file.
    for($attempt=0;;$attempt++){
        try{
            if(Test-Path -LiteralPath $Destination){[IO.File]::Replace($Pending,$Destination,[System.Management.Automation.Language.NullString]::Value)}
            else{[IO.File]::Move($Pending,$Destination)}
            return
        }catch{
            $cause=$_.Exception
            while($cause.InnerException){$cause=$cause.InnerException}
            $code=$cause.HResult -band 0xffff
            if($cause -isnot [IO.IOException] -or $code -notin @(32,33,1175) -or $attempt -ge 6){throw}
            Start-Sleep -Milliseconds ([int][Math]::Min(50*[Math]::Pow(2,$attempt),400))
        }
    }
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
        $Session=(Read-JsonUtf8 $activePath).session
    }
    $path=Resolve-ProjectChild $Root $Session
    $allowed=Resolve-ProjectChild $Root 'state/sessions'
    if(-not $path.StartsWith($allowed+'\',[StringComparison]::OrdinalIgnoreCase)){throw 'Untrusted session path.'}
    $launchPath=Resolve-ProjectChild $Root ($Session+'/launch.json')
    $launch=Read-JsonUtf8 $launchPath
    $manifest=Read-JsonUtf8 (Join-Path $Root 'project.json')
    if($launch.schema -ne 3 -or $launch.path_base -ne 'project' -or
       (Resolve-ProjectPath $Root $launch.session) -ne $path -or
       (Resolve-ProjectChild $Root $launch.runtime) -ne (Resolve-ProjectChild $Root $manifest.runtime_path) -or
       $launch.runtime_sha256 -ne $manifest.runtime_sha256){throw 'Session does not match the pinned project runtime.'}
    return [pscustomobject]@{Path=$path;Launch=$launch}
}
