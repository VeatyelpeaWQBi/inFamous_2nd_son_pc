# SPDX-License-Identifier: GPL-2.0-or-later
# Explicit maintenance operation. Never called by build.bat, run.bat or test.bat.
[CmdletBinding()]
param()
Set-StrictMode -Version Latest
$ErrorActionPreference='Stop'
$ProgressPreference='SilentlyContinue'
[Net.ServicePointManager]::SecurityProtocol=[Net.SecurityProtocolType]::Tls12
Add-Type -AssemblyName System.IO.Compression.FileSystem
. (Join-Path $PSScriptRoot 'tools.ps1')
$root=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$lockPath=Join-Path $root 'dependencies.lock.json'
$dependencyLock=Read-JsonUtf8 $lockPath
$archiveRoot=Resolve-ProjectChild $root 'externals/.archives'
$null=[IO.Directory]::CreateDirectory($archiveRoot)
$visited=@{}
$headers=@{'User-Agent'='ifss-project-dependency-restore';'Accept'='application/vnd.github+json'}
function Save-Lock {Write-JsonUtf8 $lockPath $dependencyLock}
function Save-Download([string]$Url,[string]$Path) {
    Assert-ProjectPath $root $Path
    if(Test-Path -LiteralPath $Path -PathType Leaf){return}
    $pending=$Path+'.partial'
    for($attempt=1;$attempt -le 3;$attempt++){
        try{
            Invoke-WebRequest -UseBasicParsing -Uri $Url -OutFile $pending -Headers @{'User-Agent'='ifss-project-dependency-restore'} -TimeoutSec 240
            if([IO.FileInfo]::new($pending).Length -eq 0){throw 'Empty dependency response.'}
            [IO.File]::Move($pending,$Path);return
        }catch{
            if(Test-Path -LiteralPath $pending){Remove-Item -LiteralPath $pending -Force}
            if($attempt -eq 3){throw}
            Start-Sleep -Seconds 2
        }
    }
}
function Read-Modules([string]$Path) {
    $modules=@();$entry=$null
    foreach($line in [IO.File]::ReadAllLines($Path)){
        if($line -match '^\s*\[submodule\s+"[^"]+"\]\s*$'){
            if($entry){$modules+=,[pscustomobject]$entry};$entry=@{path='';url=''}
        }elseif($entry -and $line -match '^\s*(path|url)\s*=\s*(.+)$'){$entry[$Matches[1]]=$Matches[2].Trim()}
    }
    if($entry){$modules+=,[pscustomobject]$entry}
    return $modules
}
function Archive-Path([string]$Repository,[string]$Commit) {
    return Join-Path $archiveRoot ($Repository.Replace('/','__')+'__'+$Commit+'.zip')
}
function Extract-Source([string]$Archive,[string]$Destination) {
    Assert-ProjectPath $root $Destination
    $null=[IO.Directory]::CreateDirectory($Destination)
    $zip=[IO.Compression.ZipFile]::OpenRead($Archive)
    $links=@();$linkNotes=@()
    try{
        foreach($entry in $zip.Entries){
            $name=$entry.FullName.Replace('\','/')
            $slash=$name.IndexOf('/')
            if($slash -lt 0){throw 'Source archive has no containing directory.'}
            $relative=$name.Substring($slash+1)
            if(-not $relative -or $relative.EndsWith('/')){continue}
            if($relative -match '(^|/)\.git(/|$)|(^|/)\.\.(/|$)|:|^/'){throw ('Unsafe dependency archive path: '+$relative)}
            $target=[IO.Path]::GetFullPath((Join-Path $Destination $relative))
            Assert-ProjectPath $Destination $target
            $null=[IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($target))
            $mode=($entry.ExternalAttributes -shr 16) -band 61440
            if($mode -eq 40960){
                $reader=[IO.StreamReader]::new($entry.Open())
                try{$link=$reader.ReadToEnd()}finally{$reader.Dispose()}
                $links+=@{path=$target;target=$link};continue
            }
            $entryInput=$entry.Open()
            try{
                if(Test-Path -LiteralPath $target -PathType Leaf){
                    $digest=[Security.Cryptography.SHA256]::Create()
                    try{$expected=([BitConverter]::ToString($digest.ComputeHash($entryInput))).Replace('-','').ToLowerInvariant()}finally{$digest.Dispose()}
                    if((Get-FileHash -LiteralPath $target -Algorithm SHA256).Hash.ToLowerInvariant() -ne $expected){throw ('Existing dependency file differs; refusing overwrite: '+$target)}
                }else{
                    $output=[IO.File]::Open($target,[IO.FileMode]::CreateNew,[IO.FileAccess]::Write,[IO.FileShare]::None)
                    try{$entryInput.CopyTo($output)}finally{$output.Dispose()}
                }
            }finally{$entryInput.Dispose()}
        }
    }finally{$zip.Dispose()}
    # Materialize internal file links as ordinary files for portable Windows copies.
    while($links.Count){
        $pendingLinks=@();$madeProgress=$false
        foreach($link in $links){
            $resolved=[IO.Path]::GetFullPath((Join-Path ([IO.Path]::GetDirectoryName($link.path)) $link.target))
            Assert-ProjectPath $Destination $resolved
            if(Test-Path -LiteralPath $resolved -PathType Leaf){
                if(-not(Test-Path -LiteralPath $link.path)){[IO.File]::Copy($resolved,$link.path,$false)}
                $linkNotes+=@{path=(Get-RelativePath $root $link.path);target=$link.target;representation='copied-internal-file'}
                $madeProgress=$true
            }elseif((Test-Path -LiteralPath $resolved -PathType Container) -and
                     -not @($links|Where-Object{$_.path.StartsWith($resolved+'\',[StringComparison]::OrdinalIgnoreCase)}).Count){
                if($link.path.StartsWith($resolved+'\',[StringComparison]::OrdinalIgnoreCase)){throw 'Cyclic dependency directory link.'}
                Copy-ProjectTree $root $resolved $link.path;$madeProgress=$true
                $linkNotes+=@{path=(Get-RelativePath $root $link.path);target=$link.target;representation='copied-internal-directory'}
            }else{$pendingLinks+=,$link}
        }
        if(-not $madeProgress){
            # Git for Windows with core.symlinks=false preserves a dangling link's
            # blob as an ordinary text file. Preserve those original bytes too;
            # never create a filesystem link or invent a replacement target.
            foreach($link in $pendingLinks){
                if(-not(Test-Path -LiteralPath $link.path)){
                    [IO.File]::WriteAllText($link.path,$link.target,[Text.UTF8Encoding]::new($false))
                }
                $linkNotes+=@{path=(Get-RelativePath $root $link.path);target=$link.target;representation='upstream-dangling-link-blob'}
            }
            $pendingLinks=@()
        }
        $links=$pendingLinks
    }
    if($linkNotes.Count){Write-JsonUtf8 ($Archive+'.links.json') $linkNotes}
}
function Restore-Module($Module) {
    $path=[string]$Module.path;$commit=[string]$Module.commit;$url=[string]$Module.url
    if(-not $path.StartsWith('externals/',[StringComparison]::Ordinal) -or $path -match '(^|[\\/])\.\.([\\/]|$)'){
        throw 'Dependency module destination must stay under project externals.'
    }
    if($commit -notmatch '^[0-9a-f]{40}$'){throw ('Dependency is not pinned: '+$path)}
    $destination=Resolve-ProjectChild $root $path
    if($visited.ContainsKey($path)){return};$visited[$path]=$true
    if($url -eq 'https://android.googlesource.com/platform/external/aac'){
        $repository='aosp-mirror/platform_external_aac'
    }elseif($url -match '^https://github\.com/([^/]+/[^/]+?)(?:\.git)?/?$'){
        $repository=$Matches[1]
    }else{throw ('Unsupported dependency origin: '+$url)}
    $archive=Archive-Path $repository $commit
    $download='https://codeload.github.com/'+$repository+'/zip/'+$commit
    Write-Host ('Restore '+$path+' @ '+$commit.Substring(0,12))
    if($url -eq 'https://android.googlesource.com/platform/external/aac'){
        # This Android commit is absent from the GitHub mirror. Fetch the exact
        # object from its canonical origin; the repository cache stays local.
        $download=$url+'@'+$commit
        if(-not(Test-Path -LiteralPath $archive -PathType Leaf)){
            $git='C:\Program Files\Git\cmd\git.exe'
            if(-not(Test-Path -LiteralPath $git -PathType Leaf)){throw 'The installed system Git is missing.'}
            $metadata=Join-Path $archiveRoot ('aac-git-'+$commit)
            if(-not(Test-Path -LiteralPath $metadata)){
                & $git init --bare $metadata | ForEach-Object{Write-Host $_}
                if($LASTEXITCODE -ne 0){throw 'Cannot create the project-local source metadata cache.'}
            }
            & $git -c ('safe.directory='+$metadata.Replace('\','/')) ('--git-dir='+$metadata) fetch --depth=1 --no-tags $url $commit 2>&1 | ForEach-Object{Write-Host $_}
            if($LASTEXITCODE -ne 0){throw 'Cannot fetch the pinned Android AAC dependency.'}
            & $git -c ('safe.directory='+$metadata.Replace('\','/')) ('--git-dir='+$metadata) archive --format=zip --prefix=source/ ('--output='+$archive) $commit
            if($LASTEXITCODE -ne 0){throw 'Cannot archive the pinned Android AAC dependency.'}
        }
    }else{Save-Download $download $archive}
    $archiveHash=(Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash.ToLowerInvariant()
    if($Module.PSObject.Properties['archive_sha256'] -and $Module.archive_sha256 -ne $archiveHash){throw ('Dependency archive changed: '+$path)}
    Extract-Source $archive $destination
    $Module|Add-Member -Force NoteProperty archive_sha256 $archiveHash
    $Module|Add-Member -Force NoteProperty archive (Get-RelativePath $root $archive)
    $Module|Add-Member -Force NoteProperty download_url $download
    Save-Lock
    $modulesFile=Join-Path $destination '.gitmodules'
    if(Test-Path -LiteralPath $modulesFile -PathType Leaf){
        $children=@(Read-Modules $modulesFile)
        if($children.Count){
            $treePath=Join-Path $archiveRoot ($repository.Replace('/','__')+'__'+$commit+'.tree.json')
            if(-not(Test-Path -LiteralPath $treePath -PathType Leaf)){
                $tree=Invoke-RestMethod -Uri ('https://api.github.com/repos/'+$repository+'/git/trees/'+$commit+'?recursive=1') -Headers $headers -TimeoutSec 120
                Write-JsonUtf8 $treePath $tree
            }else{$tree=Read-JsonUtf8 $treePath}
            if($tree.truncated){throw ('Truncated dependency tree: '+$path)}
            foreach($child in $children){
                if(-not $child.path -or -not $child.url){throw ('Incomplete nested dependency definition: '+$path)}
                $gitlinks=@($tree.tree|Where-Object{$_.path -eq $child.path -and $_.mode -eq '160000'})
                if($gitlinks.Count -ne 1){throw ('Nested dependency has no pinned gitlink: '+$path+'/'+$child.path)}
                $childUrl=$child.url
                if($childUrl.StartsWith('.')){$childUrl=[Uri]::new([Uri]($url.TrimEnd('/')+'/'),$childUrl).AbsoluteUri}
                $childPath=$path+'/'+$child.path
                $known=@($dependencyLock.modules|Where-Object{$_.path -eq $childPath})
                if($known.Count){$nested=$known[0]}else{
                    $nested=[pscustomobject]@{path=$childPath;url=$childUrl;commit=$gitlinks[0].sha}
                    $dependencyLock.modules+=@($nested);Save-Lock
                }
                if($nested.commit -ne $gitlinks[0].sha -or $nested.url -ne $childUrl){throw ('Nested dependency lock mismatch: '+$childPath)}
                Restore-Module $nested
            }
        }
    }
}
function Restore-Support {
    foreach($file in $dependencyLock.support_files){
        $source=Resolve-ProjectChild $root $file.source
        $destination=Resolve-ProjectChild $root $file.path
        if((Get-FileHash -LiteralPath $source -Algorithm SHA256).Hash.ToLowerInvariant() -ne $file.sha256){throw ('Dependency bootstrap changed: '+$file.source)}
        $null=[IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($destination))
        if(-not(Test-Path -LiteralPath $destination)){[IO.File]::Copy($source,$destination,$false)}
        if((Get-FileHash -LiteralPath $destination -Algorithm SHA256).Hash.ToLowerInvariant() -ne $file.sha256){throw ('Dependency support file changed: '+$file.path)}
    }
}
function Restore-Binaries {
    foreach($binary in $dependencyLock.binaries){
        $archive=Resolve-ProjectChild $root $binary.archive
        Save-Download $binary.url $archive
        if((Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash.ToLowerInvariant() -ne $binary.archive_sha256){throw 'Prebuilt dependency archive hash mismatch.'}
        $zip=[IO.Compression.ZipFile]::OpenRead($archive)
        try{
            foreach($file in $binary.files){
                $destination=Resolve-ProjectChild $root $file.path
                $name=[IO.Path]::GetFileName($destination)
                $entries=@($zip.Entries|Where-Object{[IO.Path]::GetFileName($_.FullName) -eq $name})
                if($entries.Count -ne 1){throw ('Prebuilt dependency missing/ambiguous: '+$name)}
                $null=[IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($destination))
                if(-not(Test-Path -LiteralPath $destination)){
                    $entryInput=$entries[0].Open();$entryOutput=[IO.File]::Open($destination,[IO.FileMode]::CreateNew,[IO.FileAccess]::Write)
                    try{$entryInput.CopyTo($entryOutput)}finally{$entryOutput.Dispose();$entryInput.Dispose()}
                }
                if([IO.FileInfo]::new($destination).Length -ne $file.bytes -or
                   (Get-FileHash -LiteralPath $destination -Algorithm SHA256).Hash.ToLowerInvariant() -ne $file.sha256){throw ('Prebuilt dependency changed: '+$file.path)}
            }
        }finally{$zip.Dispose()}
        Write-JsonUtf8 (Resolve-ProjectChild $root 'externals/prebuilt/ffmpeg/manifest.json') $binary
    }
}
function Write-Inventory {
    $sourceRoot=Resolve-ProjectChild $root 'externals'
    $inventory=Join-Path $sourceRoot 'dependency-inventory.tsv'
    $files=[Collections.Generic.List[string]]::new()
    $directories=[Collections.Generic.Stack[string]]::new();$directories.Push($sourceRoot)
    while($directories.Count){
        foreach($entry in [IO.Directory]::EnumerateFileSystemEntries($directories.Pop())){
            if($entry -eq $archiveRoot -or $entry -eq $inventory){continue}
            if(([IO.File]::GetAttributes($entry) -band [IO.FileAttributes]::ReparsePoint) -ne 0){throw ('Dependency archive contains a filesystem link: '+$entry)}
            if(([IO.File]::GetAttributes($entry) -band [IO.FileAttributes]::Directory) -ne 0){$directories.Push($entry)}else{$files.Add($entry)}
        }
    }
    $files.Sort([StringComparer]::Ordinal)
    $pending=$inventory+'.'+[Guid]::NewGuid().ToString('N')+'.pending';$writer=$null;$bytes=0L
    try{
        $writer=[IO.StreamWriter]::new($pending,$false,[Text.UTF8Encoding]::new($false))
        $writer.WriteLine("sha256`tbytes`tpath")
        foreach($file in $files){
            $relative=Get-RelativePath $root $file
            if($relative -match '[\t\r\n]'){throw 'Unsupported dependency filename in inventory.'}
            $size=[IO.FileInfo]::new($file).Length;$bytes+=$size
            $hash=(Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash.ToLowerInvariant()
            $writer.WriteLine($hash+"`t"+$size.ToString([Globalization.CultureInfo]::InvariantCulture)+"`t"+$relative)
        }
        $writer.Dispose();$writer=$null
        Publish-ProjectFile $pending $inventory
    }finally{if($writer){$writer.Dispose()};if(Test-Path -LiteralPath $pending){Remove-Item -LiteralPath $pending -Force}}
    $dependencyLock|Add-Member -Force NoteProperty inventory ([pscustomobject]@{path=(Get-RelativePath $root $inventory)
        sha256=(Get-FileHash -LiteralPath $inventory -Algorithm SHA256).Hash.ToLowerInvariant();files=$files.Count;bytes=$bytes})
    $dependencyLock.status='complete';Save-Lock
    Write-Host ('Project-local dependency archive complete: '+$dependencyLock.modules.Count+' pinned modules, '+$files.Count+' files, '+[Math]::Round($bytes/1MB,1)+' MiB of sources/libraries.')
}
function Restore-ProjectDependencies {
$state=Resolve-ProjectChild $root 'state/runtime';$null=[IO.Directory]::CreateDirectory($state)
$suite=$null;$run=$null
try{
    $suite=[IO.File]::Open((Join-Path $state 'v31-suite.lock'),[IO.FileMode]::OpenOrCreate,[IO.FileAccess]::ReadWrite,[IO.FileShare]::None)
    $run=[IO.File]::Open((Join-Path $state 'run.lock'),[IO.FileMode]::OpenOrCreate,[IO.FileAccess]::ReadWrite,[IO.FileShare]::None)
    $dependencyLock.status='restoring';Save-Lock
    Restore-Support
    foreach($module in @($dependencyLock.modules)){Restore-Module $module}
    Restore-Binaries
    Write-Inventory
}finally{if($run){$run.Dispose()};if($suite){$suite.Dispose()}}
}
if($MyInvocation.InvocationName -ne '.') {Restore-ProjectDependencies}
