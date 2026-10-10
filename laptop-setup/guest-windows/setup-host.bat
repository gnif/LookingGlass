@echo off
cd /d "%~dp0"
net session >nul 2>&1 || (powershell -Command "Start-Process '%~f0' -Verb RunAs" & exit /b)

reg add "HKLM\SYSTEM\CurrentControlSet\Control\Terminal Server" /v fDenyTSConnections /t REG_DWORD /d 0 /f >nul
netsh advfirewall firewall set rule group="remote desktop" new enable=Yes >nul

powercfg /x -standby-timeout-ac 0
powercfg /x -monitor-timeout-ac 0
powercfg -h off

if exist "looking-glass-host-setup.exe" (
    start /wait looking-glass-host-setup.exe /S
    echo Looking Glass host installed.
)
pause
