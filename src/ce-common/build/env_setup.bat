@echo off
rem Shared ARM CE 6 toolchain environment for all ce-common consumers
rem (zune-browser, zune-cast, zune-yt). Single source of truth.
rem Call this with: call env_setup.bat

set VCARM=C:\Program Files (x86)\Microsoft Visual Studio 9.0\VC\ce\bin\x86_arm
set VCIDE=C:\Program Files (x86)\Microsoft Visual Studio 9.0\Common7\IDE
set OPENZDK=C:\Program Files (x86)\Windows CE Tools\wce600\OpenZDK
set CELIB=C:\Program Files (x86)\Microsoft Visual Studio 9.0\VC\ce\lib\armv4i

set PATH=%VCARM%;%VCIDE%;%PATH%
set INCLUDE=%OPENZDK%\Include\Armv4i
set LIB=%OPENZDK%\Lib\ARMV4I;%CELIB%

set CE_CC="%VCARM%\cl.exe"
set CE_LINK="%VCARM%\link.exe"
set CE_LIB="%VCARM%\lib.exe"

rem Common compiler flags for all CE ARM targets
set CE_CFLAGS=/DZUNE_HD /D_WIN32_WCE=0x0600 /DUNDER_CE /DUNICODE /D_UNICODE ^
  /DARM /D_ARM_ /Darm /DTHUMB /D_THUMB_ ^
  /nologo /W3 /O2 /GS- /Gy

set CE_LFLAGS=/SUBSYSTEM:WINDOWSCE /MACHINE:THUMB /NODEFAULTLIB:oldnames.lib ^
  /NODEFAULTLIB:kernel32.lib /NODEFAULTLIB:advapi32.lib
