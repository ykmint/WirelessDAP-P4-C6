@echo off
setlocal
cd /d "%~dp0.."
where cl >nul 2>nul
if errorlevel 1 exit /b 1
cl /nologo /O2 /EHsc /MT /std:c++20 /utf-8 /DUNICODE /D_UNICODE src\common.cpp src\ssh_session.cpp tests\ssh_loopback.cpp build\vterm.lib /Fobuild\ /Febuild\ssh_loopback.exe
exit /b %ERRORLEVEL%
