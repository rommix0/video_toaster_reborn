@echo off
rem Runs only the tests that need devices or a desktop (CTest label "live"). Run from anywhere.
setlocal
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
cd /d "%~dp0.."
cmake --preset msvc || exit /b 1
cmake --build --preset msvc || exit /b 1
ctest --preset msvc -L live %* || exit /b 1
