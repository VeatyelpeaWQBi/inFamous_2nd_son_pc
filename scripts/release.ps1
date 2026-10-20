# SPDX-License-Identifier: GPL-2.0-or-later
[CmdletBinding()]
param([ValidateSet('lite','portable')][string]$Variant='lite')
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot 'tools.ps1')
. (Join-Path $PSScriptRoot 'build_tools.ps1')
. (Join-Path $PSScriptRoot 'runtime_dependencies.ps1')
$root=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$lock=$null;$phase='Build verification'
try{
    Write-Host ('Release packaging ('+$Variant+')')
    Write-Host '[1/7] Verifying the build receipt, source and executable...'
    $receiptPath=Resolve-ProjectChild $root 'Build/Release/build-receipt.json'
    $lockPath=Resolve-ProjectChild $root 'state/runtime/run.lock'
    $lock=[IO.File]::Open($lockPath,[IO.FileMode]::OpenOrCreate,[IO.FileAccess]::ReadWrite,[IO.FileShare]::None)
    $receipt=Read-JsonUtf8 $receiptPath
    if($receipt.configuration -ne 'Release' -or $receipt.source_sha256 -ne (Get-SourceFingerprint $root)){
        throw 'The Release build does not match current sources. Run build-lite.bat first.'
    }
    $exe=Resolve-ProjectChild $root $receipt.runtime
    if($receipt.runtime_sha256 -ne (Get-FileHash -LiteralPath $exe -Algorithm SHA256).Hash.ToLowerInvariant()){
        throw 'Executable hash does not match its build receipt.'
    }
    if(-not $receipt.PSObject.Properties['launcher'] -or -not $receipt.PSObject.Properties['launcher_sha256']){
        throw 'The build receipt has no native menu. Run build-lite.bat first.'
    }
    $menu=Resolve-ProjectChild $root $receipt.launcher
    if(-not(Test-Path -LiteralPath $menu -PathType Leaf) -or
       $receipt.launcher_sha256 -ne (Get-FileHash -LiteralPath $menu -Algorithm SHA256).Hash.ToLowerInvariant()){
        throw 'Launch menu hash does not match its build receipt.'
    }
    $phase='Runtime dependencies'
    Write-Host '[2/7] Inspecting runtime DLL dependencies...'
    $dlls=@(Get-ReleaseRuntimeFiles $root $exe $receipt.inspector -Portable:($Variant -eq 'portable'))
    $phase='Compatibility profiles'
    Write-Host '[3/7] Checking game compatibility profiles...'
    $compatibility=Read-JsonUtf8 (Join-Path $root 'profiles/second-son-builds.json')
    if($compatibility.schema -ne 1 -or $compatibility.game -cne 'inFAMOUS Second Son' -or $compatibility.builds -isnot [Array]){
        throw 'Invalid Second Son compatibility manifest.'
    }
    $allowedTitles=@('CUSA00004','CUSA00223','CUSA00305','CUSA00309')
    $profileFiles=@('second-son-builds.json')
    foreach($entry in @($compatibility.builds)){
        if(@($entry.title_ids).Count -eq 0 -or $entry.version -cnotmatch '^[0-9]{2}\.[0-9]{2}$' -or
           $entry.sha256 -cnotmatch '^[0-9a-f]{64}$' -or $entry.patch_file -cnotmatch '^[A-Za-z0-9_.-]+\.xml$' -or
           $entry.patch_file.Contains('..')){
            throw 'Invalid Second Son build entry.'
        }
        foreach($id in @($entry.title_ids)){
            if($allowedTitles -cnotcontains $id){throw 'Compatibility manifest includes a different game.'}
        }
        if($entry.PSObject.Properties.Name -contains 'legacy_cache_title'){
            if($entry.legacy_cache_title -and @($entry.title_ids) -cnotcontains $entry.legacy_cache_title){
                throw 'Legacy cache title is not declared by its fingerprint-bound build.'
            }
        }
        $profileFiles+=$entry.patch_file
    }
    $profileFiles=@($profileFiles|Sort-Object -Unique)
    foreach($profile in $profileFiles){
        if(-not(Test-Path -LiteralPath (Join-Path $root ('profiles/'+$profile)) -PathType Leaf)){
            throw ('Missing compatibility profile: '+$profile)
        }
    }
    $phase='Package files'
    Write-Host '[4/7] Copying the executable, clean configuration and package files...'
    $stamp=[DateTime]::Now.ToString('yyyyMMdd-HHmmss')+'-'+[Guid]::NewGuid().ToString('N').Substring(0,6)
    $relative='out/release/'+$Variant+'-'+$stamp
    $destination=Resolve-ProjectChild $root $relative
    if(Test-Path -LiteralPath $destination){throw 'Release destination already exists.'}
    $null=[IO.Directory]::CreateDirectory($destination)
    Copy-Item -LiteralPath $exe -Destination (Join-Path $destination 'inFAMOUS Second Son.exe')
    foreach($name in @('inFAMOUS Second Son.ico','LICENSE','LICENSES','README.md')){
        Copy-Item -LiteralPath (Join-Path $root $name) -Destination $destination -Recurse
    }
    $null=[IO.Directory]::CreateDirectory((Join-Path $destination 'support'))
    Copy-Item -LiteralPath $menu -Destination (Join-Path $destination 'support/Ifss.Launcher.exe')
    Write-JsonUtf8 (Join-Path $destination 'launcher.json') ([ordered]@{
        game_path='game';mode='pro4k';display='borderless';xbox_prompts=$true;touchpad_chords=$true
    })
    $null=[IO.Directory]::CreateDirectory((Join-Path $destination 'profiles'))
    foreach($profile in $profileFiles){
        Copy-Item -LiteralPath (Join-Path $root ('profiles/'+$profile)) -Destination (Join-Path $destination ('profiles/'+$profile))
    }
    $user=Join-Path $destination 'state/play/user'
    $null=[IO.Directory]::CreateDirectory($user)
    Copy-Item -LiteralPath (Join-Path $root 'package/user-files/config.json') -Destination (Join-Path $user 'config.json')
    $null=[IO.Directory]::CreateDirectory((Join-Path $destination 'game'))
    [IO.File]::WriteAllText((Join-Path $destination 'game/README.txt'),
        'Place inFAMOUS Second Son here as eboot.bin plus its resources, or under a CUSA00004/CUSA00223/CUSA00305/CUSA00309 folder. Set game_path in launcher.json when more than one copy is present.',
        [Text.UTF8Encoding]::new($false))
    $project=Read-JsonUtf8 (Join-Path $root 'project.json')
    Write-JsonUtf8 (Join-Path $destination 'project.json') ([ordered]@{
        schema=1;name=$project.name;phase='release-v2';platform='windows-x86_64';variant=$Variant;compatibility_manifest='profiles/second-son-builds.json'
    })
    if($Variant -eq 'portable'){
        foreach($dll in $dlls){Copy-Item -LiteralPath $dll.source -Destination (Join-Path $destination $dll.name)}
    }
    $phase='Third-party licenses'
    Write-Host '[5/7] Collecting third-party license notices...'
    # Static dependencies also retain their notices. Never copy source trees or .git directories.
    $external=[IO.Path]::GetFullPath((Join-Path $root $project.dependencies_root))
    $licenses=Join-Path $destination 'third-party-licenses'
    foreach($file in Get-ChildItem -LiteralPath $external -File -Recurse|
        Where-Object {($_.Name -match '^(LICENSE|COPYING|NOTICE)([._-].*)?$' -or $_.FullName -match '[\\/]LICENSES?[\\/]') -and $_.FullName -notmatch '[\\/]\.git[\\/]'}){
        $relativeLicense=$file.FullName.Substring($external.TrimEnd('\').Length+1)
        $target=Join-Path $licenses $relativeLicense
        $null=[IO.Directory]::CreateDirectory((Split-Path -Parent $target))
        Copy-Item -LiteralPath $file.FullName -Destination $target
    }
    $phase='Package verification'
    Write-Host '[6/7] Writing manifests and verifying package files...'
    $requirements=@($dlls|ForEach-Object {[ordered]@{name=$_.name;sha256=$_.sha256;bundled=($Variant -eq 'portable')}})
    Write-JsonUtf8 (Join-Path $destination 'release-manifest.json') ([ordered]@{
        schema=1;variant=$Variant;runtime_sha256=$receipt.runtime_sha256;source_sha256=$receipt.source_sha256
        launcher_sha256=$receipt.launcher_sha256
        managed_runtime='Windows .NET Framework 4.8 (system component, not bundled)'
        runtime_dependencies=$requirements;debug_collectors=$false;game_included=$false
        saves_included=$false;created_at_utc=[DateTime]::UtcNow.ToString('o')
    })
    # A complete file manifest makes missing/corrupted package files detectable.
    $manifest=@(Get-ChildItem -LiteralPath $destination -File -Recurse|Sort-Object FullName|ForEach-Object {
        [ordered]@{path=$_.FullName.Substring($destination.Length+1).Replace('\','/');sha256=(Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant()}
    })
    Write-JsonUtf8 (Join-Path $destination 'files.json') $manifest
    if((Get-SourceFingerprint $root) -ne $receipt.source_sha256){throw 'Source changed while packaging.'}
    if((Get-FileHash -LiteralPath (Join-Path $destination 'inFAMOUS Second Son.exe') -Algorithm SHA256).Hash.ToLowerInvariant() -ne $receipt.runtime_sha256){throw 'Staged executable verification failed.'}
    if((Get-FileHash -LiteralPath (Join-Path $destination 'support/Ifss.Launcher.exe') -Algorithm SHA256).Hash.ToLowerInvariant() -ne $receipt.launcher_sha256){throw 'Staged launch menu verification failed.'}
    if($Variant -eq 'portable'){
        foreach($dll in $dlls){
            if((Get-FileHash -LiteralPath (Join-Path $destination $dll.name) -Algorithm SHA256).Hash.ToLowerInvariant() -ne $dll.sha256){throw ('Staged DLL verification failed: '+$dll.name)}
        }
    }
    $zip=$destination+'.zip'
    $phase='ZIP compression'
    Write-Host ('[7/7] Compressing '+$relative+'.zip; this can take several minutes...')
    Compress-Archive -LiteralPath $destination -DestinationPath $zip -CompressionLevel Optimal
    Write-Host ('Release directory: '+$relative)
    Write-Host ('Release archive: '+$relative+'.zip')
    Write-Host 'No game files, saves, private configuration or captures were copied.'
}catch{Write-Host ('Release failed ['+$phase+']: '+$_.Exception.Message) -ForegroundColor Red;exit 1}
finally{if($lock){$lock.Dispose()}}
