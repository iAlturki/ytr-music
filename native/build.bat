@echo off
setlocal
echo ========================================================
echo   Building ytr-music Native (iALTURKi Edition)
echo   Toolchain: MinGW-W64 GCC 16.1.0 UCRT
echo ========================================================
echo.

cd /d "%~dp0"

if not exist "bin" mkdir bin
if not exist "bin\WebView2Loader.dll" (
    copy /y "sdk\x64\WebView2Loader.dll" "bin\" >nul
)

echo [*] Compiling Windows resource file (icon and manifest)...
windres.exe -Ires -i res/resource.rc -o res/resource.o -O coff
if %ERRORLEVEL% NEQ 0 (
    echo [!] Resource compilation failed!
    exit /b %ERRORLEVEL%
)

echo [*] Compiling C++ source files...
g++.exe -std=c++17 -O3 -s -mwindows -static -static-libgcc -static-libstdc++ ^
    -Inative/sdk/include -Isdk/include ^
    src/main.cpp ^
    src/main_window.cpp ^
    src/miniplayer.cpp ^
    src/webview_engine.cpp ^
    src/tray.cpp ^
    src/taskbar_controls.cpp ^
    res/resource.o ^
    -o "bin\ytr-music.exe" ^
    -lgdiplus -lcomctl32 -lole32 -loleaut32 -lshlwapi -lpsapi -luuid -lshell32 -ldwmapi -lurlmon

if %ERRORLEVEL% NEQ 0 (
    echo [!] Compilation failed with error %ERRORLEVEL%!
    exit /b %ERRORLEVEL%
)

copy /y "bin\ytr-music.exe" "bin\ytr-music-native.exe" >nul
copy /y "bin\ytr-music.exe" "bin\YouTube Music.exe" >nul

echo [*] Deploying to pack\win-unpacked for pinned taskbar shortcut...
if exist "..\pack\win-unpacked" (
    if not exist "..\pack\win-unpacked\YouTube Music.exe.old_electron" (
        copy /y "..\pack\win-unpacked\YouTube Music.exe" "..\pack\win-unpacked\YouTube Music.exe.old_electron" >nul
    )
    copy /y "bin\ytr-music.exe" "..\pack\win-unpacked\ytr-music.exe" >nul
    copy /y "bin\ytr-music.exe" "..\pack\win-unpacked\YouTube Music.exe" >nul
    copy /y "sdk\x64\WebView2Loader.dll" "..\pack\win-unpacked\WebView2Loader.dll" >nul
)

echo [*] Compilation and Deployment Succeeded!
echo [*] Output: native\bin\ytr-music.exe
dir "bin\ytr-music.exe"
echo.
