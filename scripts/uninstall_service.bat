@echo off
setlocal
set "__BAT=%~f0"
net session >nul 2>&1
if %errorlevel% equ 0 goto elevated
echo [*] Requesting Administrator elevation ^(UAC^) to uninstall AegisBlackoutGuard...
powershell -NoProfile -Command "Start-Process -FilePath $env:__BAT -Verb RunAs"
exit /b

:elevated
set "BASE_DIR=%~dp0"
if "%BASE_DIR:~-1%"=="\" set "BASE_DIR=%BASE_DIR:~0,-1%"
set "BIN_DIR=%BASE_DIR%"
if not exist "%BIN_DIR%\AegisBlackout.exe" if exist "%BASE_DIR%\..\build\AegisBlackout.exe" (
    for %%I in ("%BASE_DIR%\..\build") do set "BIN_DIR=%%~fI"
)

echo Stopping and removing AegisBlackoutGuard...
sc stop AegisBlackoutGuard >nul 2>&1
ping 127.0.0.1 -n 2 >nul
taskkill /f /im AegisBlackout.exe >nul 2>&1
sc delete AegisBlackoutGuard >nul 2>&1

if exist "%BIN_DIR%\AegisBlackout.exe" icacls "%BIN_DIR%\AegisBlackout.exe" /reset >nul 2>&1
if exist "%BIN_DIR%\AegisBlackoutSvc.exe" icacls "%BIN_DIR%\AegisBlackoutSvc.exe" /reset >nul 2>&1

echo [OK] AegisBlackoutGuard service removed and binary NTFS ACLs restored.
pause
