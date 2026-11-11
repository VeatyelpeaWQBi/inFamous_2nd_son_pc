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
"%PS%" -NoLogo -NoProfile -ExecutionPolicy Bypass -File "%~dp0..\scripts\resume_super_full.ps1" -Session "%~1" %2 %3 %4
set "RESULT=%errorlevel%"
echo.
if "%RESULT%"=="0" (
    echo Offline Full VTune command completed. Check diagnostics\recovery\status.json for the final status.
) else (
    echo Offline Full VTune recovery FAILED. Exit code: %RESULT%
    echo Check state\sessions\...\diagnostics\recovery\status.json
)
exit /b %RESULT%
