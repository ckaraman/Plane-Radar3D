@echo off
chcp 65001 >nul
title Radar USB koprusu
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0usb-koprusu.ps1"
echo.
pause
