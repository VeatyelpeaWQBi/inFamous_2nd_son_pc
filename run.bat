@echo off
setlocal
set "EXE=%~dp0inFAMOUS Second Son.exe"
if not exist "%EXE%" (
    if not exist "%~dp0Build\Release\build-receipt.json" (
        echo Current Release build receipt not found. Run build.bat first.
        exit /b 1
    )
    set "EXE=%~dp0Build\Release\inFAMOUS Second Son.exe"
)
if not exist "%EXE%" (
    echo Release engine not found. Run build.bat first.
    exit /b 1
)
"%EXE%" %*
exit /b %errorlevel%
