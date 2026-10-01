@echo off
title VietHUD SpeedMap Studio
echo ========================================================
echo   VIETHUD SPEEDMAP STUDIO - VISUAL MAP & ALERT INSPECTOR
echo ========================================================
echo Dang mo trinh duyet tai http://localhost:8088...
start "" http://localhost:8088
"C:\Users\phamq\radar_car\.venv\Scripts\python.exe" "C:\Users\phamq\radar_car\tools\speedmap_studio\server.py"
pause
