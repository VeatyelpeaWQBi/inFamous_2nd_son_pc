@echo off
setlocal
title inFAMOUS Second Son - V1 optimization test
set "PS=%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe"
if exist "%SystemRoot%\Sysnative\WindowsPowerShell\v1.0\powershell.exe" set "PS=%SystemRoot%\Sysnative\WindowsPowerShell\v1.0\powershell.exe"
echo PRO 4K manual test: both V1 optimizations ON; session saves/configuration stay isolated.
echo F1 opens exit confirmation. No automated input or timed exit.
"%PS%" -NoLogo -NoProfile -ExecutionPolicy Bypass -File "%~dp0..\scripts\launch.ps1" -Mode pro4k -NoMenu -OptimizationV1
set "RESULT=%errorlevel%"
if not "%RESULT%"=="0" (
    echo Test launch failed. Review the error above. Rebuild current sources with build.bat first.
    pause
)
exit /b %RESULT%
