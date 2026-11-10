@echo off
setlocal
cd /d "%~dp0.."
where cl >nul 2>nul
if errorlevel 1 exit /b 1
if not exist build\vterm mkdir build\vterm
cl /nologo /O2 /MT /std:c11 /D_CRT_SECURE_NO_WARNINGS /Ithird_party\libvterm\include /Ithird_party\libvterm\src /c third_party\libvterm\src\*.c /Fobuild\vterm\
if errorlevel 1 exit /b 1
lib /nologo /out:build\vterm.lib build\vterm\*.obj
exit /b %ERRORLEVEL%
