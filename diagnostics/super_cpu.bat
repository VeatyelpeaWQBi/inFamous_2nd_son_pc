@echo off
setlocal
"%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe" -NoLogo -NoProfile -ExecutionPolicy Bypass -File "%~dp0..\scripts\collect_super.ps1" -CpuHotspots -Profile lightweight -Rounds 1 -Experiment none %*
set "capture_exit=%errorlevel%"
if not "%capture_exit%"=="0" echo CPU super diagnostic failed. See the collector error above.
exit /b %capture_exit%
