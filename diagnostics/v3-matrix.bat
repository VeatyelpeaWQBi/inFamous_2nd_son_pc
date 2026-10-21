@echo off
cd /d "%~dp0.."
powershell.exe -NoLogo -NoProfile -ExecutionPolicy Bypass -File scripts\test-v3-matrix.ps1 %*
pause
