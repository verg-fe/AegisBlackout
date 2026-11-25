@echo off
setlocal enabledelayedexpansion
cd /d "%~dp0"

echo ====================================================================
echo   AegisBlackout v4.0.0 - x64 MSVC Static Build
echo ====================================================================

set "VCVARS="
where cl >nul 2>&1
if %errorlevel% equ 0 goto have_cl

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if exist "%VSWHERE%" (
    for /f "usebackq delims=" %%I in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do (
        if exist "%%I\VC\Auxiliary\Build\vcvars64.bat" set "VCVARS=%%I\VC\Auxiliary\Build\vcvars64.bat"
    )
)

if not defined VCVARS (
    for %%P in (
        "%ProgramFiles(x86)%\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
        "%ProgramFiles(x86)%\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
        "%ProgramFiles%\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
        "%ProgramFiles%\Microsoft Visual Studio\2022\Professional\VC\Auxiliary\Build\vcvars64.bat"
        "%ProgramFiles%\Microsoft Visual Studio\2022\Enterprise\VC\Auxiliary\Build\vcvars64.bat"
        "%ProgramFiles(x86)%\Microsoft Visual Studio\2019\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
    ) do (
        if not defined VCVARS if exist %%P set "VCVARS=%%~P"
    )
)

if not defined VCVARS (
    echo [ERROR] Could not locate MSVC x64 Build Tools ^(vcvars64.bat^).
    exit /b 1
)

echo [*] Initializing MSVC x64 toolchain: "%VCVARS%"
call "%VCVARS%" >nul
if %errorlevel% neq 0 (
    echo [ERROR] Failed to initialize MSVC environment.
    exit /b 1
)

:have_cl
if not exist "build" mkdir "build"

echo.
echo [1/3] Compiling build\AegisBlackout.exe ...
cl /nologo /EHsc /O2 /MT /W3 /DUNICODE /D_UNICODE ^
    "src\AegisBlackout.cpp" ^
    /Fe:"build\AegisBlackout.exe" /Fo:"build\AegisBlackout.obj" ^
    /link /SUBSYSTEM:WINDOWS user32.lib gdi32.lib dwmapi.lib d3d11.lib dxgi.lib dcomp.lib advapi32.lib bcrypt.lib shell32.lib
if %errorlevel% neq 0 exit /b 1

echo.
echo [2/3] Compiling build\AegisBlackoutSvc.exe ...
cl /nologo /O2 /MT /W3 /DUNICODE /D_UNICODE ^
    "src\AegisBlackoutSvc.c" ^
    /Fe:"build\AegisBlackoutSvc.exe" /Fo:"build\AegisBlackoutSvc.obj" ^
    /link /SUBSYSTEM:CONSOLE advapi32.lib wtsapi32.lib userenv.lib
if %errorlevel% neq 0 exit /b 1

echo.
echo [3/3] Compiling build\CaptureAuditSuite.exe ...
cl /nologo /EHsc /std:c++17 /D_SILENCE_EXPERIMENTAL_COROUTINE_DEPRECATION_WARNINGS /O2 /MT /W3 ^
    "src\CaptureAuditSuite.cpp" ^
    /Fe:"build\CaptureAuditSuite.exe" /Fo:"build\CaptureAuditSuite.obj" ^
    /link /SUBSYSTEM:CONSOLE user32.lib gdi32.lib dwmapi.lib d3d11.lib dxgi.lib d3d9.lib windowsapp.lib advapi32.lib ole32.lib
if %errorlevel% neq 0 exit /b 1

del /f /q "build\*.obj" >nul 2>&1

echo.
echo ====================================================================
echo   [OK] Build complete:
echo        build\AegisBlackout.exe
echo        build\AegisBlackoutSvc.exe
echo        build\CaptureAuditSuite.exe
echo ====================================================================
exit /b 0
