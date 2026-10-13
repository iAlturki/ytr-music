@rem SPDX-License-Identifier: CC-BY-NC-SA-4.0
@rem Copyright (c) 2026 iAlturki
@echo off
setlocal
title ytr-music (iAlturki Native C++ Edition)

echo ========================================================
echo   ytr-music Native (iAlturki Reconstructed Edition)
echo   Pure C++ Win32 ^| Ultra-Lightweight ^| Sub-1MB Footprint
echo ========================================================
echo.

cd /d "%~dp0"

:: Terminate any stale instances
taskkill /F /IM "ytr-music.exe" >nul 2>&1
taskkill /F /IM "YouTube Music.exe" >nul 2>&1
taskkill /F /IM "ytr-music-native.exe" >nul 2>&1

:: Check if native binary exists, compile if missing
if not exist "native\bin\ytr-music.exe" (
    echo [*] Compiling native C++ client...
    call native\build.bat
)

echo [*] Launching ytr-music Native Edition...
start "" "native\bin\ytr-music.exe"
echo [*] App running on your screen with extreme native speed!
echo.
