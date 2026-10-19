# SPDX-License-Identifier: GPL-2.0-or-later
# Real small-directory cleanup tests; the production capture area is never addressed.
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot '../../scripts/tools.ps1')
. (Join-Path $PSScriptRoot '../../scripts/diagnostic_tools.ps1')
$projectRoot=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
$fixture=Resolve-ProjectChild $projectRoot ('state/offline-tests/retention-'+[Guid]::NewGuid().ToString('N'))
$area=Join-Path $fixture 'state/sessions'
$null=[IO.Directory]::CreateDirectory($area)
$script:retentionPolicy=Get-DiagnosticStoragePolicy
if($script:retentionPolicy.retention_threshold_bytes -ne 20000000000L){throw 'Retention threshold must be exactly 20 GB.'}
function Get-DiagnosticStoragePolicy {return $script:retentionPolicy}
function New-RetentionFixture([string]$Name) {
    $path=Join-Path $area $Name
    $null=[IO.Directory]::CreateDirectory($path)
    Write-JsonUtf8 (Join-Path $path 'launch.json') @{
        schema=3;session=(Get-RelativePath $fixture $path);game_launched=$false;complete=$true
    }
    [IO.File]::WriteAllText((Join-Path $path 'record.txt'),'retained evidence')
    return $path
}
$first=New-RetentionFixture '20260101-000000-001-pro4k-aaaaaaaa-diagnostic-full'
$second=New-RetentionFixture '20260101-000001-001-pro4k-bbbbbbbb-diagnostic-full'
$latest=New-RetentionFixture '20260101-000002-001-pro4k-cccccccc-diagnostic-full'
$outside=Join-Path $fixture 'outside-capture'
$null=[IO.Directory]::CreateDirectory($outside)
[IO.File]::WriteAllText((Join-Path $outside 'sentinel.txt'),'shared data must survive')
$junction=Join-Path $first 'shared-cache'
$null=New-Item -ItemType Junction -Path $junction -Target $outside
try{
    # A size exactly equal to the threshold does not trigger deletion.
    $script:retentionPolicy.retention_threshold_bytes=(Get-DiagnosticUsage $area).total_bytes
    $result=Invoke-DiagnosticSessionRetention $fixture 6>$null
    if($result.triggered -or $result.removed_sessions.Count -ne 0 -or -not [IO.Directory]::Exists($first)){
        throw 'Cleanup triggered at or below the threshold.'
    }
    # Retention must use the session creation name, not a later file modification time.
    [IO.Directory]::SetLastWriteTimeUtc($first,[DateTime]::UtcNow.AddYears(1))
    $unknown=Join-Path $area 'notes'
    $null=[IO.Directory]::CreateDirectory($unknown)
    [IO.File]::WriteAllText((Join-Path $unknown 'note.txt'),'not a session')
    $script:retentionPolicy.retention_threshold_bytes=1
    $result=Invoke-DiagnosticSessionRetention $fixture 6>$null
    if(-not $result.triggered -or $result.removed_sessions.Count -ne 2 -or
       [IO.Directory]::Exists($first) -or [IO.Directory]::Exists($second) -or -not [IO.Directory]::Exists($latest)){
        throw 'Cleanup did not retain exactly the latest recognized session.'
    }
    if(-not [IO.File]::Exists((Join-Path $outside 'sentinel.txt')) -or
       -not [IO.File]::Exists((Join-Path $unknown 'note.txt'))){
        throw 'Cleanup followed a link or deleted a non-session directory.'
    }
    $result=Invoke-DiagnosticSessionRetention $fixture 6>$null
    if($result.removed_sessions.Count -ne 0 -or -not [IO.Directory]::Exists($latest)){
        throw 'A lone oversized latest session must survive.'
    }

    # Every activity check must finish before any deletion starts.
    $old=New-RetentionFixture '20260101-000003-001-pro4k-dddddddd-diagnostic-full'
    $newest=New-RetentionFixture '20260101-000004-001-pro4k-eeeeeeee-diagnostic-full'
    $workerDirectory=Join-Path $old 'diagnostics'
    $null=[IO.Directory]::CreateDirectory($workerDirectory)
    $self=[Diagnostics.Process]::GetCurrentProcess()
    try{Write-JsonUtf8 (Join-Path $workerDirectory 'worker.json') @{pid=$PID;started=$self.StartTime.ToUniversalTime().ToString('o')}}
    finally{$self.Dispose()}
    $blocked=$false
    try{Invoke-DiagnosticSessionRetention $fixture 6>$null|Out-Null}catch{
        if($_.Exception.Message -notlike '*collector is still running*'){throw}
        $blocked=$true
    }
    if(-not $blocked -or -not [IO.Directory]::Exists($old) -or -not [IO.Directory]::Exists($latest)){
        throw 'An active worker was not protected before deleting older evidence.'
    }
    Remove-Item -LiteralPath (Join-Path $workerDirectory 'worker.json')
    $result=Invoke-DiagnosticSessionRetention $fixture 6>$null
    if($result.removed_sessions.Count -ne 2 -or -not [IO.Directory]::Exists($newest)){
        throw 'Cleanup did not recover after the stale activity record was removed.'
    }
    foreach($target in @($newest,$outside,$area)){
        $rejected=$false
        try{Remove-DiagnosticSessionTree $fixture $target $newest}catch{$rejected=$true}
        if(-not $rejected){throw 'Protected or out-of-area deletion target was accepted.'}
    }
    $audit=Read-JsonUtf8 (Join-Path $fixture 'state/runtime/session-retention.json')
    if(-not $audit.complete -or $audit.removed_sessions.Count -ne 2 -or
       $audit.preserved_session -ne (Get-RelativePath $fixture $newest)){
        throw 'Cleanup audit does not describe the actual preserved and deleted sessions.'
    }
    # An explicitly selected older save must survive alongside the latest session.
    Write-JsonUtf8 (Join-Path $fixture 'project.json') @{runtime_path='Build/fixture.exe';runtime_sha256='fixture';game_sha256='fixture-game';title='CUSA00309'}
    $protectedSave=New-RetentionFixture '20260101-000000-002-pro4k-11111111-diagnostic-full'
    $victim=New-RetentionFixture '20260101-000000-003-pro4k-22222222-diagnostic-full'
    $protectedRelative=Get-RelativePath $fixture $protectedSave
    Write-JsonUtf8 (Join-Path $protectedSave 'launch.json') @{
        schema=3;path_base='project';session=$protectedRelative;runtime='Build/fixture.exe'
        runtime_sha256='old-build';game_sha256='fixture-game';game_launched=$true;complete=$true;exit_code=0
        pid=2147483647;process_started='2026-01-01T00:00:00Z'
    }
    $saveDirectory=Join-Path $protectedSave 'home/1000/savedata/CUSA00309/infamousssAuto0'
    $null=[IO.Directory]::CreateDirectory($saveDirectory)
    [IO.File]::WriteAllText((Join-Path $saveDirectory 'dis.dat'),'fixture save')
    $result=Invoke-DiagnosticSessionRetention $fixture $protectedRelative 6>$null
    if($result.removed_sessions.Count -ne 1 -or [IO.Directory]::Exists($victim) -or
       -not [IO.Directory]::Exists($protectedSave) -or -not [IO.Directory]::Exists($newest) -or
       $result.protected_save_session -ne $protectedRelative){
        throw 'Explicit save source was not protected alongside the latest session.'
    }
    Write-Host 'Retention threshold, latest-session/save-source preservation, live-worker protection and junction-safe deletion checks passed.'
}finally{
    if(Test-Path -LiteralPath $junction){[IO.Directory]::Delete($junction)}
    $allowed=Resolve-ProjectChild $projectRoot 'state/offline-tests'
    $resolved=[IO.Path]::GetFullPath($fixture)
    Assert-ProjectPath $projectRoot $resolved
    if(-not $resolved.StartsWith($allowed+'\',[StringComparison]::OrdinalIgnoreCase)){
        throw 'Refusing to clean a test path outside state/offline-tests.'
    }
    if(Test-Path -LiteralPath $resolved){Remove-Item -LiteralPath $resolved -Recurse -Force}
}
