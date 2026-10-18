# SPDX-License-Identifier: GPL-2.0-or-later
# Importing this file does not execute tools or modify the environment.
function Convert-CMakeToolPaths([string[]]$Lines) {
    $paths=@{}
    foreach($line in $Lines){
        if($line -match '^(?<tool>CMAKE_(?:COMMAND|CXX_COMPILER|MAKE_PROGRAM)):[^=]+=(?<path>.*)$'){
            $paths[$Matches['tool']]=$Matches['path']
        }
    }
    return $paths
}
function New-CMakePathArgument([string]$Name,[string]$Path) {
    if($Name -notmatch '^CMAKE_[A-Z_]+$' -or [string]::IsNullOrWhiteSpace($Path)){
        throw 'A CMake tool variable and nonempty path are required.'
    }
    # CMake embeds compiler paths into quoted .cmake source. Native backslashes
    # would become escapes such as \P unless normalized before configuration.
    return '-D'+$Name+':FILEPATH='+$Path.Replace('\','/')
}
function Resolve-ExistingBuildTool([string]$Name,[string]$Configured,[string[]]$Candidates=@()) {
    foreach($command in @(Get-Command $Name -CommandType Application -All -ErrorAction SilentlyContinue)){
        if($command.Source -notmatch '[\\/]\.codex[\\/]'){return $command.Source}
    }
    foreach($scope in @('User','Machine')){
        foreach($directory in ([Environment]::GetEnvironmentVariable('Path',$scope) -split ';')){
            if(-not $directory){continue}
            $path=Join-Path ([Environment]::ExpandEnvironmentVariables($directory.Trim().Trim('"'))) $Name
            if($path -notmatch '[\\/]\.codex[\\/]' -and (Test-Path -LiteralPath $path -PathType Leaf)){return [IO.Path]::GetFullPath($path)}
        }
    }
    foreach($path in @($Configured)+$Candidates){
        if($path -and $path -notmatch '[\\/]\.codex[\\/]' -and (Test-Path -LiteralPath $path -PathType Leaf)){
            return [IO.Path]::GetFullPath($path)
        }
    }
    throw ('Cannot locate installed '+$Name+' in PATH, user/system PATH, project cache or existing installation directories. Nothing will be installed.')
}
function Get-ProjectBuildTools([string]$Root) {
    $configured=@{}
    foreach($configuration in @('Release','RelWithDebInfo')){
        $cache=Resolve-ProjectChild $Root ('Build/'+$configuration+'/CMakeCache.txt')
        if(Test-Path -LiteralPath $cache -PathType Leaf){
            $configured=Convert-CMakeToolPaths ([IO.File]::ReadAllLines($cache));break
        }
    }
    # Locate existing installations without relying on a sibling project's build cache.
    $visualStudio=@()
    foreach($programFiles in @([Environment]::GetFolderPath('ProgramFilesX86'),$env:ProgramFiles)|Select-Object -Unique){
        $pattern=Join-Path $programFiles 'Microsoft Visual Studio/*/*/VC/Tools/Llvm/x64/bin/clang-cl.exe'
        $visualStudio+=@(Get-Item -Path $pattern -ErrorAction SilentlyContinue|Sort-Object FullName -Descending)
    }
    $cmakeCandidates=@(Join-Path $env:ProgramFiles 'CMake/bin/cmake.exe')
    $ninjaCandidates=@();$clangCandidates=@()
    foreach($compiler in $visualStudio){
        $clangCandidates+=$compiler.FullName
        $edition=Split-Path -Parent $compiler.FullName
        for($i=0;$i -lt 5;$i++){$edition=Split-Path -Parent $edition}
        $cmakeCandidates+=Join-Path $edition 'Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe'
        $ninjaCandidates+=Join-Path $edition 'Common7/IDE/CommonExtensions/Microsoft/CMake/Ninja/ninja.exe'
    }
    $msys=Join-Path ([IO.Path]::GetPathRoot($env:WINDIR)) 'msys64/clang64/bin'
    $cmakeCandidates+=Join-Path $msys 'cmake.exe'
    $ninjaCandidates+=Join-Path $msys 'ninja.exe'
    return [pscustomobject]@{
        CMAKE_COMMAND=Resolve-ExistingBuildTool 'cmake.exe' $configured['CMAKE_COMMAND'] $cmakeCandidates
        CMAKE_CXX_COMPILER=Resolve-ExistingBuildTool 'clang-cl.exe' $configured['CMAKE_CXX_COMPILER'] $clangCandidates
        CMAKE_MAKE_PROGRAM=Resolve-ExistingBuildTool 'ninja.exe' $configured['CMAKE_MAKE_PROGRAM'] $ninjaCandidates
    }
}
function Invoke-ProjectBuildProcess([string]$Executable,[string[]]$ToolArguments,[string]$Directory,[hashtable]$Environment=@{},[switch]$Capture) {
    $start=[Diagnostics.ProcessStartInfo]::new()
    $start.FileName=$Executable;$start.WorkingDirectory=$Directory;$start.UseShellExecute=$false;$start.CreateNoWindow=$true
    $start.Arguments=($ToolArguments|ForEach-Object {Quote-WindowsArgument $_}) -join ' '
    foreach($key in $Environment.Keys){$start.EnvironmentVariables[$key]=$Environment[$key]}
    if($Capture){
        $start.CreateNoWindow=$true;$start.RedirectStandardOutput=$true;$start.RedirectStandardError=$true
        $start.StandardOutputEncoding=[Text.UTF8Encoding]::new($false)
    }
    $process=[Diagnostics.Process]::Start($start)
    try{
        if($Capture){$output=$process.StandardOutput.ReadToEndAsync();$errorText=$process.StandardError.ReadToEndAsync()}
        $process.WaitForExit()
        if($process.ExitCode -ne 0){
            if($Capture -and $errorText.Result){Write-Host $errorText.Result -ForegroundColor Red}
            throw ([IO.Path]::GetFileName($Executable)+' failed, exit code '+$process.ExitCode+'. The runtime selection was not activated by this command.')
        }
        if($Capture){return $output.Result}
    }finally{$process.Dispose()}
}
function Get-ProjectBuildEnvironment([string]$Compiler,[string]$Root) {
    $directory=Split-Path -Parent $Compiler;$vcvars=$null
    for($i=0;$i -lt 8 -and $directory;$i++){
        $candidate=Join-Path $directory 'Auxiliary/Build/vcvars64.bat'
        if(Test-Path -LiteralPath $candidate -PathType Leaf){$vcvars=$candidate;break}
        $parent=Split-Path -Parent $directory
        if($parent -eq $directory){break};$directory=$parent
    }
    if(-not $vcvars){
        if($env:INCLUDE -and $env:LIB){return @{}}
        throw 'The installed x64 Visual Studio environment was not found. Run build.bat from its existing x64 developer command prompt.'
    }
    # Read only build-related paths from the child initialized by vcvars64.
    $probe=@'
[Console]::OutputEncoding=[Text.UTF8Encoding]::new($false)
$paths=@{}
foreach($name in @('PATH','INCLUDE','LIB','LIBPATH','VCToolsInstallDir','WindowsSdkDir','WindowsSDKVersion','UniversalCRTSdkDir','UCRTVersion','VCINSTALLDIR','VSINSTALLDIR')){
    $value=[Environment]::GetEnvironmentVariable($name,'Process')
    if($value){$paths[$name]=$value}
}
[Console]::WriteLine(($paths|ConvertTo-Json -Compress))
'@
    $encoded=[Convert]::ToBase64String([Text.Encoding]::Unicode.GetBytes($probe))
    $shell=Join-Path $env:WINDIR 'System32/WindowsPowerShell/v1.0/powershell.exe'
    $command='call "'+$vcvars+'" >nul && "'+$shell+'" -NoLogo -NoProfile -EncodedCommand '+$encoded
    # cmd /s /c has its own quoting rules; the command contains only trusted installed paths.
    $start=[Diagnostics.ProcessStartInfo]::new()
    $start.FileName=Join-Path $env:WINDIR 'System32/cmd.exe'
    $start.Arguments='/d /s /c "'+$command+'"';$start.WorkingDirectory=$Root
    $start.UseShellExecute=$false;$start.CreateNoWindow=$true
    $start.RedirectStandardOutput=$true;$start.RedirectStandardError=$true
    $start.StandardOutputEncoding=[Text.UTF8Encoding]::new($false)
    $process=[Diagnostics.Process]::Start($start)
    try{
        $output=$process.StandardOutput.ReadToEndAsync();$errorText=$process.StandardError.ReadToEndAsync()
        if(-not $process.WaitForExit(60000)){$process.Kill();$process.WaitForExit();throw 'Visual Studio environment initialization timed out.'}
        if($process.ExitCode -ne 0){throw ('Cannot initialize the installed x64 Visual Studio environment: '+$errorText.Result)}
        $values=$output.Result|ConvertFrom-Json;$environment=@{}
        foreach($property in $values.PSObject.Properties){$environment[$property.Name]=[string]$property.Value}
        if(-not $environment['INCLUDE'] -or -not $environment['LIB']){throw 'The installed Visual Studio environment did not supply INCLUDE and LIB.'}
        return $environment
    }finally{$process.Dispose()}
}
