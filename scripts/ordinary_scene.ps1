# SPDX-License-Identifier: GPL-2.0-or-later
# Importing this helper never captures a screen or confirms gameplay.
function Get-OrdinaryElapsed([string]$SessionPath){
    $taskFrames=Join-Path $SessionPath 'frames.csv'
    if(-not(Test-Path -LiteralPath $taskFrames)){return 0.0}
    $taskStream=[IO.File]::Open($taskFrames,[IO.FileMode]::Open,[IO.FileAccess]::Read,([IO.FileShare]::ReadWrite -bor [IO.FileShare]::Delete))
    $taskReader=[IO.StreamReader]::new($taskStream)
    try{$taskLines=$taskReader.ReadToEnd().Split([char]10)}finally{$taskReader.Dispose()}
    if($taskLines.Length -lt 3){return 0.0}
    return [double]$taskLines[$taskLines.Length-2].TrimEnd([char]13).Split(',')[1]/1e9
}
function Test-OrdinarySceneReceipt($Launch,$Receipt,[string]$SceneKey){
    if($Receipt.schema -ne 1 -or $Receipt.session -ne $Launch.session -or
       $Receipt.runtime_sha256 -ne $Launch.runtime_sha256 -or $Receipt.game_sha256 -ne $Launch.game_sha256 -or
       $Receipt.pid -ne $Launch.pid -or $Receipt.process_started -ne $Launch.process_started -or
       $Receipt.scene_key -ne $SceneKey -or $Receipt.evidence -notin @('agent-reviewed-gameplay-image','automatic-ocr-pause-render-counters') -or
       -not $Receipt.gameplay_confirmed -or -not $Receipt.view_confirmed -or
       $Receipt.elapsed_seconds -lt 0 -or $Receipt.screenshot_sha256 -notmatch '^[0-9a-f]{64}$'){
        throw 'Scene confirmation is missing, inconsistent or belongs to another process.'
    }
}
function Wait-OrdinaryScene([string]$Root,$Record,$Target,[string]$SceneKey,[int]$TimeoutSeconds=180){
    $taskReceiptPath=Join-Path $Record.Path 'ordinary-observer/scene-confirmed.json'
    $taskGatePath=Join-Path $Record.Path 'ordinary-observer/scene-gate.json'
    $taskDeadline=[DateTime]::UtcNow.AddSeconds($TimeoutSeconds)
    $taskNext=[DateTime]::UtcNow;$taskRequests=0
    while([DateTime]::UtcNow -lt $taskDeadline){
        if($Target.HasExited){throw 'Game exited before scene confirmation.'}
        if(Test-Path -LiteralPath $taskReceiptPath){
            $taskReceipt=Read-JsonUtf8 $taskReceiptPath
            Test-OrdinarySceneReceipt $Record.Launch $taskReceipt $SceneKey
            $taskImage=Resolve-ProjectChild $Root $taskReceipt.screenshot
            if(-not $taskImage.StartsWith($Record.Path+[IO.Path]::DirectorySeparatorChar,[StringComparison]::OrdinalIgnoreCase)){
                throw 'Scene image is outside this session.'
            }
            if((Get-FileHash -LiteralPath $taskImage -Algorithm SHA256).Hash.ToLowerInvariant() -ne $taskReceipt.screenshot_sha256){throw 'Reviewed scene image changed.'}
            $taskCurrent=Get-OrdinaryElapsed $Record.Path
            if($taskReceipt.elapsed_seconds -gt $taskCurrent+0.1){throw 'Scene confirmation is ahead of the game timeline.'}
            return $taskReceipt
        }
        if([DateTime]::UtcNow -ge $taskNext -and $taskRequests -lt 10 -and (Get-OrdinaryElapsed $Record.Path) -ge 3){
            & (Join-Path $PSScriptRoot 'control.ps1') -Action screenshot -Session $Record.Launch.session | Out-Null
            $taskRequests++;$taskNext=[DateTime]::UtcNow.AddSeconds(15)
            Write-JsonUtf8 $taskGatePath @{schema=1;session=$Record.Launch.session;scene_key=$SceneKey;status='awaiting_visual_review';requests=$taskRequests;gameplay_confirmed=$false}
        }
        Start-Sleep -Milliseconds 250
    }
    throw 'Scene confirmation deadline exceeded; no performance window was accepted.'
}
