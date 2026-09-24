@echo off
powershell -ExecutionPolicy Bypass -File "%~dp0copy_to_sd.ps1" %*
pause
