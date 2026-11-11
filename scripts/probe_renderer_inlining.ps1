# SPDX-License-Identifier: GPL-2.0-or-later
# Compile one existing renderer TU in isolation. Never activate/link a game runtime.
[CmdletBinding()]
param()
Set-StrictMode -Version Latest
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot 'tools.ps1')
. (Join-Path $PSScriptRoot 'build_tools.ps1')
$root=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$build=Join-Path $root 'Build/RelWithDebInfo'
$commands=Read-JsonUtf8 (Join-Path $build 'compile_commands.json')
$entry=@($commands|Where-Object { $_.file.Replace('\','/').EndsWith('/src/video_core/renderer_vulkan/vk_pipeline_cache.cpp') })
if($entry.Count -ne 1){throw 'A unique existing renderer compilation command is required.'}
$entry=$entry[0]
$tools=Get-ProjectBuildTools $root
$environment=Get-ProjectBuildEnvironment $tools.CMAKE_CXX_COMPILER $root
if($entry.command -notmatch '^(?:"(?<exe>[^"]+)"|(?<exe>\S+))\s+(?<args>.*)$'){
    throw 'Unsupported existing compiler command format.'
}
if([IO.Path]::GetFileName($Matches.exe) -ne 'clang-cl.exe'){throw 'The existing compilation must use installed clang-cl.'}
$arguments=$Matches.args
if([regex]::Matches($arguments,'(?<!\S)/Ob1(?=\s)').Count -ne 1 -or
   [regex]::Matches($arguments,'(?<!\S)/F[od](?:"[^"]*"|\S+)').Count -ne 2 -or
   -not $arguments.Contains(' -c -- ')){
    throw 'Unexpected renderer flags; refusing to reuse output paths.'
}
$area=Join-Path $root ('state/runtime/inline-probe-'+(Get-Date -Format 'yyyyMMdd-HHmmss'))
$null=[IO.Directory]::CreateDirectory($area)
$object=Join-Path $area 'vk_pipeline_cache-ob2.obj'
$original=Join-Path $build 'CMakeFiles/shadps4.dir/src/video_core/renderer_vulkan/vk_pipeline_cache.cpp.obj'
$project=Read-JsonUtf8 (Join-Path $root 'project.json')
$runtime=Resolve-ProjectChild $root $project.runtime_path
$runtimeBefore=(Get-FileHash -LiteralPath $runtime -Algorithm SHA256).Hash
$objectBefore=(Get-FileHash -LiteralPath $original -Algorithm SHA256).Hash
$arguments=[regex]::Replace($arguments,'(?<!\S)/Ob1(?=\s)','/Ob2')
$arguments=[regex]::Replace($arguments,'(?<!\S)/F[od](?:"[^"]*"|\S+)','')
$outputs=' /Fo'+(Quote-WindowsArgument $object)+' /Fd'+(Quote-WindowsArgument (Join-Path $area 'compile.pdb'))+
    ' /clang:-Rpass=inline -c -- '
$arguments=$arguments.Replace(' -c -- ',$outputs)
[IO.File]::WriteAllText((Join-Path $area 'command.txt'),$arguments,[Text.UTF8Encoding]::new($false))
$state=@{status='compiling';source=$entry.file;compiler=$tools.CMAKE_CXX_COMPILER;object=$object
    original_object_sha256=$objectBefore;runtime_sha256=$runtimeBefore;runtime_activated=$false;game_started=$false}
Write-JsonUtf8 (Join-Path $area 'status.json') $state
$process=$null
try{
    $start=[Diagnostics.ProcessStartInfo]::new($tools.CMAKE_CXX_COMPILER)
    $start.Arguments=$arguments;$start.WorkingDirectory=$entry.directory
    $start.UseShellExecute=$false;$start.CreateNoWindow=$true
    $start.RedirectStandardOutput=$true;$start.RedirectStandardError=$true
    foreach($key in $environment.Keys){$start.EnvironmentVariables[$key]=$environment[$key]}
    $process=[Diagnostics.Process]::Start($start)
    $output=$process.StandardOutput.ReadToEndAsync();$errors=$process.StandardError.ReadToEndAsync()
    if(-not $process.WaitForExit(180000)){$process.Kill();$process.WaitForExit();throw 'Isolated renderer compile exceeded 180 seconds.'}
    [IO.File]::WriteAllText((Join-Path $area 'compiler.log'),$output.Result+$errors.Result,[Text.UTF8Encoding]::new($false))
    $state.exit_code=$process.ExitCode
    if($process.ExitCode -ne 0){throw ('Isolated renderer compilation failed: '+$process.ExitCode)}
    $state.object_bytes=(Get-Item -LiteralPath $object).Length
    $state.object_sha256=(Get-FileHash -LiteralPath $object -Algorithm SHA256).Hash
    $state.status='compiled'
}catch{
    $state.status='failed';$state.error=$_.Exception.Message
}finally{
    if($process){$process.Dispose()}
    $state.runtime_unchanged=(Get-FileHash -LiteralPath $runtime -Algorithm SHA256).Hash -eq $runtimeBefore
    $state.original_object_unchanged=(Get-FileHash -LiteralPath $original -Algorithm SHA256).Hash -eq $objectBefore
    if(-not $state.runtime_unchanged -or -not $state.original_object_unchanged){$state.status='failed';$state.error='Original build artifacts changed during the isolated compile.'}
    $state.updated_utc=[DateTime]::UtcNow.ToString('o')
    Write-JsonUtf8 (Join-Path $area 'status.json') $state
}
Write-Host ('Isolated renderer inlining: '+$state.status+'; '+(Get-RelativePath $root $area))
if($state.status -ne 'compiled'){throw $state.error}
