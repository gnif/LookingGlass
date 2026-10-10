@echo off
setlocal EnableDelayedExpansion

:: 1. Cek & Minta Akses Administrator Otomatis
net session >nul 2>&1
if %errorLevel% neq 0 (
    echo [INFO] Meminta hak akses Administrator...
    powershell -Command "Start-Process '%~f0' -Verb RunAs"
    exit /b
)

title Setup Otomatis Looking Glass & Windows 11 VM
echo =======================================================
echo    SETUP OTOMATIS LOOKING GLASS & PASSTHROUGH WIN 11
echo =======================================================
echo.

cd /d "%~dp0"

:: 2. Aktifkan Remote Desktop (RDP) & Firewall Rule (Akses Darurat)
echo [1/4] Mengaktifkan Remote Desktop (RDP) cadangan...
reg add "HKEY_LOCAL_MACHINE\SYSTEM\CurrentControlSet\Control\Terminal Server" /v fDenyTSConnections /t REG_DWORD /d 0 /f >nul
netsh advfirewall firewall set rule group="remote desktop" new enable=Yes >nul
echo [OK] Remote Desktop aktif.

:: 3. Matikan Sleep & Hibernation (Mencegah freeze saat passthrough)
echo [2/4] Mematikan Sleep & Monitor Timeout...
powercfg -change -standby-timeout-ac 0 >nul 2>&1
powercfg -change -monitor-timeout-ac 0 >nul 2>&1
powercfg -change -hibernate-timeout-ac 0 >nul 2>&1
powercfg -h off >nul 2>&1
echo [OK] Power profile dioptimalkan.

:: 4. Install Looking Glass Host B7 (Otomatis / Silent)
echo [3/4] Menginstal Looking Glass Host B7...
if exist "looking-glass-host-setup.exe" (
    start /wait looking-glass-host-setup.exe /S
    echo [OK] Looking Glass Host B7 berhasil diinstal!
) else (
    echo [SKIP] File looking-glass-host-setup.exe tidak ditemukan di folder ini.
)

:: 5. Informasi Driver IVSHMEM & Display
echo [4/4] Konfigurasi Device Tambahan:
echo  - Device IVSHMEM akan muncul di Device Manager setelah XML VM ditambahkan.
echo  - Driver IVSHMEM resmi dapat diunduh dari ISO virtio-win.
echo.
echo =======================================================
echo [SELESAI] Konfigurasi Windows 11 selesai!
echo =======================================================
pause
