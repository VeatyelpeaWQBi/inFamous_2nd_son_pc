@echo off
setlocal
set "PS=%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe"
if exist "%SystemRoot%\Sysnative\WindowsPowerShell\v1.0\powershell.exe" set "PS=%SystemRoot%\Sysnative\WindowsPowerShell\v1.0\powershell.exe"
"%PS%" -NoLogo -NoProfile -ExecutionPolicy Bypass -File "%~dp0..\scripts\collect_super.ps1" -FullCapture %*
set "RESULT=%errorlevel%"
echo.
if "%RESULT%"=="0" (
    echo Full super collection succeeded. Results: state\super-captures\last-full-run.json
) else (
    echo Full super collection FAILED. Exit code: %RESULT%
    echo Startup details: state\super-captures\last-full-entry.json
    echo See the error above. No successful collection is implied.
)
pause
exit /b %RESULT%
