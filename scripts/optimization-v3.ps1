[CmdletBinding()]
param(
    [ValidateSet('Set','Show','List','Run','Menu')][string]$Action='Show',
    [string]$Profile='baseline',
    [ValidateSet('720p','900p','1080p','1440p','1800p','pro4k')][string]$Mode='pro4k',
    [switch]$Automation
)
Set-StrictMode -Version Latest
$ErrorActionPreference='Stop'

$root=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$tuningPath=Join-Path $root 'state/play/user/perf_tuning.ini'
$keys=@(
    'v3_pipeline_reuse','v3_draw_packets','v3_raw_access_chains','v3_selective_ubo',
    'v3_barrier_merge','v3_dirty_log','v3_write_runs','v3_async_present',
    'v3_transient_device_local','v3_incremental_shader_index','v3_native_draw_records',
    'v3_particle_sort'
)
$labels=@(
    'graphics pipeline reuse','draw packet','NV raw access chains','selective UBO',
    'image barrier merge','verified-clean dirty log','bounded write runs',
    'ordered present recording','device-local transient streams','incremental shader index',
    'native draw record','guarded particle sort'
)
$profiles=[ordered]@{
    baseline=@()
    core=@(1,2,5,6,8,10,11)
    shader=@(3,4,10,12)
    gpu=@(3,4,5,8,9,12)
    memory=@(6,7,9)
    all=@(1,2,3,4,5,6,7,8,9,10,11,12)
}
for($i=1;$i -le 12;$i++){
    $profiles[('v3-{0:D2}' -f $i)]=@($i)
}
$profiles['baseline-final']=@()

function Resolve-V3Profile([string]$Name) {
    $normalized=$Name.Trim().ToLowerInvariant()
    if(-not $profiles.Contains($normalized)){
        throw ('Unknown V3 profile "'+$Name+'". Use -Action List to show valid names.')
    }
    return $normalized
}

function Ensure-TuningFile {
    $parent=Split-Path -Parent $tuningPath
    if(-not(Test-Path -LiteralPath $parent -PathType Container)){
        throw 'state/play/user is missing. Start the project once so its existing play configuration is available.'
    }
    if(Test-Path -LiteralPath $tuningPath -PathType Leaf){return}
    $baseline=Join-Path $root 'state/baseline/user/perf_tuning.ini'
    if(Test-Path -LiteralPath $baseline -PathType Leaf){
        [IO.File]::Copy($baseline,$tuningPath,$false)
    }else{
        [IO.File]::WriteAllText($tuningPath,'',[Text.UTF8Encoding]::new($false))
    }
}

function Set-IniValues([Collections.IDictionary]$Values) {
    Ensure-TuningFile
    $text=[IO.File]::ReadAllText($tuningPath)
    foreach($key in $Values.Keys){
        $line=$key+' = '+[string]$Values[$key]
        $pattern='(?m)^\s*'+[regex]::Escape([string]$key)+'\s*=.*$'
        if([regex]::IsMatch($text,$pattern)){
            $text=[regex]::Replace($text,$pattern,[Text.RegularExpressions.MatchEvaluator]{param($m) $line})
        }else{
            if($text.Length -and -not $text.EndsWith([Environment]::NewLine)){$text+=[Environment]::NewLine}
            $text+=$line+[Environment]::NewLine
        }
    }
    $pending=$tuningPath+'.'+[Guid]::NewGuid().ToString('N')+'.pending'
    $backup=$tuningPath+'.'+[Guid]::NewGuid().ToString('N')+'.backup'
    try{
        [IO.File]::WriteAllText($pending,$text,[Text.UTF8Encoding]::new($false))
        [IO.File]::Replace($pending,$tuningPath,$backup)
    }finally{
        if(Test-Path -LiteralPath $pending){Remove-Item -LiteralPath $pending -Force}
        if(Test-Path -LiteralPath $backup){Remove-Item -LiteralPath $backup -Force}
    }
}

function Set-V3Profile([string]$Name) {
    $resolved=Resolve-V3Profile $Name
    $enabled=@($profiles[$resolved])
    $values=[ordered]@{}
    for($i=0;$i -lt $keys.Count;$i++){
        $values[$keys[$i]]=if($enabled -contains ($i+1)){1}else{0}
    }
    if($Automation){
        # Lightweight engine counters only. No validation layer, profiler, frame dump or GPU
        # timestamp instrumentation is enabled by the V3 suite.
        $values['perf_log']=1
        $values['profile_stacks']=0
        $values['gpu_profile']=0
        $values['path_trace']=0
        $values['dump_frames']=0
        $values['gpu_checkpoints']=0
    }
    Set-IniValues $values
    Write-Host ('V3 profile: '+$resolved)
    for($i=0;$i -lt $keys.Count;$i++){
        Write-Host ('  {0:D2} {1}={2}  {3}' -f ($i+1),$keys[$i],$values[$keys[$i]],$labels[$i])
    }
    return $resolved
}

function Show-V3Profile {
    Ensure-TuningFile
    $text=[IO.File]::ReadAllText($tuningPath)
    Write-Host ('Tuning file: '+$tuningPath)
    for($i=0;$i -lt $keys.Count;$i++){
        $match=[regex]::Match($text,'(?m)^\s*'+[regex]::Escape($keys[$i])+'\s*=\s*([^\s#;]+)')
        $value=if($match.Success){$match.Groups[1].Value}else{'0 (default)'}
        Write-Host ('  {0:D2} {1}={2}  {3}' -f ($i+1),$keys[$i],$value,$labels[$i])
    }
}

function Show-V3Profiles {
    foreach($name in $profiles.Keys){
        $items=@($profiles[$name])
        $description=if($items.Count){($items|ForEach-Object {'{0:D2}' -f $_}) -join ','}else{'all off'}
        Write-Host ('{0,-16} {1}' -f $name,$description)
    }
    Write-Host ''
    for($i=0;$i -lt $keys.Count;$i++){
        Write-Host ('  {0:D2} {1,-32} {2}' -f ($i+1),$keys[$i],$labels[$i])
    }
}

if($Action -eq 'List'){Show-V3Profiles;exit 0}
if($Action -eq 'Show'){Show-V3Profile;exit 0}
if($Action -eq 'Menu'){
    while($true){
        Write-Host ''
        Show-V3Profiles
        $choice=Read-Host 'Profile name, show, or q'
        if($choice -eq 'q'){exit 0}
        if($choice -eq 'show'){Show-V3Profile;continue}
        Set-V3Profile $choice | Out-Null
        $launch=Read-Host ('Run '+$Mode+' now? [y/N]')
        if($launch -match '^(?i)y$'){
            & (Join-Path $root 'run.bat') $Mode '--no-menu'
            exit $LASTEXITCODE
        }
    }
}

$selected=Set-V3Profile $Profile
if($Action -eq 'Run'){
    & (Join-Path $root 'run.bat') $Mode '--no-menu'
    exit $LASTEXITCODE
}
exit 0
