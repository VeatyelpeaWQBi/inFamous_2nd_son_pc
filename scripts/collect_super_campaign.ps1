# SPDX-License-Identifier: GPL-2.0-or-later
[CmdletBinding()]
param(
    [ValidateSet('writer-footprint')][string]$Candidate='writer-footprint',
    [ValidateSet('1080p','1440p','pro4k')][string]$Mode='pro4k',
    [ValidateSet('lightweight','counters')][string]$Profile='lightweight',
    [switch]$ScreenOnly,
    [switch]$PlanOnly
)
Set-StrictMode -Version Latest
$ErrorActionPreference='Stop'

$orders=if($ScreenOnly){@('AB')}else{@('AB','BA')}
$plan=@(foreach($order in $orders){[ordered]@{candidate=$Candidate;order=$order}})
Write-Host ("{0} {1}: {2} independent pairs, at most {3} releases." -f $Mode,$Profile,$plan.Count,($plan.Count*2))
foreach($step in $plan){Write-Host ("  {0} {1}" -f $step.candidate,$step.order)}
if($PlanOnly){return}
if($PSVersionTable.PSVersion.Major -ne 5){throw 'Use diagnostics/super_campaign.bat with installed Windows PowerShell 5.1.'}

. (Join-Path $PSScriptRoot 'tools.ps1')
$root=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$python=Resolve-InstalledPython
$manifestPath=Join-Path $root 'project.json'
$manifest=Read-JsonUtf8 $manifestPath
$runtime=Resolve-ProjectChild $root $manifest.runtime_path
if(Get-Process -Name ([IO.Path]::GetFileNameWithoutExtension($runtime)) -ErrorAction SilentlyContinue){
    throw 'Close the running game before starting the campaign.'
}
$snapshot=Invoke-ProjectNativeTool 'build-identity' @('snapshot')
if($LASTEXITCODE -ne 0){throw 'Could not verify the current source snapshot.'}
$source=($snapshot | ConvertFrom-Json).source_sha256
if($manifest.runtime_revision -ne ('project-snapshot:'+$source)){
    throw 'The active EXE predates the current source. Run build.bat, then start the campaign.'
}
$powerShell=Join-Path $PSHOME 'powershell.exe'
$area=Resolve-ProjectChild $root 'state/super-captures'
$null=[IO.Directory]::CreateDirectory($area)
$lock=[IO.File]::Open((Join-Path $area 'super-campaign.lock'),[IO.FileMode]::OpenOrCreate,[IO.FileAccess]::ReadWrite,[IO.FileShare]::None)
try{
    $name=(Get-Date -Format 'yyyyMMdd-HHmmss')+'-'+[Guid]::NewGuid().ToString('N')
    $directory=Join-Path $area ('campaign-'+$name)
    $null=[IO.Directory]::CreateDirectory($directory)
    $report=Join-Path $directory 'campaign.json'
    $summary=[ordered]@{
        schema=1; status='collecting'; mode=$Mode; profile=$Profile
        runtime_sha256=$manifest.runtime_sha256; source_sha256=$source
        planned_releases=$plan.Count*2; plan=$plan; results=@()
        performance_acceptance=$false
        started_utc=[DateTime]::UtcNow.ToString('o'); finished_utc=$null
    }
    Write-JsonUtf8 $report $summary
    Write-Host ('Campaign report: '+$report)
    foreach($step in $plan){
        $id=[Guid]::NewGuid().ToString('N')
        $result=[ordered]@{
            candidate=$step.candidate; order=$step.order; status='collecting'
            pair_id=$id; pair_index=$null; analysis=$null; sessions=@()
            capture_exit_code=$null; mechanism=$null; windows=$null; issues=@()
        }
        $summary.results+=@($result)
        Write-JsonUtf8 $report $summary
        try{
            if((Read-JsonUtf8 $manifestPath).runtime_sha256 -ne $manifest.runtime_sha256){
                throw 'The active executable changed during the campaign.'
            }
            & $powerShell -NoLogo -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot 'collect_feedback_pair.ps1') -Candidate $step.candidate -Order $step.order -Mode $Mode -Profile $Profile -PairId $id
            $result.capture_exit_code=$LASTEXITCODE
            $indexes=@(Get-ChildItem -LiteralPath $area -File -Filter "*-pair-$id.json")
            if($indexes.Count -ne 1){throw 'This pair did not publish a unique index.'}
            $result.pair_index=Get-RelativePath $root $indexes[0].FullName
            $index=Read-JsonUtf8 $indexes[0].FullName
            $result.sessions=@($index.sessions)
            if($result.capture_exit_code -ne 0 -or $index.status -ne 'collected'){
                throw ('Pair did not finish: '+$index.error)
            }
            $analysisPath=Join-Path $directory ($step.candidate+'-'+$step.order+'.json')
            & $python -B (Join-Path $PSScriptRoot 'analyze_feedback_pair.py') $indexes[0].FullName --output $analysisPath
            if($LASTEXITCODE -ne 0){throw 'Offline pair analysis failed. Raw sessions are retained.'}
            $analysis=Read-JsonUtf8 $analysisPath
            $result.analysis=Get-RelativePath $root $analysisPath
            $result.mechanism=$analysis.mechanism
            $result.windows=$analysis.windows
            $result.issues=@($analysis.issues)
            $runtimeChanged=@($analysis.sessions | Where-Object {$_.runtime_sha256 -ne $manifest.runtime_sha256})
            if($runtimeChanged.Count){$result.issues+='Runtime differs from the campaign executable.'}
            $result.status=if(-not $analysis.data_checks_passed -or $runtimeChanged.Count){'excluded'}
                elseif(-not $analysis.mechanism.coverage_observed){'path-not-observed'}
                else{'ready-for-review'}
        }catch{
            $result.status='failed'
            $result.issues+=@($_.Exception.Message)
            Write-Warning ($step.candidate+' '+$step.order+': '+$_.Exception.Message)
        }finally{
            Write-JsonUtf8 $report $summary
        }
    }
    $summary.status=if(@($summary.results | Where-Object {$_.status -eq 'failed'}).Count){'partial'}else{'finished'}
    $summary.finished_utc=[DateTime]::UtcNow.ToString('o')
    Write-JsonUtf8 $report $summary
    Write-Host ('Campaign '+$summary.status+'. Report: '+$report)
    if($summary.status -eq 'partial'){exit 1}
}finally{
    $lock.Dispose()
}
