[CmdletBinding()]
param()
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot '..\..\scripts\tools.ps1')
$projectRoot=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..'))
$testRoot=Resolve-ProjectChild $projectRoot ('state/offline-tests/'+[Guid]::NewGuid().ToString('N'))
$null=[IO.Directory]::CreateDirectory($testRoot)
try{
    $target=Join-Path $testRoot "设置 ' 测试.json"
    Write-JsonUtf8 $target ([ordered]@{value='first'})
    Write-JsonUtf8 $target ([ordered]@{value='second'})
    if((Get-Content -LiteralPath $target -Raw|ConvertFrom-Json).value -ne 'second'){throw 'Existing JSON was not atomically replaced.'}
    $source=Join-Path $testRoot 'source.json'
    Write-JsonUtf8 $source ([ordered]@{value='copied'})
    Copy-ProjectTree $projectRoot $source $target
    if((Get-Content -LiteralPath $target -Raw|ConvertFrom-Json).value -ne 'copied'){throw 'Existing copied file was not atomically replaced.'}
    if(-not (Test-Path -LiteralPath $source)){throw 'Atomic copy consumed its source.'}
    if(@(Get-ChildItem -LiteralPath $testRoot -Filter '*.pending').Count){throw 'Temporary replacement files were left behind.'}
    Write-Host 'PowerShell JSON replacement and safe copy checks passed.'
}finally{
    Assert-ProjectPath $projectRoot $testRoot
    foreach($item in Get-ChildItem -LiteralPath $testRoot -File -Force){
        Assert-ProjectPath $projectRoot $item.FullName
        Remove-Item -LiteralPath $item.FullName -Force
    }
    [IO.Directory]::Delete($testRoot)
}
