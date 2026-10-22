# SPDX-License-Identifier: GPL-2.0-or-later
# Isolated save-selection fixtures only; never launch a game or collector.
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot '../../scripts/tools.ps1')
. (Join-Path $PSScriptRoot '../../scripts/diagnostic_retention.ps1')
$projectRoot=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
$fixture=Resolve-ProjectChild $projectRoot ('state/offline-tests/save-selection-'+[Guid]::NewGuid().ToString('N'))
$null=[IO.Directory]::CreateDirectory($fixture)
Write-JsonUtf8 (Join-Path $fixture 'project.json') @{
    runtime_path='Build/current.exe';runtime_sha256='current-build';game_sha256='same-game';title='CUSA00309'
}
function New-SaveFixture([int]$Index,$Overrides=@{},[bool]$WithSave=$true) {
    $relative='state/sessions/'+('20260101-{0:D6}-001-pro4k-{0:x8}' -f $Index)
    $path=Resolve-ProjectChild $fixture $relative
    $null=[IO.Directory]::CreateDirectory($path)
    $launch=@{
        schema=3;path_base='project';session=$relative;runtime='Build/older.exe';runtime_sha256='older-build'
        game_sha256='same-game';complete=$true;game_launched=$true;exit_code=0
        diagnostic=$null;benchmark=$false;debug=$false;timing_profile=$null
        pid=2147483647;process_started='2026-01-01T00:00:00Z'
    }
    foreach($key in $Overrides.Keys){$launch[$key]=$Overrides[$key]}
    Write-JsonUtf8 (Join-Path $path 'launch.json') $launch
    if($WithSave){
        $slot=Join-Path $path 'home/1000/savedata/CUSA00309/infamousssAuto0'
        $null=[IO.Directory]::CreateDirectory($slot)
        [IO.File]::WriteAllText((Join-Path $slot 'dis.dat'),'fixture gameplay checkpoint')
    }
    return $relative
}
try{
    if($null -ne (Find-ProjectManualSaveSession $fixture)){throw 'Empty history must not invent a save.'}
    $older=New-SaveFixture 1
    $newer=New-SaveFixture 2
    # More recent diagnostic output, even successful, must not become the manual source.
    $diagnostic=New-SaveFixture 3 @{diagnostic='full'}
    $null=New-SaveFixture 4 @{exit_code=1}
    $null=New-SaveFixture 5 @{complete=$false}
    $null=New-SaveFixture 6 @{benchmark=$true}
    $null=New-SaveFixture 7 @{debug=$true}
    $null=New-SaveFixture 8 @{timing_profile='critical'}
    $wrongGame=New-SaveFixture 9 @{game_sha256='different-game'}
    $empty=New-SaveFixture 10 @{} $false
    $corrupt=New-SaveFixture 11
    [IO.File]::WriteAllText((Join-Path $fixture ($corrupt+'/launch.json')),'{broken')
    $null=New-SaveFixture 13 @{auto_save_source_excluded=$true}
    $selected=Find-ProjectManualSaveSession $fixture
    if((Get-RelativePath $fixture $selected.Path) -ne $newer){throw 'Latest valid manual save was not selected.'}
    if($selected.Launch.runtime_sha256 -ne 'older-build'){throw 'Compatible saves from older engine builds must be accepted.'}
    $explicit=Read-ProjectSaveSession $fixture $older
    if((Get-RelativePath $fixture $explicit.Path) -ne $older){throw 'Explicit source did not override recency.'}
    $manual=New-SaveFixture 12
    if((Get-RelativePath $fixture (Find-ProjectManualSaveSession $fixture).Path) -ne $manual){throw 'New ordinary manual progress was not selected.'}
    foreach($bad in @($wrongGame,$empty,'state/sessions/../outside')){
        $rejected=$false
        try{Read-ProjectSaveSession $fixture $bad|Out-Null}catch{$rejected=$true}
        if(-not $rejected){throw 'Wrong-game, empty or escaped save source was accepted.'}
    }
    # Return to fresh diagnostic output only: do not adopt its initial/new-game save.
    foreach($relative in @($older,$newer,$manual)){
        $launchPath=Join-Path $fixture ($relative+'/launch.json')
        $launch=Read-JsonUtf8 $launchPath;$launch.complete=$false;Write-JsonUtf8 $launchPath $launch
    }
    if($null -ne (Find-ProjectManualSaveSession $fixture)){throw 'Diagnostic/failed/incomplete saves leaked into automatic selection.'}
    Write-Host 'Automatic manual-save selection, diagnostic exclusion, compatible builds and explicit-source checks passed.'
}finally{
    $allowed=Resolve-ProjectChild $projectRoot 'state/offline-tests'
    $resolved=[IO.Path]::GetFullPath($fixture)
    Assert-ProjectPath $projectRoot $resolved
    if(-not $resolved.StartsWith($allowed+'\',[StringComparison]::OrdinalIgnoreCase)){throw 'Test cleanup escaped its fixture.'}
    if(Test-Path -LiteralPath $resolved){Remove-Item -LiteralPath $resolved -Recurse -Force}
}
