@echo off
net session >nul 2>&1
if %errorLevel% neq 0 (
    powershell -Command "Start-Process '%~f0' -Verb RunAs"
    exit /b
)

title Aktifkan Fitur Auto-Login (Tanpa PIN/Password)
echo ========================================================
echo       MEMBUKA PENGATURAN AUTO-LOGIN WINDOWS 11
echo ========================================================
echo.

echo [1] Membuka opsi centang Auto-Login di netplwiz...
reg add "HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\PasswordLess\Device" /v DevicePasswordLessBuildVersion /t REG_DWORD /d 0 /f >nul

echo [2] Membuka jendela User Accounts (netplwiz)...
echo.
echo PETUNJUK:
echo  1. HILANGKAN CENTANG pada kotak:
echo     "Users must enter a user name and password to use this computer"
echo  2. Klik tombol "Apply".
echo  3. Masukkan password akun Windows Anda (2x), lalu klik OK.
echo.
pause
start netplwiz
