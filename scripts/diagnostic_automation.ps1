# SPDX-License-Identifier: GPL-2.0-or-later
# Importing this file never starts capture or sends input.
$script:DiagnosticOcrEngine=$null
$script:DiagnosticAsyncMethod=$null
function Get-DiagnosticMenuDecision([string]$Text,[string]$Selected='') {
    $lines=@(($Text -split '\r?\n')|ForEach-Object {($_ -replace '\s+',' ').Trim().ToLowerInvariant()})
    $selected=($Selected -replace '\s+',' ').Trim().ToLowerInvariant()
    if($Text -match '(?i)are you sure|do you want|overwrite|delete save|delete data' -or $lines -contains 'yes' -or $lines -contains 'no'){
        return @{action='blocked';reason='confirmation-dialog'}
    }
    if($Text -match '(?i)\bloading\b|\bsaving\b|please wait'){return @{action='wait';reason='loading'}}
    if($Text -match '(?i)press.+\boptions\b'){return @{action='button';button='options';reason='title-prompt'}}
    if($Text -match '(?i)press.+(?:any button|any key|to start|to begin|cross|\bX\b)'){
        return @{action='button';button='confirm';reason='title-prompt'}
    }
    if(@($lines|Where-Object {$_ -match '^(pause|paused|options and save|options & save|photo mode|save game)$'}).Count){
        return @{action='blocked';reason='pause-menu'}
    }
    $target=@($lines|Where-Object {$_ -match '^(continue|continue game|continue last save|load game|resume|resume game)$'}|Select-Object -First 1)
    if($target.Count){
        if($selected -and $selected -ne $target[0]){
            $targetIndex=[array]::IndexOf($lines,$target[0]);$selectedIndex=[array]::IndexOf($lines,$selected)
            if($selectedIndex -ge 0){return @{action='button';button=if($selectedIndex -gt $targetIndex){'up'}else{'down'};reason='select-continue'}}
        }
        return @{action='button';button='confirm';reason='continue-save';selection=if($selected){'highlight'}else{'default-menu-selection'}}
    }
    if($lines -contains 'new game'){return @{action='blocked';reason='no-continue-option'}}

    # This objective is visible in the captured opening-mission HUD, including auto-resume.
    if($lines -contains 'defeat hank and find out what is happening to you'){
        return @{action='wait';reason='gameplay-hud'}
    }
    return @{action='wait';reason='unknown-frame'}
}
function Get-DiagnosticPauseResponse($Decision,[bool]$ReturnSent,[double]$Age,[bool]$Telemetry) {
    if($Decision.reason -eq 'pause-menu' -and -not $ReturnSent){
        # Visually verified footer of the fingerprint-pinned CUSA00309 01.07 options page.
        return @{action='return';button='cross';reason='verified-game-back-button'}
    }
    if($Decision.action -eq 'blocked' -and $Decision.reason -ne 'pause-menu'){
        return @{action='fail';reason=$Decision.reason}
    }
    if($ReturnSent -and $Decision.action -eq 'wait' -and $Decision.reason -eq 'unknown-frame' -and $Telemetry){
        return @{action='resumed';reason='pause-closed'}
    }
    if($Age -gt 20){return @{action='fail';reason='pause-response-timeout'}}
    return @{action='wait';reason='await-pause-response'}
}
function Test-DiagnosticPauseProbe($Decision,[bool]$PriorProof,[int]$Attempts,[double]$SinceMenuInput,[bool]$Telemetry) {
    return (-not $PriorProof -and $Attempts -eq 0 -and $SinceMenuInput -ge 15 -and
        $Telemetry -and $Decision.action -eq 'wait' -and $Decision.reason -eq 'unknown-frame')
}
function Test-DiagnosticGameplayCandidate($Decision,[bool]$ContinueSent,[double]$SinceMenuInput,[bool]$Telemetry) {
    if(-not $Telemetry -or $Decision.action -ne 'wait'){return $false}
    if($Decision.reason -eq 'gameplay-hud'){return $true}
    return ($Decision.reason -eq 'unknown-frame' -and $ContinueSent -and $SinceMenuInput -ge 15)
}
function Test-DiagnosticPngReady([string]$Path) {
    $stream=$null
    try{
        # Deny an active writer; the engine creates the final name before writing PNG data.
        $stream=[IO.File]::Open($Path,[IO.FileMode]::Open,[IO.FileAccess]::Read,[IO.FileShare]::Read)
        if($stream.Length -lt 67){return $false}
        $header=[byte[]]::new(8);$tail=[byte[]]::new(12)
        if($stream.Read($header,0,8) -ne 8){return $false}
        $null=$stream.Seek(-12,[IO.SeekOrigin]::End)
        if($stream.Read($tail,0,12) -ne 12){return $false}
        return ([BitConverter]::ToString($header) -eq '89-50-4E-47-0D-0A-1A-0A' -and
            [BitConverter]::ToString($tail) -eq '00-00-00-00-49-45-4E-44-AE-42-60-82')
    }catch [IO.IOException]{return $false}
    finally{if($stream){$stream.Dispose()}}
}
function Test-DiagnosticCaptureClosed([string]$Path) {
    $stream=$null
    try{
        $stream=[IO.File]::Open($Path,[IO.FileMode]::Open,[IO.FileAccess]::Read,[IO.FileShare]::Read)
        return $stream.Length -gt 0
    }catch [IO.IOException]{return $false}
    finally{if($stream){$stream.Dispose()}}
}
function Wait-DiagnosticWinRT($Operation,[Type]$ResultType) {
    try{
        $task=$script:DiagnosticAsyncMethod.MakeGenericMethod($ResultType).Invoke($null,@($Operation))
        if(-not $task.Wait(15000)){throw [TimeoutException]::new('Windows '+$ResultType.Name+' operation timed out.')}
        return $task.Result
    }catch{
        $cause=$_.Exception
        while($cause.InnerException){$cause=$cause.InnerException}
        throw [InvalidOperationException]::new(('Windows '+$ResultType.Name+' failed (0x{0:X8}): ' -f $cause.HResult)+$cause.Message,$cause)
    }
}
function Read-DiagnosticScreen([string]$Path) {
    if(-not $script:DiagnosticOcrEngine){
        Add-Type -AssemblyName System.Runtime.WindowsRuntime
        Add-Type -AssemblyName System.Drawing
        $null=[Windows.Storage.StorageFile,Windows.Storage,ContentType=WindowsRuntime]
        $null=[Windows.Graphics.Imaging.BitmapDecoder,Windows.Graphics.Imaging,ContentType=WindowsRuntime]
        $null=[Windows.Media.Ocr.OcrEngine,Windows.Foundation,ContentType=WindowsRuntime]
        $language=[Windows.Globalization.Language,Windows.Globalization,ContentType=WindowsRuntime]::new('en-US')
        $script:DiagnosticOcrEngine=[Windows.Media.Ocr.OcrEngine]::TryCreateFromLanguage($language)
        if(-not $script:DiagnosticOcrEngine){throw 'The installed Windows English OCR recognizer is unavailable.'}
        $script:DiagnosticAsyncMethod=[System.WindowsRuntimeSystemExtensions].GetMethods()|Where-Object {
            $_.Name -eq 'AsTask' -and $_.IsGenericMethod -and $_.GetGenericArguments().Count -eq 1 -and
            $_.GetParameters().Count -eq 1 -and $_.GetParameters()[0].ParameterType.Name -eq ('IAsyncOperation'+[char]96+'1')
        }|Select-Object -First 1
        if(-not $script:DiagnosticAsyncMethod){throw 'Windows Runtime asynchronous bridge is unavailable.'}
    }
    $stream=$null;$bitmap=$null;$image=$null
    try{
        $file=Wait-DiagnosticWinRT ([Windows.Storage.StorageFile]::GetFileFromPathAsync($Path)) ([Windows.Storage.StorageFile])
        $stream=Wait-DiagnosticWinRT ($file.OpenReadAsync()) ([Windows.Storage.Streams.IRandomAccessStreamWithContentType])
        $decoder=Wait-DiagnosticWinRT ([Windows.Graphics.Imaging.BitmapDecoder]::CreateAsync($stream)) ([Windows.Graphics.Imaging.BitmapDecoder])
        $transform=[Windows.Graphics.Imaging.BitmapTransform]::new()
        $scale=[Math]::Min(1.0,1600.0/$decoder.PixelWidth)
        $transform.ScaledWidth=[uint32]($decoder.PixelWidth*$scale);$transform.ScaledHeight=[uint32]($decoder.PixelHeight*$scale)
        $bitmap=Wait-DiagnosticWinRT ($decoder.GetSoftwareBitmapAsync(
            [Windows.Graphics.Imaging.BitmapPixelFormat]::Bgra8,[Windows.Graphics.Imaging.BitmapAlphaMode]::Premultiplied,
            $transform,[Windows.Graphics.Imaging.ExifOrientationMode]::IgnoreExifOrientation,
            [Windows.Graphics.Imaging.ColorManagementMode]::DoNotColorManage)) ([Windows.Graphics.Imaging.SoftwareBitmap])
        $result=Wait-DiagnosticWinRT ($script:DiagnosticOcrEngine.RecognizeAsync($bitmap)) ([Windows.Media.Ocr.OcrResult])
        $image=[Drawing.Bitmap]::new($Path);$selected='';$best=0
        foreach($line in $result.Lines){
            if($line.Text -notmatch '(?i)^(continue|continue game|continue last save|load game|resume|resume game|new game|options|settings|credits|extras)$'){continue}
            $yellow=0
            foreach($word in $line.Words){
                $r=$word.BoundingRect
                for($x=1;$x -lt 8;$x++){for($y=1;$y -lt 5;$y++){
                    $px=[Math]::Min($image.Width-1,[int](($r.X+$r.Width*$x/8)/$scale))
                    $py=[Math]::Min($image.Height-1,[int](($r.Y+$r.Height*$y/5)/$scale))
                    $c=$image.GetPixel($px,$py)
                    if($c.R -gt 160 -and $c.G -gt 105 -and $c.B -lt 115 -and $c.R -gt $c.B*1.4){$yellow++}
                }}
            }
            if($yellow -gt $best -and $yellow -ge 3){$best=$yellow;$selected=$line.Text}
        }
        return @{text=(@($result.Lines|ForEach-Object Text) -join [Environment]::NewLine);selected=$selected}
    }finally{
        if($image){$image.Dispose()};if($bitmap){$bitmap.Dispose()};if($stream){$stream.Dispose()}
    }
}
function Get-DiagnosticWorkload([string]$Profile,[int]$Seconds) {
    $events=@()
    if($Profile -in @('cpu','vtune')){
        for($i=10;$i -lt $Seconds-4;$i+=15){
            $events+=@{at=$i;action='axis';axis=2;value=if(($i/15)%2 -lt 1){152}else{104}}
            $events+=@{at=$i+4;action='axis';axis=2;value=128}
        }
    }else{
        for($i=0;$i -lt $Seconds-11;$i+=12){
            $events+=@{at=$i;action='axis';axis=2;value=152}
            $events+=@{at=$i+2;action='axis';axis=2;value=128}
            $events+=@{at=$i+4;action='button';button='square'}
            $events+=@{at=$i+6;action='button';button='circle'}
            $events+=@{at=$i+8;action='axis';axis=2;value=104}
            $events+=@{at=$i+10;action='axis';axis=2;value=128}
        }
    }
    return $events
}
function New-DiagnosticAutomation($Plan) {
    return [ordered]@{
        enabled=$true;phase='boot';scene_status='pending';scene_confirmed=$false
        next_screen=10.0;pending_screen=$null;screen_requested_utc=$null;screen_count=0
        continue_sent=$false;last_menu_input=-20.0;candidate_count=0;scene_evidence=$null
        pause_probe_pending=$false;pause_return_sent=$false;pause_verified=$false;pause_probe_attempts=0
        ready=$false;ready_at=$null;capture_at=$null;capture_triggered=$false;capture_trigger_attempted=$false
        event_index=0;events=@(Get-DiagnosticWorkload $Plan.profile $Plan.seconds);xray_triggered=$false
        capture_sequence=$null;capture_completed=$false;workload_completed=$false
        inputs=@();screens=@();warnings=@();input_suspended=$false
        workload=if($Plan.profile -in @('cpu','vtune')){'idle-and-camera'}elseif($Plan.profile -in @('memory','vtune-memory')){'repeated-camera-and-effects'}else{'camera-and-effects'}
    }
}
function Send-DiagnosticAutoInput([string]$Root,[string]$Session,$Auto,$Event,[double]$Elapsed) {
    $parameters=@{Action=$Event.action;Session=$Session}
    if($Event.action -eq 'axis'){$parameters.Axis=[int]$Event.axis;$parameters.Value=[int]$Event.value}
    if($Event.action -eq 'button'){$parameters.Button=$Event.button;$parameters.DurationMs=if($Event.ContainsKey('duration_ms')){[int]$Event.duration_ms}else{200}}
    & (Join-Path $PSScriptRoot 'control.ps1') @parameters | Out-Null
    $record=Read-ProjectSession $Root $Session
    $command=Read-JsonUtf8 (Join-Path $record.Path 'command.json')
    $Auto.inputs+=@{loop_elapsed_s=$Elapsed;queued_utc=[DateTime]::UtcNow.ToString('o');sequence=$command.sequence;action=$Event.action;button=$Event.button;axis=$Event.axis;value=$Event.value;acknowledged=$false;applied_unix_ms=$null}
}
function Update-DiagnosticInputAcknowledgements([string]$Root,[string]$Session,$Auto) {
    $path=Resolve-ProjectChild $Root ($Session+'/control-acks')
    foreach($input in $Auto.inputs){
        if($input.acknowledged){continue}
        $ackPath=Join-Path $path (([long]$input.sequence).ToString('D16')+'.json')
        Assert-ProjectPath $Root $ackPath
        if(-not(Test-Path -LiteralPath $ackPath -PathType Leaf)){continue}
        $ack=Read-JsonUtf8 $ackPath
        if($ack.sequence -ne $input.sequence -or $ack.action -ne $input.action){throw 'Control application acknowledgement mismatch.'}
        if($ack.PSObject.Properties['accepted'] -and -not $ack.accepted){throw ('Control command rejected: '+$ack.error)}
        $input.acknowledged=$true;$input.applied_unix_ms=$ack.applied_unix_ms
    }
    $Auto.workload_completed=($Auto.event_index -eq $Auto.events.Count -and
        @($Auto.inputs|Where-Object {-not $_.acknowledged}).Count -eq 0)
}
function Get-DiagnosticSceneTelemetry([string]$SessionPath) {
    foreach($name in @('shad_log.txt','CUSA00309.log','shadps4.log')){
        $path=Join-Path $SessionPath ('log/'+$name)
        if(-not(Test-Path -LiteralPath $path)){continue}
        # Read a bounded byte tail: verbose validation can bury the last Perf line far beyond 100 lines.
        $stream=[IO.FileStream]::new($path,[IO.FileMode]::Open,[IO.FileAccess]::Read,([IO.FileShare]::ReadWrite -bor [IO.FileShare]::Delete))
        try{
            $length=[int][Math]::Min(16MB,$stream.Length)
            $null=$stream.Seek(-$length,[IO.SeekOrigin]::End)
            $buffer=[byte[]]::new($length);$read=0
            while($read -lt $length){
                $count=$stream.Read($buffer,$read,$length-$read)
                if($count -eq 0){break};$read+=$count
            }
            $tail=[Text.Encoding]::UTF8.GetString($buffer,0,$read)
        }finally{$stream.Dispose()}
        $samples=[regex]::Matches($tail,'draws/frame\s+(\d+)\s+dispatches/frame\s+(\d+)')
        if($samples.Count){
            $last=$samples[$samples.Count-1]
            return ([int]$last.Groups[1].Value -ge 500 -and [int]$last.Groups[2].Value -ge 50)
        }
    }
    return $false
}
function Update-DiagnosticAutomation([string]$Root,[string]$Session,$Plan,$Auto,[double]$Elapsed) {
    $record=Read-ProjectSession $Root $Session;$sessionPath=$record.Path
    $dir=Join-Path $sessionPath 'diagnostics';$nativeCommandSent=$false
    if(-not $Auto.ready -and $Elapsed -ge $Plan.automation.boot_timeout_seconds){throw 'Automatic load did not produce gameplay evidence before the timeout.'}
    if($null -ne $Auto.pending_screen){
        $screensPath=Join-Path $sessionPath 'screenshots';Assert-ProjectPath $Root $screensPath
        $latest=@(Get-ChildItem -LiteralPath $screensPath -File -Filter '*.png' -ErrorAction SilentlyContinue|Where-Object {$_.LastWriteTimeUtc -ge $Auto.screen_requested_utc}|Sort-Object LastWriteTimeUtc -Descending|Select-Object -First 1)
        if($latest.Count){Assert-ProjectPath $Root $latest[0].FullName}
        if($latest.Count -and (Test-DiagnosticPngReady $latest[0].FullName)){
            $screen=Read-DiagnosticScreen $latest[0].FullName
            $decision=Get-DiagnosticMenuDecision $screen.text $screen.selected
            $Auto.screens+=@{elapsed_s=$Elapsed;image_written_utc=$latest[0].LastWriteTimeUtc.ToString('o');recognized_utc=[DateTime]::UtcNow.ToString('o');path=(Get-RelativePath $Root $latest[0].FullName);text=$screen.text.Substring(0,[Math]::Min(2000,$screen.text.Length));decision=$decision}
            $Auto.pending_screen=$null
            if($null -ne $Auto.capture_at){
                if($decision.action -ne 'wait' -or $decision.reason -eq 'loading'){
                    $Auto.input_suspended=$true
                    if(-not $Auto.warnings.Count){$Auto.warnings+='A menu/loading screen appeared during collection; workload input was suspended.'}
                }
            }elseif(-not $Auto.ready){
                $telemetry=Get-DiagnosticSceneTelemetry $sessionPath
                if($Auto.pause_probe_pending){
                    $response=Get-DiagnosticPauseResponse $decision ([bool]$Auto.pause_return_sent) ($Elapsed-$Auto.last_menu_input) $telemetry
                    if($response.action -eq 'return'){
                        Send-DiagnosticAutoInput $Root $Session $Auto @{action='button';button=$response.button;duration_ms=1000;axis=$null;value=$null} $Elapsed
                        $Auto.pause_return_sent=$true
                        $Auto.last_menu_input=$Elapsed;$Auto.phase='resume-probe';$Auto.candidate_count=0
                    }elseif($response.action -eq 'resumed'){
                        $Auto.pause_probe_pending=$false;$Auto.pause_verified=$true
                        $Auto.last_menu_input=$Elapsed;$Auto.candidate_count=0
                    }elseif($response.action -eq 'fail'){
                        throw ('Automatic pause verification stopped: '+$response.reason)
                    }
                }elseif($decision.action -eq 'blocked'){
                    throw ('Automatic menu navigation stopped: '+$decision.reason)
                }elseif($decision.action -eq 'button' -and $Elapsed-$Auto.last_menu_input -ge 8){
                    $button=$decision.button
                    if($button -eq 'confirm'){
                        # CUSA00309 01.07 displays Circle=confirm, Cross=back even when
                        # the emulator's global circle-enter preference is false.
                        $button='circle'
                    }
                    Send-DiagnosticAutoInput $Root $Session $Auto @{action='button';button=$button;duration_ms=1000;axis=$null;value=$null} $Elapsed
                    $Auto.last_menu_input=$Elapsed
                    if($decision.reason -eq 'continue-save'){$Auto.continue_sent=$true;$Auto.phase='loading'}
                    $Auto.candidate_count=0
                }elseif(Test-DiagnosticGameplayCandidate $decision ([bool]($Auto.continue_sent -or $Auto.pause_verified)) ($Elapsed-$Auto.last_menu_input) $telemetry){
                    $Auto.scene_evidence=if($decision.reason -eq 'gameplay-hud'){'objective-hud-and-render-counters'}elseif($Auto.pause_verified){'pause-response-and-render-counters'}else{'continue-menu-and-render-counters'}
                    $Auto.candidate_count++
                    if($Auto.candidate_count -ge 2){
                        $Auto.ready=$true;$Auto.ready_at=$Elapsed;$Auto.phase='warmup';$Auto.scene_status='gameplay-candidate'
                    }
                }elseif(Test-DiagnosticPauseProbe $decision ([bool]($Auto.continue_sent -or $Auto.pause_verified)) ([int]$Auto.pause_probe_attempts) ($Elapsed-$Auto.last_menu_input) $telemetry){
                    Send-DiagnosticAutoInput $Root $Session $Auto @{action='button';button='options';duration_ms=1000;axis=$null;value=$null} $Elapsed
                    $Auto.pause_probe_attempts++;$Auto.pause_probe_pending=$true
                    $Auto.last_menu_input=$Elapsed;$Auto.phase='pause-probe';$Auto.candidate_count=0
                }else{$Auto.candidate_count=0}
            }
            $Auto.next_screen=$Elapsed+$(if($Auto.pause_probe_pending){3}else{12})
        }elseif($Auto.ready -and $Elapsed-$Auto.pending_screen -gt 25){throw 'The engine did not produce the requested scene screenshot.'}
    }
    # Readiness screenshots finish before capture. OCR must not stall scheduled input.
    if($null -eq $Auto.capture_at -and $null -eq $Auto.pending_screen -and $Elapsed -ge $Auto.next_screen -and $Auto.screen_count -lt 24){
        & (Join-Path $PSScriptRoot 'control.ps1') -Action screenshot -Session $Session | Out-Null
        $Auto.screen_requested_utc=[DateTime]::UtcNow.AddSeconds(-1)
        $Auto.pending_screen=$Elapsed;$Auto.screen_count++;$Auto.next_screen=$Elapsed+12;$nativeCommandSent=$true
    }
    if($null -ne $Auto.capture_at -and -not $Auto.input_suspended){
        $position=$Elapsed-$Auto.capture_at
        while($Auto.event_index -lt $Auto.events.Count -and $position -ge $Auto.events[$Auto.event_index].at){
            $event=$Auto.events[$Auto.event_index];$Auto.event_index++
            $normalized=@{action=$event.action;button=$null;axis=$null;value=$null}
            foreach($key in $event.Keys){$normalized[$key]=$event[$key]}
            Send-DiagnosticAutoInput $Root $Session $Auto $normalized $Elapsed
        }
        if($Plan.profile -in @('all','full','shaders','compute','draw') -and -not $Auto.xray_triggered -and $position -ge $Plan.capture_schedule.guest_trace_seconds){
            Set-DiagnosticTuning $Root $sessionPath @{dump_frames=2};$Auto.xray_triggered=$true
        }
        if((Get-DiagnosticPlanSpec $Plan).RenderDoc -and -not $Auto.capture_trigger_attempted -and $position -ge $Plan.capture_schedule.renderdoc_seconds){
            $Auto.capture_trigger_attempted=$true
            try{
                & (Join-Path $PSScriptRoot 'control.ps1') -Action capture -Session $Session | Out-Null
                $Auto.capture_sequence=[long](Read-JsonUtf8 (Join-Path $sessionPath 'command.json')).sequence
            }catch{
                if($Plan.profile -ne 'full'){throw}
                $Auto.warnings+=('RenderDoc trigger: '+$_.Exception.Message)
            }
        }
    }
    Update-DiagnosticInputAcknowledgements $Root $Session $Auto
    if($null -ne $Auto.capture_sequence -and -not $Auto.capture_triggered){
        $ackPath=Join-Path $sessionPath ('control-acks/'+([long]$Auto.capture_sequence).ToString('D16')+'.json')
        Assert-ProjectPath $Root $ackPath
        if(Test-Path -LiteralPath $ackPath -PathType Leaf){
            $ack=Read-JsonUtf8 $ackPath
            if($ack.PSObject.Properties['accepted'] -and -not $ack.accepted){throw ('RenderDoc request rejected: '+$ack.error)}
            if($ack.sequence -ne $Auto.capture_sequence -or $ack.action -ne 'capture' -or -not $ack.renderdoc_ready){throw 'RenderDoc API did not acknowledge the frame request.'}
            $Auto.capture_triggered=$true;$Auto.capture_triggered_utc=[DateTime]::UtcNow.ToString('o')
        }
    }
    if($Auto.capture_triggered -and -not $Auto.capture_completed){
        $Auto.capture_completed=@(Get-ChildItem -LiteralPath (Join-Path $sessionPath 'user/captures') -Filter '*.rdc' -File -Recurse -ErrorAction SilentlyContinue|Where-Object {$_.Length -gt 0 -and (Test-DiagnosticCaptureClosed $_.FullName)}).Count -gt 0
    }
    Write-JsonUtf8 (Join-Path $dir 'automation.json') $Auto
}
function Reset-DiagnosticAutoInput([string]$Root,[string]$Session) {
    $record=Read-ProjectSession $Root $Session
    if(Get-DiagnosticTarget $record.Launch $Root){
        & (Join-Path $PSScriptRoot 'control.ps1') -Action axis -Axis 2 -Value 128 -Session $Session | Out-Null
    }
}

function Wait-DiagnosticAutomaticGame([string]$Root,[string]$Session,$Game,$Collector,$Plan) {
    $clock=[Diagnostics.Stopwatch]::StartNew()
    while(-not $Game.WaitForExit(1000)){
        if($Collector -and $Collector.HasExited -and $Game.WaitForExit(5000)){break}
        if(-not $Collector -or $Collector.HasExited -or $clock.Elapsed.TotalSeconds -gt $Plan.automation.session_timeout_seconds){
            $reason=if(-not $Collector -or $Collector.HasExited){'collector-ended-before-game'}else{'session-timeout'}
            try{& (Join-Path $PSScriptRoot 'control.ps1') -Action quit -Session $Session | Out-Null}catch{}
            $forced=$false
            if(-not $Game.WaitForExit(30000)){
                $record=Read-ProjectSession $Root $Session
                $target=Get-DiagnosticTarget $record.Launch $Root
                if(-not $target -or $target.Id -ne $Game.Id){throw 'Cannot validate the diagnostic process for timeout cleanup.'}
                $target.Kill();$target.WaitForExit();$target.Dispose();$forced=$true
            }
            $record=Read-ProjectSession $Root $Session
            Write-JsonUtf8 (Join-Path $record.Path 'diagnostics/exit-watchdog.json') @{reason=$reason;forced_exit=$forced;performance_acceptance=$false}
            if($forced -or $reason -eq 'session-timeout'){throw ('Automatic session ended: '+$reason+'; forced_exit='+$forced)}
            break
        }
    }
}
function Wait-DiagnosticAutomaticCollector([string]$Root,[string]$Session,$Collector,$Plan) {
    # Finalization uses the installed parsers and may outlast the gameplay window.
    # Keep waiting without a blocking three-minute call; the worker monitors free space.
    $cleanupClock=[Diagnostics.Stopwatch]::StartNew()
    while(-not $Collector.WaitForExit(1000)){
        if($cleanupClock.Elapsed.TotalSeconds -ge 7500){break}
    }
    if($Collector.HasExited){return}
    # The hidden collector is ours; bound its cleanup and preserve a failure record.
    $Collector.Kill();$Collector.WaitForExit()
    $record=Read-ProjectSession $Root $Session
    $dir=Join-Path $record.Path 'diagnostics';$errors=@('Collector cleanup timed out.')
    $status=Join-Path $dir 'status.json'
    if(Test-Path -LiteralPath $status){
        $state=Read-JsonUtf8 $status
        if($state.PSObject.Properties['helpers']){foreach($helper in $state.helpers){
            $owned=Get-Process -Id $helper.pid -ErrorAction SilentlyContinue
            if($owned){
                try{
                    if($owned.Path -eq $helper.path -and $owned.StartTime.ToUniversalTime().ToString('o') -eq $helper.started){
                        $owned.Kill();$null=$owned.WaitForExit(5000)
                    }
                }catch{$errors+=$_.Exception.Message}finally{$owned.Dispose()}
            }
        }}
    }
    if($Plan.wpr_profile){
        try{
            Invoke-DiagnosticNative (Get-DiagnosticTools).wpr @('-stop',(Join-Path $dir 'watchdog-recovered.etl'),'-instancename',$Plan.wpr_instance) (Join-Path $dir 'watchdog-wpr.log')
        }catch{$errors+=$_.Exception.Message}
    }
    Write-JsonUtf8 (Join-Path $dir 'collector-watchdog.json') @{errors=$errors;performance_acceptance=$false}
    throw 'The session collector exceeded its cleanup timeout; see collector-watchdog.json.'
}
