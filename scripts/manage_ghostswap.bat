@echo off
chcp 65001 >nul
setlocal
if /i "%~1"=="--check" goto syntax_check
set "__BAT=%~f0"
net session >nul 2>&1
if %errorlevel% equ 0 goto init_dir
powershell -NoProfile -Command "Start-Process -FilePath $env:__BAT -Verb RunAs -ErrorAction Stop"
exit /b 0

:syntax_check
echo SYNTAX_OK
exit /b 0

:init_dir
set "BASE_DIR=%~dp0"
if "%BASE_DIR:~-1%"=="\" set "BASE_DIR=%BASE_DIR:~0,-1%"
set "BIN_DIR=%BASE_DIR%"
if not exist "%BIN_DIR%\GhostSwap.exe" if exist "%BASE_DIR%\..\build\GhostSwap.exe" (
    for %%I in ("%BASE_DIR%\..\build") do set "BIN_DIR=%%~fI"
)
if not exist "%BIN_DIR%\GhostSwap.exe" (
    echo [ERROR] GhostSwap.exe not found in "%BASE_DIR%" or "%BASE_DIR%\..\build".
    pause
    exit /b 1
)

:menu
cls
cd /d "%BIN_DIR%"
echo ====================================================================
echo   GhostSwap v2.1 无感后台控制面板
echo ====================================================================
echo   [1] 查看实时状态 (GhostSwap v2.1 + AegisBlackout 状态)
echo   [2] 安全关闭 GhostSwap 后台
echo   [3] 切换为「协同护盾模式」(推荐：防截屏 0%% 泄漏 / Ctrl+Alt+Q 清晰截屏)
echo   [4] 切换为「极端显存焊死模式 (--lock-vram)」(连 Ctrl+Alt+Q 也截黑)
echo   [5] 停止后台并卸载 GhostSwap 开机自启任务
echo   [0] 退出菜单 (不影响后台运行)
echo ====================================================================
set /p "CH=请输入数字 [1/2/3/4/5/0]: "
if "%CH%"=="1" goto do_status
if "%CH%"=="2" goto do_stop
if "%CH%"=="3" goto do_coop
if "%CH%"=="4" goto do_lock
if "%CH%"=="5" goto do_uninstall
if "%CH%"=="0" exit /b 0
goto menu

:do_status
echo.
start /wait "" /b "%BIN_DIR%\GhostSwap.exe" --status
echo.
if exist "%BIN_DIR%\aegis_init.log" (
    echo --- AegisBlackout 初始化日志 ---
    type "%BIN_DIR%\aegis_init.log"
    echo --------------------------------
)
echo.
pause
goto menu

:do_stop
echo.
start /wait "" /b "%BIN_DIR%\GhostSwap.exe" --stop
start /wait "" /b "%BIN_DIR%\GhostSwap.exe" --stop-legacy >nul 2>&1
echo 已安全停止 GhostSwap 后台。
pause
goto menu

:do_coop
echo.
echo 正在切换为「协同护盾模式」...
start /wait "" /b "%BIN_DIR%\GhostSwap.exe" --stop >nul 2>&1
ping 127.0.0.1 -n 2 >nul
schtasks /create /f /tn "GhostSwapAutoStart" /tr "\"%BIN_DIR%\GhostSwap.exe\"" /sc onlogon /rl highest >nul
powershell.exe -NoProfile -Command "$s = New-ScheduledTaskSettingsSet -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries -ExecutionTimeLimit (New-TimeSpan -Seconds 0); Set-ScheduledTask -TaskName 'GhostSwapAutoStart' -Settings $s | Out-Null" >nul 2>&1
schtasks /run /tn "GhostSwapAutoStart" >nul 2>&1
if %errorlevel% neq 0 start "" "%BIN_DIR%\GhostSwap.exe"
ping 127.0.0.1 -n 3 >nul
start /wait "" /b "%BIN_DIR%\GhostSwap.exe" --status
echo.
echo [提示] 若浏览器此前处于焊死模式，请重启浏览器一次以重建无锁交换链。
pause
goto menu

:do_lock
echo.
echo 正在切换为「极端显存焊死模式 (--lock-vram)」...
start /wait "" /b "%BIN_DIR%\GhostSwap.exe" --stop >nul 2>&1
ping 127.0.0.1 -n 2 >nul
schtasks /create /f /tn "GhostSwapAutoStart" /tr "\"%BIN_DIR%\GhostSwap.exe\" --lock-vram" /sc onlogon /rl highest >nul
powershell.exe -NoProfile -Command "$s = New-ScheduledTaskSettingsSet -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries -ExecutionTimeLimit (New-TimeSpan -Seconds 0); Set-ScheduledTask -TaskName 'GhostSwapAutoStart' -Settings $s | Out-Null" >nul 2>&1
schtasks /run /tn "GhostSwapAutoStart" >nul 2>&1
if %errorlevel% neq 0 start "" "%BIN_DIR%\GhostSwap.exe" --lock-vram
ping 127.0.0.1 -n 3 >nul
start /wait "" /b "%BIN_DIR%\GhostSwap.exe" --status
pause
goto menu

:do_uninstall
echo.
start /wait "" /b "%BIN_DIR%\GhostSwap.exe" --stop >nul 2>&1
schtasks /delete /f /tn "GhostSwapAutoStart" >nul 2>&1
echo 已停止 GhostSwap 后台并卸载开机自启任务 GhostSwapAutoStart。
pause
goto menu