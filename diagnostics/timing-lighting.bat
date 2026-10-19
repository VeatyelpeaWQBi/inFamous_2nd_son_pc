@echo off
setlocal
title inFAMOUS Second Son - timing lighting
set "PS=%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe"
if exist "%SystemRoot%\Sysnative\WindowsPowerShell\v1.0\powershell.exe" set "PS=%SystemRoot%\Sysnative\WindowsPowerShell\v1.0\powershell.exe"
echo Core PRO 4K - lighting. Same save/scene/camera/warmup/input for comparisons.
echo Manual play. F1 opens exit confirmation. Saves/settings stay in this session.
"%PS%" -NoLogo -NoProfile -ExecutionPolicy Bypass -File "%~dp0..\scripts\launch.ps1" -Mode pro4k -NoMenu -TimingProfile lighting
set "RESULT=%errorlevel%"
if not "%RESULT%"=="0" (
    echo Launch failed. Rebuild changed C++ sources with build.bat before testing.
    pause
)
exit /b %RESULT%
