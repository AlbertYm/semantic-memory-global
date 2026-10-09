@echo off
powershell.exe -NoLogo -NoProfile -NonInteractive -ExecutionPolicy Bypass -File "%~dp0Repair-Codex-Memory.ps1" -Mode Apply
set "repair_exit=%ERRORLEVEL%"
if not "%repair_exit%"=="0" echo Persistent memory repair stopped. Review the error above.
pause
exit /b %repair_exit%
