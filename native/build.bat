@echo off
setlocal EnableExtensions
rem Usage: build.bat [release^|debug]
rem   release (default) - optimized, stripped, single portable exe
rem   debug             - symbols, no optimization (run the exe with --debug for logs/DevTools)

cd /d "%~dp0"

set "MODE=%~1"
if "%MODE%"=="" set "MODE=release"
if /i not "%MODE%"=="release" if /i not "%MODE%"=="debug" (
    echo [!] Unknown mode "%MODE%". Use: build.bat [release^|debug]
    exit /b 2
)

where g++.exe >nul 2>&1 || (echo [!] g++.exe not found. Install MinGW-w64 UCRT and add it to PATH. & exit /b 1)
where windres.exe >nul 2>&1 || (echo [!] windres.exe not found. & exit /b 1)

for /f "tokens=3" %%v in ('findstr /c:"#define YTR_VERSION_MAJOR" res\version.h') do set "VMAJ=%%v"
for /f "tokens=3" %%v in ('findstr /c:"#define YTR_VERSION_MINOR" res\version.h') do set "VMIN=%%v"
for /f "tokens=3" %%v in ('findstr /c:"#define YTR_VERSION_PATCH" res\version.h') do set "VPAT=%%v"

echo ========================================================
echo   ytr-music %VMAJ%.%VMIN%.%VPAT% (native, %MODE%)
echo ========================================================

if not exist "bin" mkdir bin
rem The exe embeds WebView2Loader.dll; a copy next to it would take precedence, so drop stale ones.
if exist "bin\WebView2Loader.dll" del /q "bin\WebView2Loader.dll"

set "COMMON=-std=c++17 -municode -mwindows -static -static-libgcc -static-libstdc++ -Wall -Wextra -Wno-unknown-pragmas -Isdk/include"
if /i "%MODE%"=="release" (
    set "OPT=-O2 -flto=auto -ffunction-sections -fdata-sections -DNDEBUG -s -Wl,--gc-sections"
) else (
    set "OPT=-O0 -g -DYTR_DEBUG_BUILD"
)
set "LIBS=-lgdiplus -lole32 -luuid -lshlwapi -lshell32 -ldwmapi -lurlmon"

echo [*] Compiling resources (icon, manifest, version, page bridge)...
windres.exe -Ires -i res/resource.rc -o res/resource.o -O coff || (echo [!] Resource compilation failed. & exit /b 1)

echo [*] Compiling and linking...
g++.exe %COMMON% %OPT% ^
    src/main.cpp ^
    src/main_window.cpp ^
    src/miniplayer.cpp ^
    src/webview_engine.cpp ^
    src/tray.cpp ^
    src/taskbar_controls.cpp ^
    res/resource.o ^
    -o "bin\ytr-music.exe" %LIBS% || (echo [!] Compilation failed. & exit /b 1)

rem Keep the pinned taskbar shortcut (pack\win-unpacked\YouTube Music.exe) on the latest build.
if exist "..\pack\win-unpacked" (
    echo [*] Updating ..\pack\win-unpacked for the pinned taskbar shortcut...
    copy /y "bin\ytr-music.exe" "..\pack\win-unpacked\ytr-music.exe" >nul
    copy /y "bin\ytr-music.exe" "..\pack\win-unpacked\YouTube Music.exe" >nul
)

for %%F in ("bin\ytr-music.exe") do echo [*] Done: native\bin\ytr-music.exe (%%~zF bytes)
exit /b 0
