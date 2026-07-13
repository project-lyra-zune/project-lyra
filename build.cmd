@echo off
setlocal
rem build.cmd - Phase 1 (Windows 7): build the Project Lyra device binaries.
rem Packaging is Phase 2 and runs on a .NET 8 machine; see BUILDING.md.
rem Prereqs: VS2008 + OpenZDK, XNA Game Studio 3.1, the NVIDIA Tegra CE6 SDK with
rem %NV_WINCE_T2_PLAT% set.
cd /d "%~dp0"

rem Platform binaries (devenv).
call "tools\build-local.cmd" nativeapp
if errorlevel 1 goto :fail
call "tools\build-local.cmd" zuxhook
if errorlevel 1 goto :fail
call "tools\build-local.cmd" exploiter
if errorlevel 1 goto :fail

if not exist "src\ce-common\out\wolfssl\wolfssl_ce_arm.lib" (
  call "src\ce-common\build\build_wolfssl.bat"
  if errorlevel 1 goto :fail
  cd /d "%~dp0"
)

if not exist "src\ce-common\out\ce_image\ce_image_ce_arm.lib" (
  call "src\ce-common\build\build_ce_image.bat"
  if errorlevel 1 goto :fail
  cd /d "%~dp0"
)

rem mods-tab's native components (nmake). Copy each output to the mod's
rem deployable location, where Phase 2 bundles it.
call "lyra\platform\mods-tab\src\reposd\build_reposd.bat"
if errorlevel 1 goto :fail
cd /d "%~dp0"
copy /y "lyra\platform\mods-tab\src\reposd\out\reposd.exe" "lyra\platform\mods-tab\reposd.exe" >nul

echo.
echo ^>^> Phase 1 done: device binaries built.
echo    Phase 2 (packaging) needs .NET 8 and cannot run here. On a Win10+/macOS/Linux
echo    box with the five binaries in place, run tools\packaging\build-release.cmd
echo    (or build-release.sh).
exit /b 0

:fail
echo.
echo BUILD FAILED.
exit /b 1
