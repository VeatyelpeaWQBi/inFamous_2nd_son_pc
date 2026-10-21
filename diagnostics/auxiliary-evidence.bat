@echo off
cd /d "%~dp0.."
powershell.exe -NoLogo -NoProfile -ExecutionPolicy Bypass -File scripts\launch.ps1 -Mode pro4k -NoMenu -TimingProfile auxiliary
pause
