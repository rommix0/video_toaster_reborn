@echo off
rem Configure, build and test with MSVC (Ninja). Run from anywhere.
rem Usage: tools\build.bat [extra ctest args, e.g. -R crc32]
setlocal
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
cd /d "%~dp0.."
cmake --preset msvc || exit /b 1
cmake --build --preset msvc || exit /b 1
ctest --preset msvc %* || exit /b 1
