# SPDX-License-Identifier: GPL-2.0-or-later
[CmdletBinding()]
param([ValidateSet('local','portable')][string]$Variant='local')
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot 'tools.ps1')
. (Join-Path $PSScriptRoot 'build_tools.ps1')
. (Join-Path $PSScriptRoot 'runtime_dependencies.ps1')
$root=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$lock=$null
try{
    $receiptPath=Resolve-ProjectChild $root 'Build/Release/build-receipt.json'
    $lockPath=Resolve-ProjectChild $root 'state/runtime/run.lock'
    $lock=[IO.File]::Open($lockPath,[IO.FileMode]::OpenOrCreate,[IO.FileAccess]::ReadWrite,[IO.FileShare]::None)
    $receipt=Read-JsonUtf8 $receiptPath
    if($receipt.configuration -ne 'Release' -or $receipt.source_sha256 -ne (Get-SourceFingerprint $root)){
        throw 'The Release build does not match current sources. Run build.bat first.'
    }
    $exe=Resolve-ProjectChild $root $receipt.runtime
    if($receipt.runtime_sha256 -ne (Get-FileHash -LiteralPath $exe -Algorithm SHA256).Hash.ToLowerInvariant()){
        throw 'Executable hash does not match its build receipt.'
    }
    $dlls=@(Get-ReleaseRuntimeFiles $root $exe $receipt.inspector -Portable:($Variant -eq 'portable'))
    $stamp=[DateTime]::Now.ToString('yyyyMMdd-HHmmss')+'-'+[Guid]::NewGuid().ToString('N').Substring(0,6)
    $relative='out/release/'+$Variant+'-'+$stamp
    $destination=Resolve-ProjectChild $root $relative
    if(Test-Path -LiteralPath $destination){throw 'Release destination already exists.'}
    $null=[IO.Directory]::CreateDirectory($destination)
    Copy-Item -LiteralPath $exe -Destination (Join-Path $destination 'inFAMOUS Second Son.exe')
    foreach($name in @('launcher.json','run.bat','LICENSE','LICENSES','README.md')){
        Copy-Item -LiteralPath (Join-Path $root $name) -Destination $destination -Recurse
    }
    $null=[IO.Directory]::CreateDirectory((Join-Path $destination 'profiles'))
    Copy-Item -LiteralPath (Join-Path $root 'profiles/CUSA00309.xml') -Destination (Join-Path $destination 'profiles/CUSA00309.xml')
    $user=Join-Path $destination 'state/play/user'
    $null=[IO.Directory]::CreateDirectory($user)
    Copy-Item -LiteralPath (Join-Path $root 'package/user-files/config.json') -Destination (Join-Path $user 'config.json')
    $null=[IO.Directory]::CreateDirectory((Join-Path $destination 'game'))
    [IO.File]::WriteAllText((Join-Path $destination 'game/README.txt'),
        'Place CUSA00309 01.07 here as CUSA00309/eboot.bin and its resources, or set game_path in launcher.json.',
        [Text.UTF8Encoding]::new($false))
    $project=Read-JsonUtf8 (Join-Path $root 'project.json')
    Write-JsonUtf8 (Join-Path $destination 'project.json') ([ordered]@{
        schema=1;name=$project.name;phase='release-v2';platform='windows-x86_64';variant=$Variant
    })
    if($Variant -eq 'portable'){
        foreach($dll in $dlls){Copy-Item -LiteralPath $dll.source -Destination (Join-Path $destination $dll.name)}
    }
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
    $requirements=@($dlls|ForEach-Object {[ordered]@{name=$_.name;sha256=$_.sha256;bundled=($Variant -eq 'portable')}})
    Write-JsonUtf8 (Join-Path $destination 'release-manifest.json') ([ordered]@{
        schema=1;variant=$Variant;runtime_sha256=$receipt.runtime_sha256;source_sha256=$receipt.source_sha256
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
    if($Variant -eq 'portable'){
        foreach($dll in $dlls){
            if((Get-FileHash -LiteralPath (Join-Path $destination $dll.name) -Algorithm SHA256).Hash.ToLowerInvariant() -ne $dll.sha256){throw ('Staged DLL verification failed: '+$dll.name)}
        }
    }
    $zip=$destination+'.zip'
    Compress-Archive -LiteralPath $destination -DestinationPath $zip -CompressionLevel Optimal
    Write-Host ('Release directory: '+$relative)
    Write-Host ('Release archive: '+$relative+'.zip')
    Write-Host 'No game files, saves, private configuration or captures were copied.'
}catch{Write-Host ('Release failed: '+$_.Exception.Message) -ForegroundColor Red;exit 1}
finally{if($lock){$lock.Dispose()}}
