@echo off
cd /d "%~dp0"
call ..\..\..\src\ce-common\build\env_setup.bat
set NMAKE=C:\Program Files (x86)\Microsoft Visual Studio 9.0\VC\bin\nmake.exe
"%NMAKE%" /f playnext_gem.mak %*
