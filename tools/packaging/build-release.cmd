@echo off
setlocal
rem Phase 2: assemble the Lyra release packages from the Phase 1 device binaries.
rem Needs .NET 8 (XUIHelper), Python 3 + Pillow + rsvg-convert, and csc. The five
rem Phase 1 binaries must be in place. Regenerates class blobs, .bgra, and .xur
rem from source, then writes dist\lyra-hd-deploykit and dist\lyra-hd.ccgame.
rem Extra args pass through.
cd /d "%~dp0..\.."
python "tools\packaging\build-release-packages.py" ^
  --exploiter "src\exploiter\bin\Zune\Debug\exploiter.exe" ^
  --nativeapp "src\nativeapp\bin\OpenZDK (ARMV4I)\Release\nativeapp.exe" ^
  --zuxhook "src\zuxhook\bin\OpenZDK (ARMV4I)\Release\zuxhook.dll" ^
  --out "dist" %*
