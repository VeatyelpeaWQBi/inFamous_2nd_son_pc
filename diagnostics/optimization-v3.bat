@echo off
setlocal
title inFAMOUS Second Son - V3 R6 C1 candidate
set "PS=%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe"
if exist "%SystemRoot%\Sysnative\WindowsPowerShell\v1.0\powershell.exe" set "PS=%SystemRoot%\Sysnative\WindowsPowerShell\v1.0\powershell.exe"
echo V3 R6 candidate: C1 depth reduction ON. A1 and earlier V3 candidates OFF.
echo Manual play. F1: exit confirmation. Saves/settings stay in this session.
"%PS%" -NoLogo -NoProfile -ExecutionPolicy Bypass -File "%~dp0..\scripts\launch.ps1" -NoMenu -OptimizationV3 all %*
set "RESULT=%errorlevel%"
if not "%RESULT%"=="0" (
    echo Launch failed. Run build.bat after C++ changes; see the error above.
    pause
)
exit /b %RESULT%
