@echo off
net session >nul 2>&1
if %errorLevel% neq 0 (
    powershell -Command "Start-Process '%~f0' -Verb RunAs"
    exit /b
)

echo [1] Menghentikan proses Looking Glass yang menggantung...
taskkill /F /IM looking-glass-host.exe >nul 2>&1
net stop "Looking Glass (host)" >nul 2>&1

echo [2] Menunggu 3 detik...
timeout /t 3 /nobreak >nul

echo [3] Memulai ulang Service Looking Glass...
net start "Looking Glass (host)"

echo [4] Menyalin log terbaru...
copy "%ProgramData%\Looking Glass (host)\looking-glass-host.txt" "%~dp0log_host.txt" >nul 2>&1

echo.
pause
