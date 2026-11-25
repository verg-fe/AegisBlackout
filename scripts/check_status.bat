@echo off
setlocal
set "BASE_DIR=%~dp0"
if "%BASE_DIR:~-1%"=="\" set "BASE_DIR=%BASE_DIR:~0,-1%"
set "BIN_DIR=%BASE_DIR%"
if not exist "%BIN_DIR%\aegis_init.log" if exist "%BASE_DIR%\..\build\aegis_init.log" (
    for %%I in ("%BASE_DIR%\..\build") do set "BIN_DIR=%%~fI"
)

echo ====================================================================
echo   AegisBlackout v4.0.0 - Runtime Status Check
echo ====================================================================
echo.

tasklist /fi "imagename eq AegisBlackout.exe" 2>nul | findstr /i "AegisBlackout.exe" >nul
if errorlevel 1 (
    echo  [OFF] AegisBlackout.exe is not currently running.
) else (
    echo  [ON ] AegisBlackout.exe is ACTIVE.
)

sc query AegisBlackoutGuard >nul 2>&1
if errorlevel 1 (
    echo  [TIP] AegisBlackoutGuard service is not installed ^(run scripts\install_service.bat^).
) else (
    sc query AegisBlackoutGuard | findstr /i "RUNNING" >nul
    if errorlevel 1 (
        echo  [OFF] AegisBlackoutGuard service is installed but STOPPED.
    ) else (
        echo  [ON ] AegisBlackoutGuard SYSTEM Watchdog is RUNNING ^(High-IL UIPI^).
    )
)

echo.
if exist "%BIN_DIR%\aegis_init.log" (
    echo --- Latest aegis_init.log ^(%BIN_DIR%^) ---
    type "%BIN_DIR%\aegis_init.log"
    echo ----------------------------------------
) else (
    echo  [INFO] No aegis_init.log found in "%BIN_DIR%".
)

echo.
echo  * Privileged Physical Snipping : [ Ctrl + Alt + Q ]
echo  * Clean Emergency Exit         : [ Ctrl + Alt + Shift + End ]
echo ====================================================================
pause
