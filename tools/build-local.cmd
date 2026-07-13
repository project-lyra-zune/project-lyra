@echo off
setlocal
rem build-local.cmd - build a Project Lyra device binary in place.
rem Requires Windows 7 + Visual Studio 2008 (VC9). nativeapp/zuxhook/plugins need
rem the OpenZDK (ARMV4I) platform (and, for nativeapp, the NVIDIA Tegra CE6 SDK
rem with %NV_WINCE_T2_PLAT% set, to compile the install-splash shaders). The
rem exploiter needs XNA Game Studio 3.1.
rem
rem   tools\build-local.cmd nativeapp
rem   tools\build-local.cmd zuxhook
rem   tools\build-local.cmd exploiter
rem   tools\build-local.cmd plugin hello           (builds plugins\plugin-hello)
rem
rem On a plain clone kerncore is already where each project expects it (src targets
rem reference ..\kerncore; plugins reference ..\..\src\kerncore), so no path setup.
rem Override devenv autodetection by pre-setting DEVENV.
rem
rem Note: error/report text is echoed from labels, never inside an if(...) block.
rem The VS install path contains "(x86)"; a ")" expanded inside a parenthesized
rem block closes it early ("... was unexpected at this time"). Plain-line echoes
rem treat the paren as literal output.

set "T1=%~1"
set "T2=%~2"
set "CONFIG=Release|OpenZDK (ARMV4I)"
set "OUTSUB=bin\OpenZDK (ARMV4I)\Release"
if /I "%T1%"=="nativeapp"  goto :std
if /I "%T1%"=="zuxhook"    goto :std
if /I "%T1%"=="exploiter"  goto :exploiter
if /I "%T1%"=="plugin"     goto :plugin
goto :usage

:std
set "PROJDIR=%~dp0..\src\%T1%"
set "PROJ=%T1%.vcproj"
if /I "%T1%"=="nativeapp" set "OUTFILE=nativeapp.exe"
if /I "%T1%"=="zuxhook"   set "OUTFILE=zuxhook.dll"
if /I "%T1%"=="nativeapp" call :ensure_tegra
if /I "%T1%"=="nativeapp" if not defined NV_WINCE_T2_PLAT goto :no_tegra
goto :haveproj

:exploiter
set "PROJDIR=%~dp0..\src\exploiter"
set "PROJ=exploiter.csproj"
set "OUTFILE=exploiter.exe"
set "CONFIG=Debug|Zune"
set "OUTSUB=bin\Zune\Debug"
goto :haveproj

:plugin
if "%T2%"=="" goto :usage
set "PROJDIR=%~dp0..\plugins\plugin-%T2%"
set "PROJ=plugin-%T2%.vcproj"
set "OUTFILE=plugin-%T2%.dll"
goto :haveproj

:haveproj
if not exist "%PROJDIR%\%PROJ%" goto :no_proj

rem Locate devenv.com (VS2008 / VC9). Honor a pre-set DEVENV, else autodetect.
if not defined DEVENV call :find_devenv
if not defined DEVENV goto :no_devenv
if not exist "%DEVENV%" goto :bad_devenv

echo ^>^> building %PROJ% : %CONFIG:|=/%
pushd "%PROJDIR%"
"%DEVENV%" "%PROJ%" /Rebuild "%CONFIG%"
set "RC=%ERRORLEVEL%"
popd
if not "%RC%"=="0" goto :build_failed

echo.
echo ^>^> built: %PROJDIR%\%OUTSUB%\%OUTFILE%
exit /b 0

rem Resolve each candidate into a plain var before testing it: %VS90COMNTOOLS%
rem and %ProgramFiles(x86)% both contain "(x86)", which breaks a conditional if
rem expanded inline. A plain SET is safe; the quoted %CAND% test then is too.
:find_devenv
if not defined VS90COMNTOOLS goto :cand_pf
set "CAND=%VS90COMNTOOLS%..\IDE\devenv.com"
call :try_cand
if defined DEVENV goto :eof
:cand_pf
set "CAND=%ProgramFiles%\Microsoft Visual Studio 9.0\Common7\IDE\devenv.com"
call :try_cand
if defined DEVENV goto :eof
set "CAND=%ProgramFiles(x86)%\Microsoft Visual Studio 9.0\Common7\IDE\devenv.com"
call :try_cand
goto :eof

:try_cand
if not defined DEVENV if exist "%CAND%" set "DEVENV=%CAND%"
goto :eof

rem nativeapp's shaders compile with the NVIDIA Tegra CE6 SDK (cgc/shaderfix),
rem found via %NV_WINCE_T2_PLAT%. If unset, default it to the SDK's standard
rem install path; a non-standard install just needs the env var set beforehand.
:ensure_tegra
if defined NV_WINCE_T2_PLAT goto :eof
set "CAND=%ProgramFiles(x86)%\NVIDIA Corporation\ce6_tegra_250_5265393"
if exist "%CAND%\host_bin\cgc.exe" set "NV_WINCE_T2_PLAT=%CAND%"
goto :eof

:no_proj
echo ERROR: %PROJDIR%\%PROJ% not found. Run this from a Project Lyra clone.
exit /b 1

:no_devenv
echo ERROR: devenv.com (Visual Studio 2008 / VC9) not found.
echo   Install VS2008, or set DEVENV to the full path of devenv.com.
exit /b 1

:bad_devenv
echo ERROR: DEVENV points at a missing file: %DEVENV%
exit /b 1

:no_tegra
echo ERROR: NVIDIA Tegra CE6 SDK not found (needed to compile nativeapp's shaders).
echo   Install it, or set NV_WINCE_T2_PLAT to its root (the dir holding host_bin\).
exit /b 1

:build_failed
echo.
echo BUILD FAILED (devenv rc=%RC%). Diagnose with the build log:
echo   %PROJDIR%\%OUTSUB:bin\=obj\%\BuildLog.htm
exit /b %RC%

:usage
echo Usage: tools\build-local.cmd {nativeapp^|zuxhook^|exploiter^|plugin ^<name^>}
exit /b 2
