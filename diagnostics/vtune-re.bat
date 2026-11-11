@echo off
setlocal
set "PS=%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe"
if exist "%SystemRoot%\Sysnative\WindowsPowerShell\v1.0\powershell.exe" set "PS=%SystemRoot%\Sysnative\WindowsPowerShell\v1.0\powershell.exe"
if "%~1"=="" (
    echo Usage: vtune-re.bat ^<session-id or state/sessions/...^> [-RebuildVtune] [-ScanOnly]
    echo Example: vtune-re.bat 20261109-215211-278-pro4k-cab9b081-diagnostic-full -RebuildVtune
    echo Offline only: does not launch the game. Game and other profilers must be stopped.
    exit /b 1
)
set "SESS=%~1"
echo %SESS% | findstr /I /B /C:"state\sessions\" /C:"state/sessions/" >nul
if errorlevel 1 set "SESS=state/sessions/%SESS%"
"%PS%" -NoLogo -NoProfile -ExecutionPolicy Bypass -File "%~dp0..\scripts\resume_super_full.ps1" -Session "%SESS%" %2 %3 %4
set "RESULT=%errorlevel%"
echo.
if "%RESULT%"=="0" (
    echo Offline Full VTune recovery succeeded.
) else (
    echo Offline Full VTune recovery FAILED. Exit code: %RESULT%
    echo Check state\sessions\...\diagnostics\recovery\status.json
)
exit /b %RESULT%