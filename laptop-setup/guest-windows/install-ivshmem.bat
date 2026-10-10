@echo off
cd /d "%~dp0"
net session >nul 2>&1 || (powershell -Command "Start-Process '%~f0' -Verb RunAs" & exit /b)

pnputil /add-driver ivshmem.inf /install
pause
