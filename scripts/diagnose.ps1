# SPDX-License-Identifier: GPL-2.0-or-later
[CmdletBinding()]
param(
    [ValidateSet('menu','check','cpu','gpu','memory','compute','draw','validation','crash','shaders','renderdoc','nsight','vtune','vtune-memory','all','full','capture','stop','mark','xray','status','recover','worker')]
    [string]$Action='full',
    [ValidateSet('1080p','pro4k')][string]$Mode='pro4k',
    [ValidateRange(-1,3600)][int]$DelaySeconds=90,
    [ValidateRange(10,600)][int]$Seconds=60,

    [ValidateRange(2,30)][int]$Frames=2,
    [string]$Session,
    [switch]$PrepareOnly,[switch]$WaitForDebugger,[switch]$OptimizationV1
)
Set-StrictMode -Version Latest
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot 'tools.ps1')
. (Join-Path $PSScriptRoot 'diagnostic_tools.ps1')
$root=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
try{
    if($Action -eq 'menu'){$Action='full'}

    if($Action -eq 'check'){
        $tools=Get-DiagnosticTools
        Test-DiagnosticBootstrap
        if(-not(Test-Path -LiteralPath $tools.schema)){throw 'Installed WPT schema is unavailable.'}
        $settings=[Xml.XmlReaderSettings]::new()
        $settings.DtdProcessing=[Xml.DtdProcessing]::Prohibit
        $settings.XmlResolver=$null
        $settings.ValidationType=[Xml.ValidationType]::Schema
        $settings.Schemas.XmlResolver=$null
        $null=$settings.Schemas.Add($null,$tools.schema)
        $reader=[Xml.XmlReader]::Create((Join-Path $PSScriptRoot 'diagnostic.wprp'),$settings)
        try{while($reader.Read()){}}finally{$reader.Dispose()}
        $rows=@()
        foreach($name in @('wpr','wpa','xperf','gpuview','renderdoc','ngfx','vtune','nvidia_smi')){
            $path=$tools.$name
            $rows+=@{tool=$name;available=[bool]$path;version=if($path){(Get-Item -LiteralPath $path).VersionInfo.FileVersion}else{$null}}
        }
        Add-Type -AssemblyName System.Runtime.WindowsRuntime
        $ocr=[Windows.Media.Ocr.OcrEngine,Windows.Foundation,ContentType=WindowsRuntime]::AvailableRecognizerLanguages
        if(-not @($ocr|Where-Object LanguageTag -eq 'en-US').Count){throw 'Windows English OCR is unavailable.'}
        [pscustomobject]@{syntax='passed';wpr_schema='passed';automatic_menu_checks='passed';english_ocr=$true;administrator=(Test-DiagnosticAdministrator);sdk_layers_available=[bool]$tools.sdk;tools=$rows;game_started=$false}|ConvertTo-Json -Depth 5
        if(@($rows|Where-Object {-not $_.available -and $_.tool -ne 'nvidia_smi'}).Count -or -not $tools.sdk){exit 1}
        exit 0
    }
    if($Action -eq 'worker'){Invoke-DiagnosticWorker $root $Session;exit 0}
    if($Action -in @('capture','stop','mark','xray')){
        Send-DiagnosticControl $root $Session $Action $Frames
        Write-Host ('Queued diagnostic action: '+$Action)
        exit 0
    }
    if($Action -in @('status','recover')){
        $record=Read-ProjectSession $root $Session
        $dir=Join-Path $record.Path 'diagnostics'
        $plan=Read-JsonUtf8 (Join-Path $dir 'plan.json')
        if($plan.session -ne (Get-RelativePath $root $record.Path) -or $plan.wpr_instance -notmatch '^IFSS_[a-f0-9]{32}$'){throw 'Untrusted diagnostic plan.'}
        if($Action -eq 'status'){
            Write-Host ('Session: '+(Get-RelativePath $root $record.Path))
            $path=Join-Path $dir 'status.json'
            if(Test-Path -LiteralPath $path){Read-JsonUtf8 $path|ConvertTo-Json -Depth 18}else{Write-Host 'Collector has not written a status yet.'}
            exit 0
        }
        if(-not (Test-DiagnosticAdministrator)){throw 'Recovery needs an administrator PowerShell.'}
        if(Get-DiagnosticTarget $record.Launch $root){throw 'Exit the diagnostic game normally before recovery.'}
        $workerPath=Join-Path $dir 'worker.json'
        if(Test-Path -LiteralPath $workerPath){
            $worker=Read-JsonUtf8 $workerPath
            $process=Get-Process -Id $worker.pid -ErrorAction SilentlyContinue
            if($process -and $process.StartTime.ToUniversalTime().ToString('o') -eq $worker.started){throw 'Collector is still active; wait for it to finish.'}
        }
        $tools=Get-DiagnosticTools
        if(-not $plan.wpr_profile){throw 'This profile does not own a WPR recording.'}
        Invoke-DiagnosticNative $tools.wpr @('-stop',(Join-Path $dir 'recovered.etl'),'-instancename',$plan.wpr_instance) (Join-Path $dir 'recovery.log')
        Write-Host 'Saved this session WPR trace. Other recordings were not addressed.'
        exit 0
    }
    # Offline protocol and syntax checks run before an engine is started.
    Test-DiagnosticBootstrap
    $arguments=@{
        Mode=$Mode;Diagnostic=$Action;DiagnosticDelaySeconds=$DelaySeconds;DiagnosticSeconds=$Seconds

        NoMenu=$true;PrepareOnly=$PrepareOnly;WaitForDebugger=$WaitForDebugger;OptimizationV1=$OptimizationV1
    }
    if($Action -eq 'full'){Write-Host 'Full: one game process, one session, concurrent diagnostics; no restart or batch phases.'}
    & (Join-Path $PSScriptRoot 'launch.ps1') @arguments
    exit $LASTEXITCODE
}catch{
    $failureRecord=$_
    $details=Get-DiagnosticErrorInfo $failureRecord $root
    try{
        $stateDirectory=Resolve-ProjectChild $root 'state'
        $null=[IO.Directory]::CreateDirectory($stateDirectory)
        Write-JsonUtf8 (Join-Path $stateDirectory 'diagnostic-last-error.json') @{action=$Action;session=$Session;recorded_utc=[DateTime]::UtcNow.ToString('o');error=$details}
    }catch{Write-Warning ('Could not persist driver error: '+$_.Exception.Message)}
    Write-Error -Message $failureRecord.Exception.Message -ErrorAction Continue
    exit 1
}
