@echo off
setlocal
cd /d "%~dp0"
rem MSBuild's file tracker breaks on very long paths, so build outside the source tree.
set "BUILD=%TEMP%\errt_build"
cmake -S . -B "%BUILD%" -G "Visual Studio 17 2022" -A x64 || exit /b 1
cmake --build "%BUILD%" --config Release || exit /b 1
if not exist dist mkdir dist
copy /y "%BUILD%\Release\dinput8.dll" dist\ >nul
copy /y errt_fix.ini dist\ >nul
echo.
echo Built: %~dp0dist\dinput8.dll
