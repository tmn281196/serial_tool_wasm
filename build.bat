@echo off
rem Build Serial Tool to WebAssembly with CMake + Ninja, output in web\.
rem Usage: build.bat          (Release)
rem        build.bat debug    (Debug, slower but with assertions)
rem        build.bat clean    (remove the build folders first)
rem Toolchain (emsdk, cmake, ninja) comes from TOOLS; defaults to the tien-len project's tools\.
setlocal
cd /d "%~dp0"
if "%TOOLS%"=="" set "TOOLS=D:\tmniosc\#lab\webstack\tien-len\tools"
rem Started from Git Bash these make emsdk_env print bash exports instead of setting cmd variables.
set MSYSTEM=
set SHELL=
set EMSDK_QUIET=1
call "%TOOLS%\emsdk\emsdk_env.bat" >nul
set "PATH=%TOOLS%\cmake\bin;%TOOLS%\ninja;%PATH%"
set PRESET=web
if "%1"=="debug" set PRESET=web-debug
if "%1"=="clean" rmdir /s /q build 2>nul
cmake --preset %PRESET% >nul || exit /b 1
cmake --build --preset %PRESET% || exit /b 1
echo Done: web\index.html
