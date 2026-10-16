[CmdletBinding()]
param()
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot '..\..\scripts\tools.ps1')
$projectRoot=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..'))
$testRoot=Resolve-ProjectChild $projectRoot ('state/offline-tests/'+[Guid]::NewGuid().ToString('N'))
$null=[IO.Directory]::CreateDirectory($testRoot)
try{
    $target=Join-Path $testRoot "设置 ' 测试.json"
    $expected='无法删除要被替换的文件。亮度正常。'+[Environment]::NewLine+'inFAMOUS'
    Write-JsonUtf8 $target ([ordered]@{value=$expected})
    if((Read-JsonUtf8 $target).value -cne $expected){throw 'UTF-8 JSON did not preserve Unicode error text.'}
    $stream=[IO.File]::Open($target,[IO.FileMode]::Open,[IO.FileAccess]::Read,([IO.FileShare]::ReadWrite -bor [IO.FileShare]::Delete))
    $reader=$null
    try{
        $reader=[IO.StreamReader]::new($stream,[Text.UTF8Encoding]::new($false,$true),$true)
        Write-JsonUtf8 $target ([ordered]@{value='second'})
        if(($reader.ReadToEnd()|ConvertFrom-Json).value -cne $expected){throw 'An open reader lost its original complete snapshot.'}
        if((Read-JsonUtf8 $target).value -ne 'second'){throw 'Existing JSON was not atomically replaced.'}
    }finally{
        if($reader){$reader.Dispose()}else{$stream.Dispose()}
    }
    $stream=[IO.File]::Open($target,[IO.FileMode]::Open,[IO.FileAccess]::Read,[IO.FileShare]::Read)
    $blocked=$false
    try{
        try{Write-JsonUtf8 $target ([ordered]@{value='must not overwrite a locked file'})}
        catch{
            $cause=$_.Exception
            while($cause.InnerException){$cause=$cause.InnerException}
            if($cause -isnot [IO.IOException] -or ($cause.HResult -band 0xffff) -notin @(32,33,1175)){throw}
            $blocked=$true
        }
        if(-not $blocked){throw 'A reader denying deletion did not block atomic replacement.'}
        if((Read-JsonUtf8 $target).value -ne 'second'){throw 'A failed replacement damaged the last complete JSON.'}
        if(@(Get-ChildItem -LiteralPath $testRoot -Filter '*.pending').Count){throw 'Failed replacement left a pending file.'}
    }finally{$stream.Dispose()}
    Write-JsonUtf8 $target ([ordered]@{value='unlocked'})
    if((Read-JsonUtf8 $target).value -ne 'unlocked'){throw 'Replacement did not recover after the reader closed.'}
    $invalid=Join-Path $testRoot 'invalid.json'
    [IO.File]::WriteAllText($invalid,'{"value": invalid}',[Text.UTF8Encoding]::new($false))
    $rejected=$false
    try{$null=Read-JsonUtf8 $invalid}catch{
        if($_.Exception.Message -notlike 'Invalid JSON in *'){throw}
        $rejected=$true
    }
    if(-not $rejected){throw 'Malformed JSON must be rejected.'}
    $source=Join-Path $testRoot 'source.json'
    Write-JsonUtf8 $source ([ordered]@{value='copied'})
    Copy-ProjectTree $projectRoot $source $target
    if((Read-JsonUtf8 $target).value -ne 'copied'){throw 'Existing copied file was not atomically replaced.'}
    if(-not (Test-Path -LiteralPath $source)){throw 'Atomic copy consumed its source.'}
    if(@(Get-ChildItem -LiteralPath $testRoot -Filter '*.pending').Count){throw 'Temporary replacement files were left behind.'}
    $configPath=Join-Path $testRoot 'config.json'
    Write-JsonUtf8 $configPath @{General=@{home_dir='';addon_install_dir='game';font_dir='';sys_modules_dir='';install_dirs=@(@{path='game'})}}
    Convert-ProjectConfigPaths $configPath $projectRoot $testRoot
    $sessionConfig=Read-JsonUtf8 $configPath
    if((Resolve-ProjectPath $testRoot $sessionConfig.General.addon_install_dir) -ne (Join-Path $projectRoot 'game')){
        throw 'Session config no longer resolves to the project game directory.'
    }
    Convert-ProjectConfigPaths $configPath $testRoot $projectRoot
    $restored=Read-JsonUtf8 $configPath
    if($restored.General.addon_install_dir -ne 'game' -or $restored.General.install_dirs[0].path -ne 'game'){
        throw 'Config paths changed across a project/session round trip.'
    }
    Write-JsonUtf8 $configPath @{General=@{home_dir='../outside';addon_install_dir='game'}}
    $before=[IO.File]::ReadAllText($configPath);$rejected=$false
    try{Convert-ProjectConfigPaths $configPath $projectRoot $testRoot}catch{
        if($_.Exception.Message -ne 'Path escaped the project.'){throw};$rejected=$true
    }
    if(-not $rejected -or [IO.File]::ReadAllText($configPath) -cne $before){throw 'An external config path was accepted or damaged the stored file.'}
    Write-Host 'UTF-8 JSON, atomic files and config path boundary checks passed.'
}finally{
    Assert-ProjectPath $projectRoot $testRoot
    foreach($item in Get-ChildItem -LiteralPath $testRoot -File -Force){
        Assert-ProjectPath $projectRoot $item.FullName
        Remove-Item -LiteralPath $item.FullName -Force
    }
    [IO.Directory]::Delete($testRoot)
}
