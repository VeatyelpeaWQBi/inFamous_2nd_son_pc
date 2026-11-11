# SPDX-License-Identifier: GPL-2.0-or-later
# Offline startup handoff regression; synthetic JSON only, no game/process/input.
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot '../../scripts/tools.ps1')
. (Join-Path $PSScriptRoot '../../scripts/super_capture_tools.ps1')
$projectRoot=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
$fixture=Resolve-ProjectChild $projectRoot ('state/test-super-handoff-'+[Guid]::NewGuid().ToString('N'))
$batchRelative='state/super-captures/test-batch'
$newSession='state/sessions/new-session'
$oldSession='state/sessions/old-session'
$currentHash=('a'*64);$oldHash=('b'*64)
$runtime='Build/RelWithDebInfo/inFAMOUS Second Son.exe'
function Expect-Failure([scriptblock]$Operation,[string]$Message) {
    $caught=$false
    try{& $Operation | Out-Null}catch{
        $caught=$true
        if($_.Exception.Message -notmatch $Message){throw}
    }
    if(-not $caught){throw ('Expected rejection: '+$Message)}
}
try{
    foreach($relative in @($batchRelative,$newSession,$oldSession)){
        $null=[IO.Directory]::CreateDirectory((Join-Path $fixture $relative))
    }
    Write-JsonUtf8 (Join-Path $fixture 'project.json') @{runtime_path=$runtime;runtime_sha256=$currentHash}
    $oldLaunch=[ordered]@{schema=3;path_base='project';session=$oldSession;runtime=$runtime
        runtime_sha256=$oldHash;complete=$true;game_launched=$true;super_capture=$false}
    Write-JsonUtf8 (Join-Path $fixture ($oldSession+'/launch.json')) $oldLaunch
    Write-JsonUtf8 (Join-Path $fixture 'state/active.json') @{session=$oldSession}
    # Reproduce the old global-pointer failure, then prove the new handoff waits.
    Expect-Failure {Read-ProjectSession $fixture $oldSession} 'pinned project runtime'
    if($null -ne (Read-SuperCaptureSession $fixture $batchRelative)){
        throw 'A stale active.json must not select a session or abort waiting.'
    }
    $request=[ordered]@{schema=1;batch=$batchRelative;status='starting'}
    Write-JsonUtf8 (Join-Path $fixture ($batchRelative+'/capture.json')) $request
    Assert-SuperCaptureStarting $fixture $batchRelative
    $newLaunch=[ordered]@{schema=3;path_base='project';session=$newSession;runtime=$runtime
        runtime_sha256=$currentHash;complete=$false;game_launched=$true;super_capture=$true
        super_capture_batch=$batchRelative;pid=2147483001;process_started='2026-10-30T01:00:00.1234567Z'}
    Write-JsonUtf8 (Join-Path $fixture ($newSession+'/launch.json')) $newLaunch
    # A prepared launch alone is not a handoff: the child must publish its receipt.
    if($null -ne (Read-SuperCaptureSession $fixture $batchRelative)){throw 'Selected an unpublished launch.'}
    Publish-SuperCaptureSession $fixture $batchRelative $newLaunch
    $selected=Read-SuperCaptureSession $fixture $batchRelative
    if($selected.Launch.session -ne $newSession){throw 'The exact new session was not selected.'}
    # The active pointer is deliberately STILL the obsolete engine's session.
    if((Read-JsonUtf8 (Join-Path $fixture 'state/active.json')).session -ne $oldSession){throw 'The test lost its stale pointer.'}
    $newLaunch.runtime_sha256=$oldHash
    Write-JsonUtf8 (Join-Path $fixture ($newSession+'/launch.json')) $newLaunch
    Expect-Failure {Read-SuperCaptureSession $fixture $batchRelative} 'pinned project runtime'
    $newLaunch.runtime_sha256=$currentHash
    $newLaunch.pid=2147483002
    Write-JsonUtf8 (Join-Path $fixture ($newSession+'/launch.json')) $newLaunch
    Expect-Failure {Read-SuperCaptureSession $fixture $batchRelative} 'launch identity'
    $newLaunch.pid=2147483001
    $newLaunch.process_started='2026-10-30T01:00:01.1234567Z'
    Write-JsonUtf8 (Join-Path $fixture ($newSession+'/launch.json')) $newLaunch
    Expect-Failure {Read-SuperCaptureSession $fixture $batchRelative} 'launch identity'
    $newLaunch.process_started='2026-10-30T01:00:00.1234567Z'
    $newLaunch.super_capture_batch='state/super-captures/another-batch'
    Write-JsonUtf8 (Join-Path $fixture ($newSession+'/launch.json')) $newLaunch
    Expect-Failure {Read-SuperCaptureSession $fixture $batchRelative} 'launch identity'
    $newLaunch.super_capture_batch=$batchRelative
    Write-JsonUtf8 (Join-Path $fixture ($newSession+'/launch.json')) $newLaunch
    $request.status='failed'
    Write-JsonUtf8 (Join-Path $fixture ($batchRelative+'/capture.json')) $request
    Expect-Failure {Assert-SuperCaptureStarting $fixture $batchRelative} 'launch cancelled'
    # A receipt racing with cancellation remains available for exact-process cleanup.
    Publish-SuperCaptureSession $fixture $batchRelative $newLaunch
    if((Read-SuperCaptureSession $fixture $batchRelative).Launch.pid -ne $newLaunch.pid){throw 'Cancelled batch lost its owned process receipt.'}
    $newLaunch.complete=$true
    Write-JsonUtf8 (Join-Path $fixture ($newSession+'/launch.json')) $newLaunch
    if($null -ne (Read-SuperCaptureSession $fixture $batchRelative)){throw 'A completed session must not be adopted.'}
    Expect-Failure {Read-SuperCaptureSession $fixture '../outside'} 'Untrusted super capture batch'
    Write-Host 'PASS: stale-build startup race reproduced; batch-only handoff, runtime/PID/time/owner checks and cancellation. No game started.'
}finally{
    $absolute=[IO.Path]::GetFullPath($fixture)
    $allowed=[IO.Path]::GetFullPath((Join-Path $projectRoot 'state')).TrimEnd('\')+'\'
    if(-not $absolute.StartsWith($allowed,[StringComparison]::OrdinalIgnoreCase) -or
        [IO.Path]::GetFileName($absolute) -notmatch '^test-super-handoff-[a-f0-9]{32}$'){
        throw 'Refusing cleanup outside the isolated handoff fixture.'
    }
    if(Test-Path -LiteralPath $absolute){Remove-Item -LiteralPath $absolute -Recurse -Force}
}
