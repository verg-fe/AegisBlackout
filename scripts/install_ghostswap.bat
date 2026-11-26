@echo off
chcp 65001 >nul
setlocal
if /i "%~1"=="--check" goto syntax_check
set "__BAT=%~f0"
net session >nul 2>&1
if %errorlevel% equ 0 goto elevated
echo ====================================================================
echo   Requesting Administrator elevation (UAC) for GhostSwap v2.1...
echo ====================================================================
powershell -NoProfile -Command "Start-Process -FilePath $env:__BAT -Verb RunAs -ErrorAction Stop"
if %errorlevel% neq 0 goto uac_fail
exit /b 0

:uac_fail
echo.
echo [ERROR] Administrator UAC prompt was cancelled. Please run as Administrator.
pause
exit /b 1

:syntax_check
echo SYNTAX_OK
exit /b 0

:elevated
set "BASE_DIR=%~dp0"
if "%BASE_DIR:~-1%"=="\" set "BASE_DIR=%BASE_DIR:~0,-1%"
set "BIN_DIR=%BASE_DIR%"
if not exist "%BIN_DIR%\GhostSwap.exe" if exist "%BASE_DIR%\..\build\GhostSwap.exe" (
    for %%I in ("%BASE_DIR%\..\build") do set "BIN_DIR=%%~fI"
)
if not exist "%BIN_DIR%\GhostSwap.exe" (
    echo [ERROR] GhostSwap.exe not found in "%BASE_DIR%" or "%BASE_DIR%\..\build".
    echo         Run build.bat first or extract all Release files together.
    pause
    exit /b 1
)

cd /d "%BIN_DIR%"
echo ====================================================================
echo   AegisBlackout v4.1.0 + GhostSwap v2.1 无感伴侣部署 (协同护盾模式)
echo ====================================================================
echo   Binary Directory: "%BIN_DIR%"
echo.

echo [1/4] 正在优雅停止旧版 GhostSwap 实例...
start /wait "" /b "%BIN_DIR%\GhostSwap.exe" --stop >nul 2>&1
start /wait "" /b "%BIN_DIR%\GhostSwap.exe" --stop-legacy >nul 2>&1
ping 127.0.0.1 -n 2 >nul

echo [2/4] 正在注册最高权限静默开机自启任务 GhostSwapAutoStart...
schtasks /create /f /tn "GhostSwapAutoStart" /tr "\"%BIN_DIR%\GhostSwap.exe\"" /sc onlogon /rl highest >nul
powershell.exe -NoProfile -Command "$s = New-ScheduledTaskSettingsSet -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries -ExecutionTimeLimit (New-TimeSpan -Seconds 0); Set-ScheduledTask -TaskName 'GhostSwapAutoStart' -Settings $s | Out-Null" >nul 2>&1

echo [3/4] 正在静默拉起 GhostSwap v2.1 (无黑框后台)...
schtasks /run /tn "GhostSwapAutoStart" >nul 2>&1
if %errorlevel% neq 0 start "" "%BIN_DIR%\GhostSwap.exe"
ping 127.0.0.1 -n 3 >nul

echo [4/4] 正在核验 GhostSwap v2.1 实时运行状态...
start /wait "" /b "%BIN_DIR%\GhostSwap.exe" --status
echo.
echo ====================================================================
echo   [完成] GhostSwap v2.1 已在后台静默运行 (协同护盾模式)
echo   瞬间全黑防偷拍   : Ctrl + Alt + F9  (开关黑幕)
echo   黑幕档位切换     : Ctrl + Alt + F10 (100%%/85%%) / F11 (浓度循环)
echo   自己清晰特权截屏 : Ctrl + Alt + Q   (需先通过 install_service.bat 启用护盾)
echo ====================================================================
pause
exit /b 0