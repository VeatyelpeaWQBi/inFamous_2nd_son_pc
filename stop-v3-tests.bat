@echo off
setlocal
cd /d "%~dp0"
if not exist "state\runtime" mkdir "state\runtime"
> "state\runtime\v3-suite.stop" echo User requested a normal stop.
echo Stop requested. The suite will close only its own test session and save its logs.
pause
