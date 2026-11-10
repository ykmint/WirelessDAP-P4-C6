@echo off
setlocal
set "APP_OUTPUT=build\WirelessDAP-dev.exe"
if "%~1"=="release" set "APP_OUTPUT=release\WirelessDAP.exe"
cd /d "%~dp0"
where cl >nul 2>nul
if errorlevel 1 echo Run this script from an x64 Native Tools Command Prompt for Visual Studio.
if errorlevel 1 exit /b 1
if not exist build mkdir build
if not exist release mkdir release
call tests\build_vterm.cmd
if errorlevel 1 exit /b 1
cl /nologo /O2 /EHsc /MT /std:c++20 /utf-8 src\packtool.cpp /Fobuild\packtool.obj /Febuild\packtool.exe
if errorlevel 1 exit /b 1
build\packtool.exe build\payload assets\payload.bin
if errorlevel 1 exit /b 1
rc /nologo /c65001 /fobuild\app.res assets\app.rc
if errorlevel 1 exit /b 1
cl /nologo /O2 /EHsc /MT /std:c++20 /utf-8 /DUNICODE /D_UNICODE src\common.cpp src\transport.cpp src\engine.cpp src\stlink_bridge.cpp src\stlink_tests.cpp src\tests.cpp src\interfaces.cpp src\keil_bridge.cpp src\ssh_session.cpp src\ssh_panel.cpp src\ssh_tests.cpp src\app.cpp build\app.res build\vterm.lib /Fobuild\ /Fe%APP_OUTPUT% /link /SUBSYSTEM:WINDOWS /MANIFEST:NO windowsapp.lib user32.lib gdi32.lib ole32.lib
exit /b %ERRORLEVEL%
