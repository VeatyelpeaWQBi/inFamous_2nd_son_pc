# SPDX-License-Identifier: GPL-2.0-or-later
[CmdletBinding()]
param()
Set-StrictMode -Version Latest
$ErrorActionPreference='Stop'
if($PSVersionTable.PSVersion.Major -ne 5){throw 'Use the installed Windows PowerShell 5.1.'}
. (Join-Path $PSScriptRoot 'tools.ps1')
. (Join-Path $PSScriptRoot 'build_tools.ps1')
$root=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$area=Join-Path $root ('state/runtime/renderer-pgo-'+(Get-Date -Format 'yyyyMMdd-HHmmss')+'-'+[Guid]::NewGuid().ToString('N').Substring(0,8))
$null=[IO.Directory]::CreateDirectory($area)
$shell=Join-Path $PSHOME 'powershell.exe'
$compilerReceipt=Join-Path $root 'Build/RelWithDebInfo/compiler-variant.json'
$tools=Get-ProjectBuildTools $root
$profdata=Join-Path (Split-Path -Parent $tools.CMAKE_CXX_COMPILER) 'llvm-profdata.exe'
if(-not(Test-Path -LiteralPath $profdata -PathType Leaf)){throw 'The installed clang-cl has no llvm-profdata.'}
$state=@{schema=1;status='building-train';error=$null;baseline_restored=$false
    source_date_epoch=[DateTimeOffset]::UtcNow.ToUnixTimeSeconds();performance_acceptance=$false}
$index=Join-Path $area 'training.json'
$lock=[IO.File]::Open((Join-Path $root 'state/runtime/renderer-pgo.lock'),[IO.FileMode]::OpenOrCreate,[IO.FileAccess]::ReadWrite,[IO.FileShare]::None)
$restore=$false
function Save-Training {
    $state.updated_utc=[DateTime]::UtcNow.ToString('o')
    Write-JsonUtf8 $index $state
}
function Build-TrainingVariant([string]$Mode){
    if(Get-Process -Name 'inFAMOUS Second Son' -ErrorAction SilentlyContinue){
        throw 'Close the running game before changing the renderer build.'
    }
    $arguments=@('-NoLogo','-NoProfile','-ExecutionPolicy','Bypass','-File',(Join-Path $PSScriptRoot 'build.ps1'),'-RendererPgo',$Mode)
    Invoke-ProjectBuildProcess $shell $arguments $root @{SOURCE_DATE_EPOCH=[string]$state.source_date_epoch}
    $receipt=Read-JsonUtf8 $compilerReceipt
    if($receipt.renderer_pgo -ne $Mode -or $receipt.enabled){throw 'Unexpected renderer compiler receipt.'}
    Write-JsonUtf8 (Join-Path $area ($Mode+'-compiler.json')) $receipt
    return $receipt
}
try{
    Save-Training
    if(Get-Process -Name 'inFAMOUS Second Son' -ErrorAction SilentlyContinue){throw 'A game is already running.'}
    $restore=$true
    $train=Build-TrainingVariant 'Train'
    $state.status='collecting';Save-Training
    $started=[DateTime]::UtcNow
    & $shell -NoLogo -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot 'collect_super.ps1') -Mode pro4k -Profile lightweight -Rounds 1 -Experiment none -RendererPgoTrial Train
    $code=$LASTEXITCODE
    if($code -ne 0){throw ('Renderer training capture failed: '+$code)}
    $capture=Read-JsonUtf8 (Join-Path $root 'state/super-captures/last-native-run.json')
    if($capture.status -ne 'collected' -or [DateTime]::Parse($capture.updated_utc).ToUniversalTime() -lt $started){
        throw 'No fresh successful renderer training capture.'
    }
    $state.session=$capture.session;$state.batch=$capture.batch
    $session=Resolve-ProjectChild $root $capture.session
    $launch=Read-JsonUtf8 (Join-Path $session 'launch.json')
    $cycles=@(Read-JsonUtf8 (Join-Path $session 'super-rounds.json'))
    if($launch.runtime_sha256 -cne $train.runtime_sha256 -or $launch.renderer_pgo -ne 'Train' -or
       -not $launch.complete -or $launch.exit_code -ne 0 -or $launch.progress_published -ne $false -or
       $launch.source_game_written -ne $false -or $cycles.Count -ne 1 -or $cycles[0].kind -ne 'measurement' -or
       -not $cycles[0].warm -or -not $cycles[0].meter_consumed){
        throw 'The isolated training build did not complete one qualified warm release.'
    }
    $raw=Get-Item -LiteralPath (Join-Path $session 'renderer-pgo.profraw')
    if($raw.Length -lt 1 -or $raw.Length -gt 16MB){throw 'Renderer raw profile is outside its size bound.'}
    $state.status='indexing';Save-Training
    $indexedProfile=Join-Path $area 'renderer.profdata'
    Invoke-ProjectBuildProcess $profdata @('merge','--failure-mode=any','-o',$indexedProfile,$raw.FullName) $area
    $counts=Invoke-ProjectBuildProcess $profdata @('show','--all-functions','--counts',$indexedProfile) $area @{} -Capture
    foreach($name in @('DispatchDirect','BindBuffers','ObtainBuffer','GetComputePipeline')){
        if($counts -notmatch ('(?m)^  \?'+$name+'@[^\r\n]+:\r?\n    Hash:[^\r\n]+\r?\n    Counters:[^\r\n]+\r?\n    Block counts: \[[0-9, ]*[1-9]')){
            throw ('Renderer profile has no executed '+$name+' counters.')
        }
    }
    [IO.File]::WriteAllText((Join-Path $area 'profile-counts.txt'),$counts,[Text.UTF8Encoding]::new($false))
    $state.raw_sha256=(Get-FileHash -LiteralPath $raw.FullName).Hash.ToLowerInvariant()
    $state.profile=Get-RelativePath $root $indexedProfile
    $state.profile_sha256=(Get-FileHash -LiteralPath $indexedProfile).Hash.ToLowerInvariant()
    $state.status='trained'
}catch{
    $state.status='failed';$state.error=$_.Exception.Message
}finally{
    try{
        if($restore){
            $normal=Build-TrainingVariant 'Off'
            $state.baseline_restored=$true
            if($state.status -eq 'trained'){
                foreach($key in @('source_sha256','compiler_sha256','normalized_commands_sha256','generated_revision_sha256')){
                    if($train.$key -cne $normal.$key){throw ('Training and ordinary builds differ in '+$key)}
                }
                if((ConvertTo-Json -InputObject $train.generated_headers -Depth 8 -Compress) -cne
                   (ConvertTo-Json -InputObject $normal.generated_headers -Depth 8 -Compress)){
                    throw 'Training and ordinary builds have different generated headers.'
                }
            }
        }
    }catch{
        $state.status='failed';$state.error=([string]$state.error+' Restore/check: '+$_.Exception.Message).Trim()
    }finally{
        $lock.Dispose()
        Save-Training
    }
}
Write-Host ('Renderer PGO training: '+$state.status+'; '+(Get-RelativePath $root $index))
if($state.status -ne 'trained'){throw $state.error}
