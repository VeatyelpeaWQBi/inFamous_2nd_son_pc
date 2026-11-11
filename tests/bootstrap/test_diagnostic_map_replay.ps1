# SPDX-License-Identifier: GPL-2.0-or-later
# Offline state-machine replay of the recorded OPTIONS-map failure. No game,
# OCR engine, physical input or profiler is started; commands are recorded locally.
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot '../../scripts/tools.ps1')
. (Join-Path $PSScriptRoot '../../scripts/diagnostic_automation.ps1')
$projectRoot=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
$relative='state/sessions/offline-map-replay-'+[Guid]::NewGuid().ToString('N')
$fixture=Resolve-ProjectChild $projectRoot $relative
$manifest=Read-JsonUtf8 (Join-Path $projectRoot 'project.json')
$script:replayScreen='GEORGETOWN'
$script:replayButtons=[Collections.Generic.List[string]]::new()
function Read-DiagnosticScreen([string]$Path) {
    return @{text=$script:replayScreen;selected=''}
}
function Test-DiagnosticPngReady([string]$Path) {return $true}
function Send-DiagnosticAutoInput([string]$Root,[string]$Session,$Auto,$Event,[double]$Elapsed) {
    if($Event.action -ne 'button'){throw 'Unexpected replay input action.'}
    $script:replayButtons.Add($Event.button)
}
function Update-DiagnosticInputAcknowledgements([string]$Root,[string]$Session,$Auto) {}
try{
    $null=[IO.Directory]::CreateDirectory((Join-Path $fixture 'diagnostics'))
    $null=[IO.Directory]::CreateDirectory((Join-Path $fixture 'screenshots'))
    [IO.File]::WriteAllText((Join-Path $fixture 'screenshots/replay.png'),'offline screenshot placeholder')
    Write-JsonUtf8 (Join-Path $fixture 'launch.json') @{
        schema=3;path_base='project';session=$relative;runtime=$manifest.runtime_path
        runtime_sha256=$manifest.runtime_sha256
    }
    $plan=@{profile='cpu';seconds=60;automation=@{boot_timeout_seconds=360}}
    $auto=New-DiagnosticAutomation $plan
    $auto.events=@()
    $recordedMap="CURRENT LOCATION`nSEATTLE, WA`nGEORGETOWN`nMOBILE COMMAND`nSCANNING STATION`nSECURITY CAMERA`nGENOME ANALYSIS`nPOWERS`n@ Details`nO Options and Saves"
    foreach($step in @(
        @{at=16;text='GEORGETOWN';buttons=1;ready=$false;verified=$false},
        @{at=21;text=$recordedMap;buttons=2;ready=$false;verified=$false},
        @{at=26;text=$recordedMap;buttons=2;ready=$false;verified=$false},
        @{at=30;text='GEORGETOWN';buttons=2;ready=$false;verified=$true},
        @{at=46;text='GEORGETOWN';buttons=2;ready=$false;verified=$true},
        @{at=58;text='GEORGETOWN';buttons=2;ready=$true;verified=$true}
    )){
        $script:replayScreen=$step.text
        $auto.pending_screen=$step.at-1
        $auto.screen_requested_utc=[DateTime]::UtcNow.AddMinutes(-1)
        $auto.next_screen=$step.at+100
        Write-JsonUtf8 (Join-Path $fixture 'status.json') @{
            schema=2;observed_unix_ms=[DateTimeOffset]::UtcNow.ToUnixTimeMilliseconds()
            draws_per_frame=7000;dispatches_per_frame=400
        }
        Update-DiagnosticAutomation $projectRoot $relative $plan $auto $step.at
        if($replayButtons.Count -ne $step.buttons -or $auto.ready -ne $step.ready -or
            $auto.pause_verified -ne $step.verified){throw ('Unexpected map replay transition at '+$step.at+' seconds.')}
    }
    if(($replayButtons -join ',') -ne 'options,options'){throw 'Map probe must contain exactly one open and one close toggle.'}
    if($auto.scene_evidence -ne 'pause-response-and-render-counters'){throw 'A closed map must still require native rendering evidence.'}
    Write-Host 'PASS: actual navigation loop replays world -> OPTIONS map -> close -> two gameplay candidates. No game started.'
}finally{
    $absolute=[IO.Path]::GetFullPath($fixture)
    $allowed=[IO.Path]::GetFullPath((Join-Path $projectRoot 'state/sessions')).TrimEnd('\')+'\'
    if(-not $absolute.StartsWith($allowed,[StringComparison]::OrdinalIgnoreCase) -or
        [IO.Path]::GetFileName($absolute) -notmatch '^offline-map-replay-[a-f0-9]{32}$'){
        throw 'Refusing cleanup outside the isolated map replay fixture.'
    }
    if(Test-Path -LiteralPath $absolute){Remove-Item -LiteralPath $absolute -Recurse -Force}
}
