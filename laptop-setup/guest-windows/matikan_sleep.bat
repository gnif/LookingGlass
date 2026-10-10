@echo off
net session >nul 2>&1
if %errorLevel% neq 0 (
    powershell -Command "Start-Process '%~f0' -Verb RunAs"
    exit /b
)

title Matikan Sleep & Timeout Permanen
echo ========================================================
echo   MEMATIKAN SLEEP DAN MONITOR TIMEOUT (ANTI-BLANK)
echo ========================================================
echo.

echo [1] Menonaktifkan Standby dan Sleep...
powercfg /change standby-timeout-ac 0
powercfg /change standby-timeout-dc 0

echo [2] Menonaktifkan Monitor Turn Off...
powercfg /change monitor-timeout-ac 0
powercfg /change monitor-timeout-dc 0

echo [3] Menonaktifkan Hibernate...
powercfg /change hibernate-timeout-ac 0
powercfg /change hibernate-timeout-dc 0
powercfg -h off

echo [4] Mengunci Power Plan ke High Performance...
powercfg /setactive 8c5e7fda-e8bf-4a96-9a85-a6e23a8c635c >nul 2>&1

echo.
echo [SELESAI] Windows tidak akan pernah sleep atau mematikan layar lagi!
pause
