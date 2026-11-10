@echo off
setlocal
cd /d "%~dp0.."
where cl >nul 2>nul
if errorlevel 1 exit /b 1
cl /nologo /O2 /EHsc /MT /std:c++20 /utf-8 /DUNICODE /D_UNICODE src\common.cpp src\transport.cpp src\engine.cpp src\stlink_bridge.cpp tests\stlink_acceptance.cpp /Fobuild\ /Febuild\stlink_acceptance.exe /link windowsapp.lib ole32.lib
exit /b %ERRORLEVEL%
