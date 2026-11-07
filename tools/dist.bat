@echo off
rem Build and test (tools\build.bat), then package the app as build\dist\vtoast-reborn-alpha-<commit>\ and .zip.
rem The package holds vtoast.exe, the Qt runtime, the FFmpeg DLLs, the MSVC runtime, README.md and licences.
rem Usage: tools\dist.bat
setlocal
call "%~dp0build.bat" || exit /b 1
cd /d "%~dp0.."
for /f %%h in ('git rev-parse --short HEAD') do set NAME=vtoast-reborn-alpha-%%h
set APP=build\msvc\app
set OUT=build\dist\%NAME%
set FFMPEG=..\ffmpeg-9.0.2-full_build-shared

mkdir "%OUT%\licenses" 2>nul
copy /y "%APP%\vtoast.exe" "%OUT%\" >nul || exit /b 1
copy /y "%APP%\*.dll" "%OUT%\" >nul || exit /b 1
for %%d in (generic iconengines imageformats networkinformation platforms styles tls) do (
    xcopy /e /i /y /q "%APP%\%%d" "%OUT%\%%d" >nul || exit /b 1
)
rem windeployqt runs with --no-compiler-runtime, so the MSVC runtime comes from the redist folder.
for /d %%v in ("C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Redist\MSVC\14.*") do set CRT=%%v\x64\Microsoft.VC143.CRT
copy /y "%CRT%\*.dll" "%OUT%\" >nul || exit /b 1
copy /y README.md "%OUT%\" >nul || exit /b 1
copy /y "%FFMPEG%\LICENSE" "%OUT%\licenses\FFmpeg-LICENSE.txt" >nul || exit /b 1
copy /y "%FFMPEG%\README.txt" "%OUT%\licenses\FFmpeg-README.txt" >nul || exit /b 1

powershell -NoProfile -Command "Compress-Archive -Force -Path '%OUT%' -DestinationPath '%OUT%.zip'" || exit /b 1
echo Packaged %OUT% and %OUT%.zip
