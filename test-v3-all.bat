@echo off
setlocal
cd /d "%~dp0"
set "PS=%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe"
if exist "%SystemRoot%\Sysnative\WindowsPowerShell\v1.0\powershell.exe" set "PS=%SystemRoot%\Sysnative\WindowsPowerShell\v1.0\powershell.exe"
echo V3 unattended suite: build, checks, muted comparisons, confirmation, full and local logs.
echo It can take many hours. No Codex session or AI calls are used while it runs.
if not exist "state\runtime" mkdir "state\runtime"
set "BOOTLOG=state\runtime\v3-suite-bootstrap-%RANDOM%-%RANDOM%.log"
echo Startup log: %BOOTLOG%
"%PS%" -NoLogo -NoProfile -ExecutionPolicy Bypass -File "%~dp0scripts\run-v3-suite.ps1" -Run %* > "%BOOTLOG%" 2>&1
set "RESULT=%ERRORLEVEL%"
echo.
echo Exit code: %RESULT%
echo Result pointer: state\runtime\v3-suite-last.json
echo If startup failed before creating a suite, read: %BOOTLOG%
pause
exit /b %RESULT%
