@echo off
setlocal
set "PS=%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe"
if exist "%SystemRoot%\Sysnative\WindowsPowerShell\v1.0\powershell.exe" set "PS=%SystemRoot%\Sysnative\WindowsPowerShell\v1.0\powershell.exe"
if /I "%~1"=="--check" goto inspect
echo BAT baseline phase. Standalone EXE compilation is deferred until the baseline is accepted.
echo Use build.bat --check to inspect the existing toolchain.
exit /b 2
:inspect
"%PS%" -NoLogo -NoProfile -ExecutionPolicy Bypass -File "%~dp0scripts\check-build.ps1"
exit /b %errorlevel%