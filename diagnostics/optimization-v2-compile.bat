@echo off
setlocal
title inFAMOUS Second Son - V2 compile test
set "PS=%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe"
if exist "%SystemRoot%\Sysnative\WindowsPowerShell\v1.0\powershell.exe" set "PS=%SystemRoot%\Sysnative\WindowsPowerShell\v1.0\powershell.exe"
echo PRO 4K V2 profile: compile. F1 opens exit confirmation.
echo Manual play; no automated input or timed exit. Session saves/settings stay isolated.
"%PS%" -NoLogo -NoProfile -ExecutionPolicy Bypass -File "%~dp0..\scripts\launch.ps1" -Mode pro4k -NoMenu -OptimizationV2 compile
set "RESULT=%errorlevel%"
if not "%RESULT%"=="0" (
    echo Launch failed. Review errors above. Rebuild changed C++ sources with build.bat first.
    pause
)
exit /b %RESULT%
