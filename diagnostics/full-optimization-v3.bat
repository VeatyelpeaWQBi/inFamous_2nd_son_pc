@echo off
setlocal
title inFAMOUS Second Son - V3 R11 lighting vector reads full
set "PS=%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe"
if exist "%SystemRoot%\Sysnative\WindowsPowerShell\v1.0\powershell.exe" set "PS=%SystemRoot%\Sysnative\WindowsPowerShell\v1.0\powershell.exe"
"%PS%" -NoLogo -NoProfile -ExecutionPolicy Bypass -File "%~dp0..\scripts\diagnostic-click.ps1" -Action full -OptimizationV3 all %*
exit /b %errorlevel%
