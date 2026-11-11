# SPDX-License-Identifier: GPL-2.0-or-later
# Two independent releases with a scoped compiler candidate and normal-build restoration.
[CmdletBinding()]
param([ValidateSet('AB','BA')][string]$Order='AB',[string]$PgoTraining)
Set-StrictMode -Version Latest
$ErrorActionPreference='Stop'
if($PSVersionTable.PSVersion.Major -ne 5){throw 'Use the installed Windows PowerShell 5.1.'}
. (Join-Path $PSScriptRoot 'tools.ps1')
. (Join-Path $PSScriptRoot 'build_tools.ps1')
$root=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$candidate=if($PgoTraining){'renderer-pgo'}else{'renderer-inline'}
$training=$null;$reference=$null;$referencePath=$null
if($PgoTraining){
    $trainingPath=Resolve-ProjectChild $root $PgoTraining
    $training=Read-JsonUtf8 $trainingPath
    if($training.status -ne 'trained' -or -not $training.baseline_restored){
        throw 'Renderer PGO requires a completed training session and restored normal build.'
    }
    $profilePath=Resolve-ProjectChild $root $training.profile
    if((Get-FileHash -LiteralPath $profilePath -Algorithm SHA256).Hash.ToLowerInvariant() -cne $training.profile_sha256){
        throw 'Renderer PGO profile differs from the indexed training artifact.'
    }
    $referencePath=Join-Path (Split-Path -Parent $trainingPath) 'Off-compiler.json'
    $reference=Read-JsonUtf8 $referencePath
}
$area=Join-Path $root ('state/runtime/'+$candidate+'-'+(Get-Date -Format 'yyyyMMdd-HHmmss')+'-'+[Guid]::NewGuid().ToString('N').Substring(0,8))
$null=[IO.Directory]::CreateDirectory($area)
$python=Resolve-InstalledPython
$shell=Join-Path $PSHOME 'powershell.exe'
$state=@{schema=1;candidate=$candidate;order=$Order;status='preparing';trials=@();phase='';error=$null;baseline_restored=$false;performance_acceptance=$false}
if($training){$state.profile_sha256=$training.profile_sha256;$state.training=Get-RelativePath $root $trainingPath}
$index=Join-Path $area 'pair.json'
$restore=$false
$lock=[IO.File]::Open((Join-Path $root ('state/runtime/'+$candidate+'.lock')),[IO.FileMode]::OpenOrCreate,[IO.FileAccess]::ReadWrite,[IO.FileShare]::None)
function Save-Pair {$state.updated_utc=[DateTime]::UtcNow.ToString('o');Write-JsonUtf8 $index $state}
function Build-Variant([bool]$Enabled){
    if(@(Get-Process -Name 'inFAMOUS Second Son' -ErrorAction SilentlyContinue).Count){throw 'A game is running; this compiler experiment will not build or take over that process.'}
    $arguments=@('-NoLogo','-NoProfile','-ExecutionPolicy','Bypass','-File',(Join-Path $PSScriptRoot 'build.ps1'))
    if($training){
        if($Enabled){$arguments+=@('-RendererPgo','Use','-RendererPgoData',$training.profile)}
        Invoke-ProjectBuildProcess $shell $arguments $root @{SOURCE_DATE_EPOCH=[string]$training.source_date_epoch}
        return
    }
    if($Enabled){$arguments+='-RendererInlineCandidate'}
    & $shell @arguments
    $code=$LASTEXITCODE
    if($code -ne 0){throw ('Renderer variant build failed: '+$code)}
}
try{
    Save-Pair
    foreach($character in $Order.ToCharArray()){
        $label=[string]$character;$enabled=$label -eq 'B'
        $state.phase='building-'+$label;Save-Pair
        if($enabled){$restore=$true}
        Build-Variant $enabled
        if(-not $enabled){$restore=$false;$state.baseline_restored=$true}else{$state.baseline_restored=$false}
        $manifest=Read-JsonUtf8 (Join-Path $root 'Build/RelWithDebInfo/compiler-variant.json')
        $manifestPath=Join-Path $area ($label+'-compiler.json')
        if($training){
            $expectedMode=if($enabled){'Use'}else{'Off'}
            if($manifest.renderer_pgo -ne $expectedMode -or $manifest.enabled){throw 'Unexpected renderer PGO build mode.'}
            foreach($key in @('source_sha256','compiler_sha256','normalized_commands_sha256','generated_revision_sha256')){
                if($manifest.$key -cne $reference.$key){throw ('The trained renderer build differs in '+$key)}
            }
        }elseif([bool]$manifest.enabled -ne $enabled){
            throw 'The actual compiler variant does not match this trial.'
        }
        Write-JsonUtf8 $manifestPath $manifest
        if($training -and ($enabled -or $state.trials.Count -eq 1)){
            $a=if($label -eq 'B' -and $state.trials.Count -eq 0){$referencePath}else{Join-Path $area 'A-compiler.json'}
            $b=Join-Path $area 'B-compiler.json'
            & $python -X utf8 -X faulthandler -B (Join-Path $PSScriptRoot 'analyze_renderer_inlining.py') --check-pgo-build $a $b $training.profile_sha256
            if($LASTEXITCODE -ne 0){throw 'Renderer profile and build comparison failed before capture.'}
        }elseif(-not $training -and $state.trials.Count -eq 1){
            $a=Join-Path $area 'A-compiler.json';$b=Join-Path $area 'B-compiler.json'
            & $python -X utf8 -X faulthandler -B (Join-Path $PSScriptRoot 'analyze_renderer_inlining.py') --check-build $a $b
            if($LASTEXITCODE -ne 0){throw 'Compiler source/argument comparison failed; no second release started.'}
        }
        $state.phase='capturing-'+$label;Save-Pair
        $captureStarted=[DateTime]::UtcNow
        $captureArgs=@('-NoLogo','-NoProfile','-ExecutionPolicy','Bypass','-File',(Join-Path $PSScriptRoot 'collect_super.ps1'),
            '-Mode','pro4k','-Profile','lightweight','-Rounds','1','-Experiment','none','-MinimumWarmup','0','-MaximumWarmup','0')
        if($training -and $enabled){$captureArgs+=@('-RendererPgoTrial','Use')}
        & $shell @captureArgs
        $code=$LASTEXITCODE
        if($code -ne 0){throw ('A compiler trial failed; no retry or additional release: '+$code)}
        $capture=Read-JsonUtf8 (Join-Path $root 'state/super-captures/last-native-run.json')
        if($capture.status -ne 'collected' -or [DateTime]::Parse($capture.updated_utc).ToUniversalTime() -lt $captureStarted){throw 'No fresh successful compiler trial receipt.'}
        $state.trials+=@{label=$label;session=$capture.session;batch=$capture.batch;compiler_manifest=(Get-RelativePath $root $manifestPath)}
        Save-Pair
    }
    $state.status='collected'
}catch{
    $state.status='failed';$state.error=$_.Exception.Message
}finally{
    if($restore){
        $state.phase='restoring-baseline';Save-Pair
        try{Build-Variant $false;$state.baseline_restored=$true}
        catch{$state.status='failed';$state.error=([string]$state.error+' Baseline restore: '+$_.Exception.Message)}
    }
    $state.phase='finished';Save-Pair
    $lock.Dispose()
}
Write-Host ('Renderer compiler pair: '+$state.status+'; '+(Get-RelativePath $root $index))
if($state.status -ne 'collected'){throw $state.error}
