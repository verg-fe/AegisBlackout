@echo off
setlocal
set "__BAT=%~f0"
net session >nul 2>&1
if %errorlevel% equ 0 goto elevated
echo [*] Requesting Administrator elevation ^(UAC^) to install AegisBlackoutGuard...
powershell -NoProfile -Command "Start-Process -FilePath $env:__BAT -Verb RunAs"
exit /b

:elevated
set "BASE_DIR=%~dp0"
if "%BASE_DIR:~-1%"=="\" set "BASE_DIR=%BASE_DIR:~0,-1%"
set "BIN_DIR=%BASE_DIR%"
if not exist "%BIN_DIR%\AegisBlackout.exe" if exist "%BASE_DIR%\..\build\AegisBlackout.exe" (
    for %%I in ("%BASE_DIR%\..\build") do set "BIN_DIR=%%~fI"
)

if not exist "%BIN_DIR%\AegisBlackout.exe" (
    echo [ERROR] AegisBlackout.exe not found in "%BASE_DIR%" or "%BASE_DIR%\..\build".
    echo         Run build.bat first or extract all Release files together.
    pause
    exit /b 1
)
if not exist "%BIN_DIR%\AegisBlackoutSvc.exe" (
    echo [ERROR] AegisBlackoutSvc.exe not found in "%BIN_DIR%".
    pause
    exit /b 1
)

echo ====================================================================
echo   AegisBlackout v4.0.1 - Kernel UIPI + Per-Window Cloak + Watchdog Installer
echo ====================================================================
echo   Binary Directory: "%BIN_DIR%"
echo.

echo [1/5] Stopping any previous AegisBlackout instance...
sc stop AegisBlackoutGuard >nul 2>&1
taskkill /f /im AegisBlackout.exe >nul 2>&1
ping 127.0.0.1 -n 3 >nul

echo [2/5] Applying NTFS Read-Only ACL hardening on binaries...
icacls "%BIN_DIR%\AegisBlackout.exe" /inheritance:r /grant:r "NT AUTHORITY\SYSTEM:(F)" "BUILTIN\Administrators:(F)" "BUILTIN\Users:(RX)" >nul
icacls "%BIN_DIR%\AegisBlackoutSvc.exe" /inheritance:r /grant:r "NT AUTHORITY\SYSTEM:(F)" "BUILTIN\Administrators:(F)" "BUILTIN\Users:(RX)" >nul

echo [3/5] Installing and hardening SYSTEM service AegisBlackoutGuard...
sc delete AegisBlackoutGuard >nul 2>&1
ping 127.0.0.1 -n 2 >nul
sc create AegisBlackoutGuard binPath= "\"%BIN_DIR%\AegisBlackoutSvc.exe\"" start= auto DisplayName= "AegisBlackout Kernel UIPI Watchdog" >nul
sc description AegisBlackoutGuard "AegisBlackout 24x7 GPU HW-DRM, High-IL win32k UIPI & Self-Healing Anti-Capture Watchdog" >nul
sc failure AegisBlackoutGuard reset= 0 actions= restart/1000/restart/1000/restart/1000 >nul
sc sdset AegisBlackoutGuard "D:(A;;CCLCSWRPWPDTLOCRRC;;;SY)(A;;CCDCLCSWRPWPDTLOCRSDRCWDWO;;;BA)(A;;CCLCSWLOCRRC;;;IU)(A;;CCLCSWLOCRRC;;;SU)" >nul

echo [4/5] Starting AegisBlackoutGuard ^(Session 0 SYSTEM -> Console High-IL^)...
sc start AegisBlackoutGuard >nul
ping 127.0.0.1 -n 4 >nul

echo [5/5] Verifying service, High-IL shield process & HW-DRM log...
echo.
sc query AegisBlackoutGuard | findstr /i "RUNNING" >nul
if errorlevel 1 (
    echo  [WARN] AegisBlackoutGuard service did not reach RUNNING state.
) else (
    echo  [OK] AegisBlackoutGuard service is RUNNING ^(SYSTEM^).
)

tasklist /fi "imagename eq AegisBlackout.exe" 2>nul | findstr /i "AegisBlackout.exe" >nul
if errorlevel 1 (
    echo  [WARN] AegisBlackout.exe was not detected in tasklist yet.
) else (
    echo  [OK] AegisBlackout.exe is active ^(High-IL UIPI + 24x7 GPU HW-DRM + Per-Wnd Cloak^).
)

echo.
if exist "%BIN_DIR%\aegis_init.log" (
    echo --- aegis_init.log ---
    type "%BIN_DIR%\aegis_init.log"
    echo ----------------------
)

echo.
echo  * Physical Zero-Silent Snipping : [ Ctrl + Alt + Q ]
echo  * Clean Emergency Exit ^(no UAC^): [ Ctrl + Alt + Shift + End ]
echo ====================================================================
pause
