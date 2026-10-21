@echo off
setlocal
title inFAMOUS Second Son - bounded consumer evidence
set "PS=%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe"
if exist "%SystemRoot%\Sysnative\WindowsPowerShell\v1.0\powershell.exe" set "PS=%SystemRoot%\Sysnative\WindowsPowerShell\v1.0\powershell.exe"
echo Pinned read-fault register evidence. Muted, isolated saves, all V3 candidates OFF.
echo After 45 seconds: at most 64 samples per instruction. No full collector or FPS acceptance.
"%PS%" -NoLogo -NoProfile -ExecutionPolicy Bypass -File "%~dp0..\scripts\launch.ps1" -NoMenu -TimingProfile consumer %*
exit /b %errorlevel%
