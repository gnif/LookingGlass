@echo off
net session >nul 2>&1 || (powershell -Command "Start-Process '%~f0' -Verb RunAs" & exit /b)

net stop "Looking Glass (host)" >nul 2>&1
taskkill /f /im looking-glass-host.exe >nul 2>&1

powershell -NoProfile -Command "Get-PnpDevice | Where-Object { $_.HardwareID -like '*1AF4*' } | ForEach-Object { Disable-PnpDevice -InstanceId $_.InstanceId -Confirm:\$false; Start-Sleep 1; Enable-PnpDevice -InstanceId $_.InstanceId -Confirm:\$false }"
timeout /t 2 /nobreak >nul

net start "Looking Glass (host)"
pause
