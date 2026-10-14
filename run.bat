@echo off
setlocal
set "PS=%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe"
if exist "%SystemRoot%\Sysnative\WindowsPowerShell\v1.0\powershell.exe" set "PS=%SystemRoot%\Sysnative\WindowsPowerShell\v1.0\powershell.exe"
set "MODE=auto"
set "MENU_OPTION="
set "DEBUG_OPTION="
set "BENCHMARK_OPTION="
:parse
if "%~1"=="" goto launch
if /I "%~1"=="--no-menu" (
    set "MENU_OPTION=-NoMenu"
    goto next
)
if /I "%~1"=="--debug" (
    set "DEBUG_OPTION=-WaitForDebugger"
    goto next
)
if /I "%~1"=="--benchmark" (
    set "BENCHMARK_OPTION=-Benchmark"
    goto next
)
if /I "%~1"=="--help" goto help
for %%M in (720p 900p 1080p 1440p 1800p pro4k) do (
    if /I "%~1"=="%%M" (
        set "MODE=%%M"
        goto next
    )
)
echo Unsupported argument. Use run.bat --help.
exit /b 2
:next
shift
goto parse
:help
echo Usage: run.bat [720p ^| 900p ^| 1080p ^| 1440p ^| 1800p ^| pro4k] [--no-menu] [--debug] [--benchmark]
echo Default: open settings and reuse the saved resolution. Benchmarks: 1080p or pro4k only.
exit /b 0
:launch
"%PS%" -NoLogo -NoProfile -ExecutionPolicy Bypass -File "%~dp0scripts\launch.ps1" -Mode "%MODE%" %MENU_OPTION% %DEBUG_OPTION% %BENCHMARK_OPTION%
exit /b %errorlevel%
