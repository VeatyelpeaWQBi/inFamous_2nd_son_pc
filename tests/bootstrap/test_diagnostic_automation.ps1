# SPDX-License-Identifier: GPL-2.0-or-later
# Pure decision checks: no game, OCR, disk output, keyboard or controller access.
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot '../../scripts/diagnostic_automation.ps1')
$checks=@(
    @{text='Press OPTIONS to begin';selected='';action='button';button='options'},
    @{text="CONTINUE`nNEW GAME`nOPTIONS";selected='OPTIONS';action='button';button='up'},
    @{text="CONTINUE`nNEW GAME";selected='CONTINUE';action='button';button='confirm'},
    @{text="NEW GAME`nOPTIONS";selected='';action='blocked';button=$null},
    @{text="Overwrite saved data?`nYES`nNO";selected='';action='blocked';button=$null},
    @{text='Loading please wait';selected='';action='wait';button=$null},
    @{text='An unknown frame';selected='';action='wait';button=$null},
    @{text='Defeat Hank and find out what is happening to you';selected='';action='wait';button=$null},
    @{text='OPTIONS AND SAVE';selected='';action='blocked';button=$null}
)
foreach($check in $checks){
    $decision=Get-DiagnosticMenuDecision $check.text $check.selected
    if($decision.action -ne $check.action){throw ('Wrong menu action: '+$check.text)}
    if($check.button -and $decision.button -ne $check.button){throw ('Wrong menu input: '+$check.text)}
}
$hud=Get-DiagnosticMenuDecision 'Defeat Hank and find out what is happening to you'
if(-not (Test-DiagnosticGameplayCandidate $hud $false 0 $true)){throw 'An already-running mission must not require a continue-menu input.'}
if(Test-DiagnosticGameplayCandidate $hud $false 0 $false){throw 'HUD text alone must not permit workload input.'}
$unknown=Get-DiagnosticMenuDecision 'An unknown frame'
if(Test-DiagnosticGameplayCandidate $unknown $false 30 $true){throw 'Rendering alone must not classify an unknown frame as gameplay.'}
if(-not (Test-DiagnosticGameplayCandidate $unknown $true 30 $true)){throw 'The existing continue-menu readiness path must remain available.'}
$loading=Get-DiagnosticMenuDecision 'Loading please wait'
if(Test-DiagnosticGameplayCandidate $loading $true 30 $true){throw 'Loading must not start the workload.'}
$pause=Get-DiagnosticMenuDecision ('OPTIONS AND SAVE'+[Environment]::NewLine+'Defeat Hank and find out what is happening to you')
if(Test-DiagnosticGameplayCandidate $pause $true 30 $true){throw 'A paused scene must not start the workload.'}
$pauseWithResume=Get-DiagnosticMenuDecision ('RESUME GAME'+[Environment]::NewLine+'OPTIONS AND SAVE')
if($pauseWithResume.reason -ne 'pause-menu'){throw 'A pause probe must not confirm the highlighted resume/save row.'}
if(-not(Test-DiagnosticPauseProbe $unknown $false 0 30 $true)){throw 'An auto-resumed world needs a bounded pause probe.'}
foreach($blockedDecision in @($loading,$pause,(Get-DiagnosticMenuDecision 'Overwrite saved data?'))){
    if(Test-DiagnosticPauseProbe $blockedDecision $false 0 30 $true){throw 'A non-world screen was allowed to trigger a probe.'}
}
if(Test-DiagnosticPauseProbe $unknown $false 1 30 $true){throw 'Pause probing must never repeat blindly.'}
if(Test-DiagnosticPauseProbe $unknown $false 0 30 $false){throw 'Unknown pixels without render telemetry are not scene evidence.'}
if(Test-DiagnosticPauseProbe $unknown $true 0 30 $true){throw 'A verified scene does not need a pause probe.'}
$return=Get-DiagnosticPauseResponse $pauseWithResume $false 2 $true
if($return.action -ne 'return' -or $return.button -ne 'cross'){throw 'The verified HK pause page must use Cross=back, never Circle=save.'}
if((Get-DiagnosticPauseResponse $pauseWithResume $true 2 $true).action -ne 'wait'){throw 'An open pause menu was incorrectly marked resumed.'}
if((Get-DiagnosticPauseResponse $pauseWithResume $true 21 $true).action -ne 'fail'){throw 'Failed menu close must not repeat or proceed.'}
if((Get-DiagnosticPauseResponse $unknown $true 2 $true).action -ne 'resumed'){throw 'Closed menu plus render telemetry must confirm the resume response.'}
if((Get-DiagnosticPauseResponse (Get-DiagnosticMenuDecision 'Overwrite saved data?') $true 2 $true).action -ne 'fail'){throw 'A save confirmation must stop automation.'}
foreach($profile in @('cpu','gpu','memory','compute','draw','validation','crash','shaders','renderdoc','nsight','vtune','vtune-memory','all','full','v3-performance')){
    $events=@(Get-DiagnosticWorkload $profile 60)
    $last=-1
    foreach($event in $events){
        if($event.at -le $last -or $event.at -ge 60){throw 'Workload command times must be strictly ordered within the window.'}
        $last=$event.at
        if($event.action -eq 'axis' -and ($event.axis -notin $(if($profile -eq 'v3-performance'){@(1,2)}else{@(2)}) -or $event.value -lt 0 -or $event.value -gt 255)){throw 'Only bounded workload-axis changes are allowed.'}
        if($event.action -eq 'button' -and ($profile -in @('cpu','vtune') -or $event.button -notin @('square','circle'))){throw 'Unexpected workload button.'}
    }
    if($events.Count -and ($events[-1].action -ne 'axis' -or $events[-1].value -ne 128)){throw 'The final workload input must center the camera.'}
}
$v3Events=@(Get-DiagnosticWorkload 'v3-performance' 60)
if(@($v3Events|Where-Object {$_.action -eq 'button' -and $_.button -eq 'circle'}).Count -ne 1 -or
   @($v3Events|Where-Object {$_.action -eq 'button' -and $_.button -eq 'square'}).Count -ne 0 -or
   @($v3Events|Where-Object {$_.action -eq 'axis' -and $_.axis -eq 1}).Count -ne 2){
    throw 'V3 workload must rotate, walk once and perform one ability jump.'
}
Write-Host 'Diagnostic menu safety and workload scheduling checks passed.'
