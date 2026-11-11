@echo off
setlocal
set "PS=%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe"
if exist "%SystemRoot%\Sysnative\WindowsPowerShell\v1.0\powershell.exe" set "PS=%SystemRoot%\Sysnative\WindowsPowerShell\v1.0\powershell.exe"
"%PS%" -NoLogo -NoProfile -ExecutionPolicy Bypass -File "%~dp0..\scripts\collect_feedback_pair.ps1" %*
set "RESULT=%errorlevel%"
echo.
if "%RESULT%"=="0" (echo Feedback pair collected. Results: state\super-captures\last-feedback-pair.json) else (echo Feedback pair FAILED. Exit code: %RESULT%)
pause
exit /b %RESULT%
