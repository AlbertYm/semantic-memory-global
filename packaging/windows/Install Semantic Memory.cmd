@echo off
setlocal EnableExtensions DisableDelayedExpansion
powershell.exe -NoLogo -NoProfile -ExecutionPolicy Bypass -File "%~dp0Run-Bundle.ps1"
set "SM_EXIT=%ERRORLEVEL%"
if not "%SM_EXIT%"=="0" echo Operation failed. Read the error above before retrying.
if /i not "%~1"=="--no-pause" pause
exit /b %SM_EXIT%
