# SPDX-License-Identifier: GPL-2.0-or-later
function Get-ReleaseRuntimeFiles {
    param([string]$Root,[string]$Executable,[string]$Inspector,[switch]$Portable)
    if(-not(Test-Path -LiteralPath $Inspector -PathType Leaf)){
        throw 'The installed LLVM llvm-readobj.exe is needed to inspect DLL imports; nothing is installed.'
    }
    $project=Read-JsonUtf8 (Join-Path $Root 'project.json')
    $directories=@((Split-Path -Parent $Executable))
    $redist=@()
    foreach($base in @([Environment]::GetFolderPath('ProgramFilesX86'),$env:ProgramFiles)|Select-Object -Unique){
        $pattern=Join-Path $base 'Microsoft Visual Studio/*/*/VC/Redist/MSVC/*/x64/Microsoft.VC*.CRT'
        $redist+=@(Get-Item -Path $pattern -ErrorAction SilentlyContinue|Sort-Object FullName -Descending|ForEach-Object FullName)
    }
    $directories+=$redist
    $local=Get-ProjectDependencyPaths $Root $project
    foreach($external in @($local.prebuilt,$local.sources)){
        if(Test-Path -LiteralPath $external){
            $directories+=@(Get-ChildItem -LiteralPath $external -Filter *.dll -File -Recurse|
                Where-Object { -not($_.Attributes -band [IO.FileAttributes]::ReparsePoint) }|
                ForEach-Object DirectoryName|Sort-Object -Unique)
        }
    }
    # Project DLLs come only from project outputs/archive. The installed Visual
    # Studio redist above and Windows system components remain prerequisites.
    $directories=@($directories|Select-Object -Unique)
    $system=Join-Path $env:WINDIR 'System32'
    $queue=[Collections.Generic.Queue[string]]::new()
    $queue.Enqueue($Executable)
    $seen=[Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
    $files=[Collections.Generic.List[object]]::new()
    while($queue.Count){
        $binary=$queue.Dequeue()
        $text=Invoke-ProjectBuildProcess $Inspector @('--file-headers','--coff-imports',$binary) $Root @{} -Capture
        if($text -notmatch 'IMAGE_FILE_MACHINE_AMD64'){throw ('Not a Windows x64 image: '+$binary)}
        foreach($line in ($text -split [char]10)){
            if($line -notmatch '^\s*Name:\s*(\S+\.dll)\s*$'){continue}
            $name=$Matches[1]
            if([IO.Path]::GetFileName($name) -ne $name){throw 'Invalid DLL import name.'}
            if(-not $seen.Add($name)){continue}
            if($name -match '^(api-ms-|ext-ms-)'){continue}
            $isVC=$name -match '^(msvcp|vcruntime|concrt|vcomp|vcamp|atl)[0-9].*\.dll$'
            $osFile=Join-Path $system $name
            if(-not $isVC -and (Test-Path -LiteralPath $osFile -PathType Leaf)){
                $company=[Diagnostics.FileVersionInfo]::GetVersionInfo($osFile).CompanyName
                if($company -match 'Microsoft' -or $name -ieq 'vulkan-1.dll'){continue}
            }
            $found=$null
            $candidates=if($isVC){@($redist)+$directories}else{$directories}
            foreach($directory in $candidates){
                $candidate=Join-Path $directory $name
                if(Test-Path -LiteralPath $candidate -PathType Leaf){$found=[IO.Path]::GetFullPath($candidate);break}
            }
            if(-not $found -and -not $Portable -and (Test-Path -LiteralPath $osFile -PathType Leaf)){$found=$osFile}
            if(-not $found){throw ('Cannot locate redistributable runtime '+$name+'. Reuse the existing runtime installation; no download is performed.')}
            if($Portable -and $found.StartsWith($env:WINDIR.TrimEnd('\')+'\',[StringComparison]::OrdinalIgnoreCase)){
                throw ('Portable packages cannot copy Windows components: '+$name)
            }
            if($name -match '^(msvcp|vcruntime|concrt|vcomp|vcamp)[0-9].*d\.dll$' -or $name -ieq 'ucrtbased.dll'){throw 'Debug CRT cannot be included in a Release package.'}
            $files.Add([pscustomobject]@{name=$name;source=$found;sha256=(Get-FileHash -LiteralPath $found -Algorithm SHA256).Hash.ToLowerInvariant()})
            $queue.Enqueue($found)
        }
    }
    return $files.ToArray()
}
