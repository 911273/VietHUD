@echo off
chcp 65001 > nul
title VIETHUD - CONG CU TONG HOP VA CAP NHAT DU LIEU THE NHO
color 0A

cd /d "%~dp0"

REM Neu co file EXE thi uu tien chay truc tiep EXE
if exist "VIETHUD_UPDATE_TOOL.exe" (
    start "" "VIETHUD_UPDATE_TOOL.exe"
    exit /b
)

echo ===============================================================================
echo            VIETHUD MASTER DATA BUILDER ^& SD-CARD SYNC TOOL (2026)
echo ===============================================================================
echo.
echo Chon che do dong goi xuat the nho:
echo.
echo   [1] BAN DO VECTOR TOI GIAN (Khuyen dung - Chi ~16.6MB, Chep the sieu nhanh)
echo       + Day du Canh bao giao thong, Camera phat nguoi, Bien bao toan quoc
echo       + Day du Tuyen duong Vector (tiles.bin) va Ten duong pho (names.bin)
echo       + Day du Thu vien am thanh giong noi (sounds/)
echo       - Khong kem file anh raster nang 448MB
echo.
echo   [2] BAN DO DAY DU (Full - ~460MB)
echo       + Bao gom toan bo muc [1] va them file anh nen Raster (maptiles.bin)
echo.
echo   [3] CHI DU LIEU CANH BAO GIAO THONG (Alerts Only - ~3MB)
echo       + Chi bao gom Camera, Bien bao va Am thanh
echo.

set /p choice="Nhap lua chon cua ban [1/2/3] (Nhan Enter mac dinh la 1): "

if "%choice%"=="2" (
    set MODE_ARG=--full
) else if "%choice%"=="3" (
    set MODE_ARG=--mode alerts
) else (
    set MODE_ARG=--vector-only
)

echo.
echo Dang quet du lieu va tu dong dong goi...
echo.

if exist "tools\viethud_builder.py" (
    python tools\viethud_builder.py %MODE_ARG%
) else (
    python viethud_builder.py %MODE_ARG%
)

echo.
echo ===============================================================================
echo Da hoan tat! Nhan phim bat ky de thoat...
echo ===============================================================================
pause > nul
