# SPDX-License-Identifier: GPL-2.0-or-later
# Existing Windows OCR, native benchmark controls and rendering averages only.
function Test-OrdinaryStatusSnapshot($Status,[long]$Sequence,$Launch,[long]$NowUnixMs){
    if($null -eq $Status -or $Status -isnot [pscustomobject]){return $false}
    # A valid JSON prefix such as {} is possible with older native writers.
    # Inspect fields before accessing them under StrictMode; incomplete/stale data is retried.
    foreach($taskKey in @('sequence','schema')){
        if(-not $Status.PSObject.Properties[$taskKey] -or $null -eq $Status.$taskKey){return $false}
    }
    $taskSequence=0L;$taskSchema=0L
    if(-not [long]::TryParse([string]$Status.sequence,[ref]$taskSequence) -or $taskSequence -ne $Sequence){return $false}
    if(-not [long]::TryParse([string]$Status.schema,[ref]$taskSchema)){return $false}
    if($taskSchema -lt 2){throw 'Rebuild required: automatic scene verification needs native status schema 2.'}
    foreach($taskKey in @('game_serial','perf_log','gpu_profile','display_window_count','display_width','display_height',
                          'observed_unix_ms','draws_per_frame','dispatches_per_frame')){
        if(-not $Status.PSObject.Properties[$taskKey] -or $null -eq $Status.$taskKey){return $false}
    }
    $taskObserved=0L
    if(-not [long]::TryParse([string]$Status.observed_unix_ms,[ref]$taskObserved) -or
       $NowUnixMs - $taskObserved -gt 5000 -or $taskObserved - $NowUnixMs -gt 1000){return $false}
    if($Status.game_serial -ne 'CUSA00309' -or $Status.perf_log -ne 0 -or $Status.gpu_profile -ne 0){throw 'Unexpected game or heavy instrumentation during ordinary navigation.'}
    if($Status.display_window_count -ne 1 -or $Status.display_width -ne $Launch.resolution.width -or $Status.display_height -ne $Launch.resolution.height){throw 'Automatic comparison output dimensions/window count do not match the selected resolution.'}
    return $true
}
function Get-OrdinaryRenderStatus([string]$Root,$Record){
    & (Join-Path $PSScriptRoot 'control.ps1') -Action status -Session $Record.Launch.session | Out-Null
    $taskCommand=Read-JsonUtf8 (Join-Path $Record.Path 'command.json')
    if($null -eq $taskCommand -or $taskCommand -isnot [pscustomobject] -or -not $taskCommand.PSObject.Properties['sequence']){throw 'The queued status request has no sequence.'}
    $taskDeadline=[DateTime]::UtcNow.AddSeconds(6)
    while([DateTime]::UtcNow -lt $taskDeadline){
        $taskPath=Join-Path $Record.Path 'status.json'
        if(Test-Path -LiteralPath $taskPath){
            try{
                $taskStatus=Read-JsonUtf8 $taskPath
                if(Test-OrdinaryStatusSnapshot $taskStatus ([long]$taskCommand.sequence) $Record.Launch ([DateTimeOffset]::UtcNow.ToUnixTimeMilliseconds())){
                    return $taskStatus
                }
            }catch [IO.IOException]{}catch [IO.InvalidDataException]{}
        }
        Start-Sleep -Milliseconds 100
    }
    throw 'Native status request did not produce a complete, fresh matching snapshot before its deadline.'
}
function Get-OrdinaryImageSignature([string]$Path){
    if(-not(Test-DiagnosticPngReady $Path)){throw 'Scene screenshot is incomplete.'}
    Add-Type -AssemblyName System.Drawing
    $taskBitmap=[Drawing.Bitmap]::new($Path)
    try{
        $taskValues=@()
        for($taskY=0;$taskY -lt 9;$taskY++){
            for($taskX=0;$taskX -lt 16;$taskX++){
                $taskPixel=$taskBitmap.GetPixel([int]($taskBitmap.Width*(.1+.8*($taskX+.5)/16)),[int]($taskBitmap.Height*(.1+.8*($taskY+.5)/9)))
                $taskValues+=@([int]$taskPixel.R,[int]$taskPixel.G,[int]$taskPixel.B)
            }
        }
        return ,$taskValues
    }finally{$taskBitmap.Dispose()}
}
function Get-OrdinaryImageDistance($First,$Second){
    if($First.Count -ne 432 -or $Second.Count -ne 432){throw 'Unexpected scene signature shape.'}
    $taskSum=0.0
    for($taskIndex=0;$taskIndex -lt 432;$taskIndex++){$taskSum += [Math]::Abs([double]$First[$taskIndex]-[double]$Second[$taskIndex])}
    return $taskSum/(432*255)
}
function Wait-AutomaticOrdinaryScene([string]$Root,$Record,$Target,[string]$SceneKey,[string]$ReferenceImage='',
                                      [double]$MaximumImageDistance=.15,[int]$TimeoutSeconds=240){
    if(-not $Record.Launch.comparison_automation){throw 'Automatic scene navigation requires an isolated automated-comparison launch.'}
    $taskDirectory=Join-Path $Record.Path 'diagnostics'
    $null=[IO.Directory]::CreateDirectory($taskDirectory)
    $taskPlan=@{profile='ordinary';seconds=60;automation=@{boot_timeout_seconds=$TimeoutSeconds}}
    $taskAuto=New-DiagnosticAutomation $taskPlan
    $taskClock=[Diagnostics.Stopwatch]::StartNew();$taskNextStatus=0.0;$taskStatus=$null
    while(-not $taskAuto.ready){
        if($Target.HasExited){throw 'Game exited before automatic scene verification.'}
        if(Test-Path -LiteralPath (Join-Path $Root 'state/runtime/v3-suite.stop')){throw 'Local stop request received.'}
        if($taskClock.Elapsed.TotalSeconds -gt $TimeoutSeconds){throw 'Automatic scene verification timed out.'}
        if($taskClock.Elapsed.TotalSeconds -ge $taskNextStatus){
            $taskStatus=Get-OrdinaryRenderStatus $Root $Record
            $taskNextStatus=$taskClock.Elapsed.TotalSeconds+2
        }
        Update-DiagnosticAutomation $Root $Record.Launch.session $taskPlan $taskAuto $taskClock.Elapsed.TotalSeconds
        Start-Sleep -Milliseconds 250
    }
    $taskLast=$taskAuto.screens[$taskAuto.screens.Count-1]
    $taskImage=Resolve-ProjectChild $Root $taskLast.path
    $taskDistance=$null
    if($ReferenceImage){
        $taskReference=Resolve-ProjectChild $Root $ReferenceImage
        $taskDistance=Get-OrdinaryImageDistance (Get-OrdinaryImageSignature $taskReference) (Get-OrdinaryImageSignature $taskImage)
        if($taskDistance -gt $MaximumImageDistance){throw ('Scene image differs from its fixed-save control: '+$taskDistance)}
    }
    $taskReceipt=@{schema=1;session=$Record.Launch.session;runtime_sha256=$Record.Launch.runtime_sha256;game_sha256=$Record.Launch.game_sha256
        pid=$Record.Launch.pid;process_started=$Record.Launch.process_started;scene_key=$SceneKey
        screenshot=$taskLast.path;screenshot_sha256=(Get-FileHash -LiteralPath $taskImage -Algorithm SHA256).Hash.ToLowerInvariant()
        evidence='automatic-ocr-pause-render-counters';gameplay_confirmed=$true;view_confirmed=$true
        view_policy='fixed save, no workload/camera input, optional coarse control-image check; not semantic image acceptance'
        reference_image=$ReferenceImage;image_distance=$taskDistance;maximum_image_distance=$MaximumImageDistance
        native_status=$taskStatus;scene_evidence=$taskAuto.scene_evidence;pause_verified=$taskAuto.pause_verified
        elapsed_seconds=(Get-OrdinaryElapsed $Record.Path);confirmed_utc=[DateTime]::UtcNow.ToString('o')}
    Test-OrdinarySceneReceipt $Record.Launch $taskReceipt $SceneKey
    Write-JsonUtf8 (Join-Path $Record.Path 'ordinary-observer/scene-confirmed.json') $taskReceipt
    return $taskReceipt
}
