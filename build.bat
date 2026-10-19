@echo off
setlocal
set "PS=%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe"
if exist "%SystemRoot%\Sysnative\WindowsPowerShell\v1.0\powershell.exe" set "PS=%SystemRoot%\Sysnative\WindowsPowerShell\v1.0\powershell.exe"
if /I "%~1"=="--check" goto inspect
if not "%~1"=="" (
    echo Usage: build.bat [--check]
    exit /b 2
)
"%PS%" -NoLogo -NoProfile -ExecutionPolicy Bypass -File "%~dp0scripts\build.ps1"
exit /b %errorlevel%
:inspect
"%PS%" -NoLogo -NoProfile -ExecutionPolicy Bypass -File "%~dp0scripts\check-build.ps1"
exit /b %errorlevel%