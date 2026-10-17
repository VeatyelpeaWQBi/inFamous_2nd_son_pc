# SPDX-License-Identifier: GPL-2.0-or-later
[CmdletBinding()]
param(
    [ValidateSet('menu','check','cpu','gpu','memory','compute','draw','validation','crash','shaders','renderdoc','nsight','vtune','vtune-memory','all','full','capture','stop','mark','xray','status','recover')]
    [string]$Action='full',
    [switch]$OptimizationV1,
    [ValidateSet('none','control','compile','readback','both')][string]$OptimizationV2='none'
)
Set-StrictMode -Version Latest
$ErrorActionPreference='Stop'
$root=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$powerShell=Join-Path $PSHOME 'powershell.exe'
$exitCode=1
$pauseOnExit=$true
$startedUtc=[DateTime]::UtcNow
$failure=$null
try{
    $identity=[Security.Principal.WindowsIdentity]::GetCurrent()
    try{$administrator=([Security.Principal.WindowsPrincipal]::new($identity)).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)}
    finally{$identity.Dispose()}
    if($Action -notin @('check','status') -and -not $administrator){
        # Elevate the same interactive entry, so failures remain visible in its window.
        $arguments=@('-NoLogo','-NoProfile','-ExecutionPolicy','Bypass','-File',('"'+$PSCommandPath+'"'),'-Action',$Action)
        if($OptimizationV1){$arguments+='-OptimizationV1'}
        if($OptimizationV2 -ne 'none'){$arguments+=@('-OptimizationV2',$OptimizationV2)}
        $child=Start-Process -FilePath $powerShell -ArgumentList $arguments -WorkingDirectory $root -Verb RunAs -Wait -PassThru
        $exitCode=$child.ExitCode
        $child.Dispose()
        $pauseOnExit=$false
    }else{
        Write-Host ('inFAMOUS Second Son - '+$Action)
        if($OptimizationV1){Write-Host 'V1 optimization test: both new candidates enabled for this session only.'}
        if($OptimizationV2 -ne 'none'){Write-Host ('V2 optimization test: '+$OptimizationV2+'; session only.')}
        if($Action -in @('menu','cpu','gpu','memory','compute','draw','validation','crash','shaders','renderdoc','nsight','vtune','vtune-memory','all','full')){
            Write-Host '无人值守采集：自动进入存档、预热、执行输入、抓帧和退出，无需按键。'
            Write-Host '预设 PRO 4K；最早启动 90 秒后采集，且须先取得场景证据并预热 20 秒；采集 60 秒。'
            Write-Host '采集副本临时使用英文菜单，便于自动识别。结果保存到 state/sessions。'
        }
        if($Action -in @('menu','full')){Write-Host '完整采集：只启动一次游戏，所有采集器关联同一进程；失败时不重启补采。'}
        Push-Location -LiteralPath $root
        try{
            # Run in a child console process: diagnose.ps1 may exit without skipping this entry's final prompt.
            $driverArguments=@('-NoLogo','-NoProfile','-ExecutionPolicy','Bypass','-File',(Join-Path $PSScriptRoot 'diagnose.ps1'),'-Action',$Action,'-Mode','pro4k','-DelaySeconds','90','-Seconds','60','-Frames','2')
            if($OptimizationV1){$driverArguments+='-OptimizationV1'}
            if($OptimizationV2 -ne 'none'){$driverArguments+=@('-OptimizationV2',$OptimizationV2)}
            & $powerShell @driverArguments
            $exitCode=$LASTEXITCODE
        }finally{Pop-Location}
        if($exitCode -ne 0){$failure='本次诊断未完整完成，请查看会话 status.json 或终端错误。';Write-Host ('未成功完成，退出码：'+$exitCode+'。请查看上方错误与会话记录。') -ForegroundColor Red}
    }
}catch{
    $exitCode=1
    $failure=$_.Exception.Message
    Write-Host $failure -ForegroundColor Red
}finally{
    if($pauseOnExit){
        try{
            . (Join-Path $PSScriptRoot 'tools.ps1')
            $statePath=Resolve-ProjectChild $root 'state'
            $null=[IO.Directory]::CreateDirectory($statePath)
            $session=$null;$captureStatus=$null;$runtimeIssues=$null;$currentErrors=@();$failureKind=$null;$storageUsage=$null
            try{
                $record=Read-ProjectSession $root $null
                if($record.Launch.PSObject.Properties['diagnostic'] -and $record.Launch.diagnostic -and
                    [DateTime]::Parse($record.Launch.prepared_at).ToUniversalTime() -ge $startedUtc){
                    $session=Get-RelativePath $root $record.Path
                    $capturePath=Join-Path $record.Path 'diagnostics/status.json'
                    if(Test-Path -LiteralPath $capturePath){
                        $capture=Read-JsonUtf8 $capturePath;$captureStatus=$capture.status;$currentErrors=@($capture.errors)
                        if($capture.PSObject.Properties['usage']){$storageUsage=$capture.usage}
                        if($capture.PSObject.Properties['failure_kind']){$failureKind=$capture.failure_kind}
                        if($currentErrors.Count){$failure=$currentErrors[0]}
                    }
                    $runtimeIssues=$session+'/diagnostics/runtime-errors.json'
                    if(-not(Test-Path -LiteralPath (Join-Path $record.Path 'diagnostics/runtime-errors.json'))){$runtimeIssues=$null}
                }
            }catch{}
            $result=@{
                schema=2;action=$Action;optimization_v1=[bool]$OptimizationV1;optimization_v2=$OptimizationV2;started_utc=$startedUtc.ToString('o');finished_utc=[DateTime]::UtcNow.ToString('o')
                exit_code=$exitCode;error=$failure;errors=$currentErrors;session=$session;status=$captureStatus
                failure_kind=$failureKind;runtime_errors_file=$runtimeIssues;storage_usage=$storageUsage;performance_acceptance=$false
            }
            Write-JsonUtf8 (Join-Path $statePath 'diagnostic-last-result.json') $result
            # Always replace the previous run's error record, including a successful run.
            # Original session records remain available; stale failures are never attributed to this run.
            $driverError=$null;$errorPath=Join-Path $statePath 'diagnostic-last-error.json'
            if(Test-Path -LiteralPath $errorPath){
                $previous=Read-JsonUtf8 $errorPath
                if($previous.PSObject.Properties['recorded_utc'] -and [DateTime]::Parse($previous.recorded_utc).ToUniversalTime() -ge $startedUtc){$driverError=$previous.error}
            }
            Write-JsonUtf8 $errorPath @{
                schema=2;action=$Action;session=$session;recorded_utc=$result.finished_utc;exit_code=$exitCode
                error=if($exitCode -ne 0){$failure}else{$null};details=$driverError;runtime_errors_file=$runtimeIssues
            }
            if($exitCode -ne 0){
                Write-Host ('本次失败原因：'+$failure) -ForegroundColor Red
                if($runtimeIssues){Write-Host ('运行时错误详情：'+$runtimeIssues)}
            }
            if($storageUsage -and $storageUsage.PSObject.Properties['total_bytes'] -and
                $storageUsage.PSObject.Properties['peak_total_bytes'] -and $storageUsage.PSObject.Properties['available_free_bytes']){
                Write-Host ('本轮磁盘占用：{0:F2} GiB；已观测峰值：{1:F2} GiB；磁盘剩余：{2:F2} GiB。' -f ($storageUsage.total_bytes/1GB),($storageUsage.peak_total_bytes/1GB),($storageUsage.available_free_bytes/1GB))
            }
            Write-Host '启动结果摘要：state/diagnostic-last-result.json'
        }catch{Write-Host $_.Exception.Message -ForegroundColor Red}
        $closeSeconds=if($exitCode -eq 0){8}else{30}
        Write-Host ('本次操作已结束，窗口将在 '+$closeSeconds+' 秒后关闭；结果已保存，无需按键。');Start-Sleep -Seconds $closeSeconds
    }
}
exit $exitCode
