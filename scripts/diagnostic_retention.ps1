# SPDX-License-Identifier: GPL-2.0-or-later
# Loaded by diagnostic_tools.ps1. Cleanup is invoked only before a debug launch, under run.lock.
# Saves belong to the game fingerprint, not to one particular engine build.
function Test-ProjectGameSaveHome([string]$Root,[string]$SaveHome,[string]$Title) {
    Assert-ProjectPath $Root $SaveHome
    if(-not(Test-Path -LiteralPath $SaveHome -PathType Container)){return $false}
    foreach($user in Get-ChildItem -LiteralPath $SaveHome -Directory){
        Assert-ProjectPath $Root $user.FullName
        $savedata=Join-Path $user.FullName ('savedata/'+$Title)
        Assert-ProjectPath $Root $savedata
        if(-not(Test-Path -LiteralPath $savedata -PathType Container)){continue}
        foreach($slot in Get-ChildItem -LiteralPath $savedata -Directory -Filter 'infamousss*'){
            $data=Join-Path $slot.FullName 'dis.dat'
            Assert-ProjectPath $Root $data
            if((Test-Path -LiteralPath $data -PathType Leaf) -and (Get-Item -LiteralPath $data).Length -gt 0){return $true}
        }
    }
    return $false
}
function Read-ProjectSaveSession([string]$Root,[string]$Session) {
    $path=Resolve-ProjectChild $Root $Session
    $area=Resolve-ProjectChild $Root 'state/sessions'
    if([IO.Path]::GetDirectoryName($path) -ne $area){throw 'Save source must be a direct project session.'}
    $launch=Read-JsonUtf8 (Join-Path $path 'launch.json')
    $manifest=Read-JsonUtf8 (Join-Path $Root 'project.json')
    foreach($key in @('schema','path_base','session','runtime','runtime_sha256','game_sha256','complete','game_launched','exit_code','pid','process_started')){
        if(-not $launch.PSObject.Properties[$key]){throw 'Save source has an incomplete launch record.'}
    }
    if($launch.schema -ne 3 -or $launch.path_base -ne 'project' -or
       (Resolve-ProjectChild $Root $launch.session) -ne $path -or
       $launch.game_sha256 -ne $manifest.game_sha256 -or $launch.complete -ne $true -or
       $launch.game_launched -ne $true -or $launch.exit_code -ne 0){
        throw 'Save source must be a successful completed session of this game.'
    }
    $null=Resolve-ProjectChild $Root $launch.runtime
    if(-not(Test-ProjectGameSaveHome $Root (Join-Path $path 'home') $manifest.title)){throw 'Save source contains no gameplay save.'}
    Assert-DiagnosticSessionInactive $Root $path
    return [pscustomobject]@{Path=$path;Launch=$launch}
}
function Find-ProjectManualSaveSession([string]$Root) {
    $area=Resolve-ProjectChild $Root 'state/sessions'
    if(-not(Test-Path -LiteralPath $area)){return $null}
    foreach($item in Get-ChildItem -LiteralPath $area -Directory | Sort-Object Name -Descending){
        if($item.Name -notmatch '^\d{8}-\d{6}-\d{3}-(auto|720p|900p|1080p|1440p|1800p|pro4k)-[a-f0-9]{8}($|-)'){continue}
        try{
            Assert-ProjectPath $Root $item.FullName
            $launch=Read-JsonUtf8 (Join-Path $item.FullName 'launch.json')
            # Never let an automated/new-game diagnostic save supersede manual progress.
            if(-not $launch.PSObject.Properties['diagnostic']){continue}
            # Preserve accidental new-game sessions, but never propagate them automatically.
            if($launch.PSObject.Properties['auto_save_source_excluded'] -and $launch.auto_save_source_excluded){continue}
            $automated=$false
            foreach($key in @('diagnostic','benchmark','debug','timing_profile')){
                if($launch.PSObject.Properties[$key] -and $launch.$key){$automated=$true;break}
            }
            if($automated){continue}
            return (Read-ProjectSaveSession $Root (Get-RelativePath $Root $item.FullName))
        }catch{Write-Verbose ('Skipped unusable manual save '+$item.Name+': '+$_.Exception.Message)}
    }
    return $null
}
function Test-DiagnosticRecordedProcess($Record,[string]$ExpectedPath) {
    $process=Get-Process -Id ([int]$Record.pid) -ErrorAction SilentlyContinue
    if(-not $process){return $false}
    try{return (Test-ProjectProcessIdentity $process $ExpectedPath ([string]$Record.started))}
    finally{$process.Dispose()}
}
function Assert-DiagnosticSessionInactive([string]$Root,[string]$Session) {
    $launchPath=Join-Path $Session 'launch.json'
    Assert-ProjectPath $Root $launchPath
    if(Test-Path -LiteralPath $launchPath){
        $launch=Read-JsonUtf8 $launchPath
        if($launch.game_launched){
            $record=[pscustomobject]@{pid=$launch.pid;started=$launch.process_started}
            if(Test-DiagnosticRecordedProcess $record (Resolve-ProjectPath $Root $launch.runtime)){
                throw ('Session still has a live game: '+[IO.Path]::GetFileName($Session))
            }
        }
    }
    $workerPath=Join-Path $Session 'diagnostics/worker.json'
    Assert-ProjectPath $Root $workerPath
    if(Test-Path -LiteralPath $workerPath){
        $worker=Read-JsonUtf8 $workerPath
        $shell=Join-Path $env:WINDIR 'System32/WindowsPowerShell/v1.0/powershell.exe'
        if(Test-DiagnosticRecordedProcess $worker $shell){throw 'A previous diagnostic collector is still running; no sessions were removed.'}
    }
    $statusPath=Join-Path $Session 'diagnostics/status.json'
    Assert-ProjectPath $Root $statusPath
    if(Test-Path -LiteralPath $statusPath){
        $status=Read-JsonUtf8 $statusPath
        if($status.PSObject.Properties['helpers']){
            foreach($helper in $status.helpers){
                if(Test-DiagnosticRecordedProcess $helper ([string]$helper.path)){
                    throw 'A previous diagnostic helper is still running; no sessions were removed.'
                }
            }
        }
    }
}
function Remove-DiagnosticSessionTree([string]$Root,[string]$Session,[string]$PreservedSession) {
    $area=Resolve-ProjectChild $Root 'state/sessions'
    $target=[IO.Path]::GetFullPath($Session)
    Assert-ProjectPath $Root $target
    if([IO.Path]::GetDirectoryName($target) -ne $area -or $target -eq $PreservedSession){
        throw 'Cleanup target is not an old direct child session.'
    }
    $pending=[Collections.Generic.Stack[object]]::new()
    $pending.Push(@{path=$target;expanded=$false})
    while($pending.Count){
        $entry=$pending.Pop();$path=[IO.Path]::GetFullPath($entry.path)
        if($path -ne $target -and -not $path.StartsWith($target+'\',[StringComparison]::OrdinalIgnoreCase)){
            throw 'Cleanup path escaped its session.'
        }
        # Validate parents, then unlink reparse points without following their targets.
        Assert-ProjectPath $Root ([IO.Path]::GetDirectoryName($path))
        $item=Get-Item -LiteralPath $path -Force -ErrorAction Stop
        if($item.Attributes -band [IO.FileAttributes]::ReparsePoint){
            if($item.PSIsContainer){[IO.Directory]::Delete($path)}else{[IO.File]::Delete($path)}
        }elseif(-not $item.PSIsContainer){
            [IO.File]::Delete($path)
        }elseif($entry.expanded){
            [IO.Directory]::Delete($path)
        }else{
            $pending.Push(@{path=$path;expanded=$true})
            foreach($child in Get-ChildItem -LiteralPath $path -Force -ErrorAction Stop){
                $pending.Push(@{path=$child.FullName;expanded=$false})
            }
        }
    }
}
function Invoke-DiagnosticSessionRetention([string]$Root,[string]$ProtectedSession) {
    $protected=if($ProtectedSession){(Read-ProjectSaveSession $Root $ProtectedSession).Path}else{$null}
    $area=Resolve-ProjectChild $Root 'state/sessions'
    $threshold=[long](Get-DiagnosticStoragePolicy).retention_threshold_bytes
    $usage=if(Test-Path -LiteralPath $area){Get-DiagnosticUsage $area}else{@{total_bytes=0L}}
    $directories=if(Test-Path -LiteralPath $area){@(Get-ChildItem -LiteralPath $area -Directory -Force)}else{@()}
    $pattern='^\d{8}-\d{6}-\d{3}-(auto|720p|900p|1080p|1440p|1800p|pro4k)-[a-f0-9]{8}(-diagnostic-[a-z-]+)?$'
    $sessions=@($directories|Where-Object {$_.Name -match $pattern}|Sort-Object Name -Descending)
    foreach($item in $sessions){Assert-ProjectPath $Root $item.FullName}
    $preserved=if($sessions.Count){$sessions[0].FullName}else{$null}
    $record=[ordered]@{
        schema=1;checked_utc=[DateTime]::UtcNow.ToString('o');area='state/sessions'
        threshold_bytes=$threshold;before_bytes=$usage.total_bytes;after_bytes=$usage.total_bytes
        triggered=($usage.total_bytes -gt $threshold);preserved_session=if($preserved){Get-RelativePath $Root $preserved}else{$null}
        protected_save_session=$ProtectedSession;planned_sessions=@();removed_sessions=@();unrecognized_directories=@($directories|Where-Object {$_.Name -notmatch $pattern}|ForEach-Object {$_.Name})
        complete=$false;error=$null
    }
    if($record.triggered){$record.planned_sessions=@($sessions|Select-Object -Skip 1|Where-Object {$_.FullName -ne $protected}|ForEach-Object {Get-RelativePath $Root $_.FullName})}
    $audit=Resolve-ProjectChild $Root 'state/runtime/session-retention.json'
    $null=[IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($audit))
    Write-JsonUtf8 $audit $record
    try{
        if($record.triggered){
            # Complete all activity checks before deleting the first directory.
            foreach($item in $sessions){Assert-DiagnosticSessionInactive $Root $item.FullName}
            foreach($relative in $record.planned_sessions){
                $target=Resolve-ProjectChild $Root $relative
                Remove-DiagnosticSessionTree $Root $target $preserved
                $record.removed_sessions+=@($relative)
                Write-JsonUtf8 $audit $record
            }
            if(Test-Path -LiteralPath $area){$record.after_bytes=(Get-DiagnosticUsage $area).total_bytes}
        }
        $record.complete=$true
        Write-JsonUtf8 $audit $record
        Write-Host ('采集区总量：{0:F2} GB；启动前清理阈值：{1:F0} GB；本次清理 {2} 个旧 session。' -f ($record.after_bytes/1e9),($threshold/1e9),$record.removed_sessions.Count)
        if($record.preserved_session){Write-Host ('保留最近一次：'+$record.preserved_session)}
        return $record
    }catch{
        $record.error=$_.Exception.Message
        Write-JsonUtf8 $audit $record
        throw
    }
}
