@echo off
chcp 65001 >nul
title VietHUD SpeedMap Studio
cls
echo ===============================================================================
echo                      VIETHUD SPEEDMAP STUDIO PRO
echo                 Trinh Quan Ly & Chinh Sua Canh Bao Giao Thong
echo ===============================================================================
echo.
echo [*] Dang khoi dong may chu SpeedMap Studio tai http://localhost:8088 ...
echo.

set "SCRIPT_DIR=%~dp0"
set "PYTHON_CMD="

if exist "%SCRIPT_DIR%.venv\Scripts\python.exe" (
    set "PYTHON_CMD=%SCRIPT_DIR%.venv\Scripts\python.exe"
) else (
    set "PYTHON_CMD=python"
)

cd /d "%SCRIPT_DIR%tools\speedmap_studio"
"%PYTHON_CMD%" server.py

if %errorlevel% neq 0 (
    echo.
    echo [!] Co loi xay ra khi khoi chay. Nhan phim bat ky de thoat.
    pause >nul
)
