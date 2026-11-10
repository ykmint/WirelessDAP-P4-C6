@echo off
setlocal
cd /d "%~dp0.."
where cl >nul 2>nul
if errorlevel 1 exit /b 1
cl /nologo /O2 /EHsc /MT /std:c++20 /utf-8 /DUNICODE /D_UNICODE src\common.cpp src\transport.cpp tests\i2c_radio_diagnostic.cpp /Fobuild\ /Febuild\i2c_radio_diagnostic.exe /link windowsapp.lib ole32.lib
exit /b %ERRORLEVEL%
