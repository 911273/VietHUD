@echo off
chcp 65001 > nul
title VIETHUD - XUAT BAN DO VECTOR TOI GIAN (~16MB)
color 0A

echo ===============================================================================
echo      VIETHUD DATA BUILDER - BAN DO VECTOR TOI GIAN (VECTOR ONLY ~16MB)
echo ===============================================================================
echo.
echo Dang dong goi ban do vector, ten duong, canh bao va am thanh...
echo (Bo qua file anh raster maptiles.bin nang 448MB de chep cuc nhanh)
echo.

cd /d "%~dp0"
if exist "tools\viethud_builder.py" (
    python tools\viethud_builder.py --vector-only
) else (
    python viethud_builder.py --vector-only
)

echo.
echo ===============================================================================
echo Da hoan tat! Nhan phim bat ky de thoat...
echo ===============================================================================
pause > nul
