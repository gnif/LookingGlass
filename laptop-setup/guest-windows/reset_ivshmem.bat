@echo off
net session >nul 2>&1
if %errorLevel% neq 0 (
    powershell -Command "Start-Process '%~f0' -Verb RunAs"
    exit /b
)

title Reset IVSHMEM & Restart Looking Glass
echo ========================================================
echo        RESET DEVICE IVSHMEM & LOOKING GLASS
echo ========================================================
echo.

echo [1] Menghentikan service Looking Glass...
net stop "Looking Glass (host)" >nul 2>&1
taskkill /F /IM looking-glass-host.exe >nul 2>&1

echo [2] Mereset device driver IVSHMEM di Windows...
powershell -Command "Get-PnpDevice | Where-Object { $_.FriendlyName -like '*IVSHMEM*' -or $_.HardwareID -like '*1AF4*' } | ForEach-Object { Write-Host 'Restarting:' $_.FriendlyName; Disable-PnpDevice -InstanceId $_.InstanceId -Confirm:$false; Start-Sleep 1; Enable-PnpDevice -InstanceId $_.InstanceId -Confirm:$false }"

echo [3] Menunggu 2 detik...
timeout /t 2 /nobreak >nul

echo [4] Memulai kembali service Looking Glass...
net start "Looking Glass (host)"

echo [5] Menyalin log terbaru ke shared folder...
copy "%ProgramData%\Looking Glass (host)\looking-glass-host.txt" "%~dp0log_host.txt" >nul 2>&1

echo.
echo ========================================================
echo Selesai! Cek apakah service berhasil berjalan di atas.
echo ========================================================
pause
