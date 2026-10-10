@echo off
cd /d "%~dp0"
net session >nul 2>&1
if %errorLevel% neq 0 (
    powershell -Command "Start-Process '%~f0' -Verb RunAs"
    exit /b
)
echo Memasang Driver IVSHMEM...
pnputil /add-driver "ivshmem.inf" /install
echo Selesai.
pause
