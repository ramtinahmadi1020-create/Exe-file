@echo off
setlocal EnableDelayedExpansion
chcp 65001 >nul

echo ============================================================
echo   VPN Echo 1.1.1.1  -  Downloader / Verifier
echo ============================================================
echo.

set "URL=https://raw.githubusercontent.com/ramtinahmadi1020-create/Exe-file/arena/63cfab41-exe-file/exe-file/VPN-Echo-Windows-x64.zip"
set "NAME=VPN-Echo-Windows-x64.zip"
set "EXPECTED=461b9b6359a1aa704224172eabf6efb844d0264f77a85187892b7f37c8b4fdc7"

if not exist "%NAME%" (
    echo Downloading %NAME% ...
    curl -L --fail --retry 3 -o "%NAME%" "%URL%"
    if errorlevel 1 (
        echo.
        echo [X] Download failed. Check your internet connection and run again.
        pause
        exit /b 1
    )
) else (
    echo %NAME% already exists, skipping download.
)

echo.
echo Verifying SHA-256 ...
set "HASH="
for /f "skip=1 tokens=* delims=" %%H in ('certutil -hashfile "%NAME%" SHA256') do (
    if not defined HASH set "HASH=%%H"
)
set "HASH=!HASH: =!"

echo   expected : %EXPECTED%
echo   actual   : !HASH!

if /i "!HASH!"=="%EXPECTED%" (
    echo.
    echo [OK] File is intact and complete.
) else (
    echo.
    echo [X] Hash mismatch - the file is damaged or incomplete.
    echo     Delete "%NAME%" and run this script again.
    pause
    exit /b 1
)

echo.
echo Extracting ...
tar -xf "%NAME%"
if errorlevel 1 (
    powershell -NoProfile -Command "Expand-Archive -Path '%NAME%' -DestinationPath '.' -Force"
)

echo.
echo ============================================================
echo   Ready. Two ways to use it:
echo.
echo   1) Installer  :  VPN-Echo-Setup.exe
echo   2) Portable   :  "VPN Echo\VPN Echo.exe"
echo.
echo   Run as Administrator (the app asks for it automatically).
echo ============================================================
echo.
explorer "%~dp0"
pause
