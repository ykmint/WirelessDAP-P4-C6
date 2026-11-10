@echo off
setlocal
cd /d "%~dp0.."
call "E:\Visual Studio\18\Insiders\VC\Auxiliary\Build\vcvars32.bat" >nul
cl /nologo /O2 /EHsc /MT /std:c++20 tests\rddi_smoke.cpp /Fobuild\rddi_smoke.obj /Febuild\rddi_smoke.exe
exit /b %ERRORLEVEL%
