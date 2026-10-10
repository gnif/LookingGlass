@echo off
cd /d "%~dp0"
sc query "Looking Glass (host)"
copy "%ProgramData%\Looking Glass (host)\looking-glass-host.txt" "%~dp0host.log" >nul 2>&1
pause
