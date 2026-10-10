@echo off
title Cek & Jalankan Looking Glass Host
cd /d "%~dp0"

echo [1] MENCOBA MENJALANKAN SERVICE LOOKING GLASS...
net start "Looking Glass (host)"

echo.
echo [2] MENYALIN LOG KE SHARED FOLDER...
copy "%ProgramData%\Looking Glass (host)\looking-glass-host.txt" "%~dp0log_host.txt" >nul 2>&1

echo.
echo [3] STATUS SERVICE:
sc query "Looking Glass (host)"

echo.
pause
