@echo off
setlocal
set "PS=%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe"
if exist "%SystemRoot%\Sysnative\WindowsPowerShell\v1.0\powershell.exe" set "PS=%SystemRoot%\Sysnative\WindowsPowerShell\v1.0\powershell.exe"
set "PROFILE=%~1"
set "MODE=%~2"
if "%PROFILE%"=="" set "PROFILE=all"
if "%MODE%"=="" set "MODE=pro4k"
"%PS%" -NoLogo -NoProfile -ExecutionPolicy Bypass -File "%~dp0scripts\optimization-v3.ps1" -Action Run -Profile "%PROFILE%" -Mode "%MODE%"
exit /b %errorlevel%
