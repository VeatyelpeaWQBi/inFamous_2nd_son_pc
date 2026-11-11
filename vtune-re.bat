@echo off
setlocal
cd /d "%~dp0" || exit /b 1
set "PS=%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe"
if exist "%SystemRoot%\Sysnative\WindowsPowerShell\v1.0\powershell.exe" set "PS=%SystemRoot%\Sysnative\WindowsPowerShell\v1.0\powershell.exe"
"%PS%" -NoLogo -NoProfile -ExecutionPolicy Bypass -File "%~dp0scripts\resume_super_full.ps1" -Session "state/sessions/20261030-200455-120-pro4k-0da8b956-diagnostic-full" -RebuildVtune %*
set "RESULT=%errorlevel%"
echo.
if "%RESULT%"=="0" (
    echo Command completed. Check diagnostics\recovery\status.json in the session for its final status.
) else (
    echo VTune recovery command FAILED. Exit code: %RESULT%
    echo See the error above and the session diagnostics\recovery\status.json.
)
pause
exit /b %RESULT%
