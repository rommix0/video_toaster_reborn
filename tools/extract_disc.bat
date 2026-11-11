@echo off
rem Extract 4.2\ from your own Video Toaster v4.3 CD image into data\ (the files not in the repository stay git-ignored). Needs 7-Zip.
rem Usage: tools\extract_disc.bat "path\to\Video Toaster v4.3.iso"
setlocal
if "%~1"=="" (
    echo Usage: tools\extract_disc.bat "path\to\Video Toaster v4.3.iso"
    exit /b 1
)
if not exist "%~f1" (
    echo CD image not found: "%~f1"
    exit /b 1
)
set "ISO=%~f1"
cd /d "%~dp0.."

set "SEVENZIP=7z"
where 7z >nul 2>nul || set "SEVENZIP=%ProgramFiles%\7-Zip\7z.exe"
"%SEVENZIP%" x -y -bso0 -bsp0 -odata "%ISO%" 4.2 || (
    echo Extraction failed. Is 7-Zip installed?
    exit /b 1
)

rem The v4.2 CD has no 4.2\ folder and its switcher.main is a different build.
powershell -NoProfile -Command "if ((Get-FileHash -LiteralPath 'data\4.2\Programs\switcher.main' -Algorithm SHA256 -ErrorAction SilentlyContinue).Hash -ne '0C7BF7326F8E1B245ED49159AA874FB4FD19D1BAC2E591F7D4F6D0382B1CEEDE') { exit 1 }" || (
    echo This is not the Video Toaster v4.3 CD: data\4.2\Programs\switcher.main is missing or different.
    exit /b 1
)
echo Extracted to data\
