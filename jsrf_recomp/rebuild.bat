@echo off
rem One-click rebuild of jsrf_recomp (Release). Configures the build folder on first use.
setlocal
cd /d "%~dp0"

rem Find cmake: PATH first, then the copy that ships with Visual Studio 2022.
set "CMAKE=cmake"
where cmake >nul 2>nul
if not errorlevel 1 goto have_cmake
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" goto have_cmake
for /f "usebackq delims=" %%I in (`"%VSWHERE%" -latest -products * -property installationPath`) do set "VSDIR=%%I"
if defined VSDIR set "CMAKE=%VSDIR%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
:have_cmake

rem The recompiled code is generated from your own default.xbe, not shipped.
if not exist "src\recomp\gen\recomp_dispatch.c" (
  echo No recompiled code yet in src\recomp\gen.
  echo Put your game files in jsrf_recomp\game\ and run generate_code.bat first.
  goto fail
)

rem A setup that failed halfway leaves CMakeCache.txt without the project
rem files (MSB1009: Project file does not exist), so check for the project
rem itself and start the build folder over if it is missing.
if exist "build\ALL_BUILD.vcxproj" goto configured
if exist "build" (
  echo The build folder is incomplete ^(an earlier setup failed^) - starting it over.
  rmdir /s /q build
)
echo Configuring...
"%CMAKE%" -S . -B build -G "Visual Studio 17 2022" -A x64
if errorlevel 1 goto config_fail
if not exist "build\ALL_BUILD.vcxproj" goto config_fail
:configured

rem v118b: the generated bodies recomp_manual.c wraps get their _gen names
rem first, every time (idempotent), so the link cannot break
rem the link.
set "PY=py -3"
where py >nul 2>&1 || set "PY=python"
%PY% "%~dp0tools\wrap_dsound.py" "%~dp0src\recomp\gen"
if errorlevel 1 goto fail

echo Building (Release)...
"%CMAKE%" --build build --config Release --parallel
if errorlevel 1 goto fail

echo.
echo Build OK: build\Release\jsrf_recomp.exe
if /i "%~1"=="nopause" exit /b 0
echo Run run_jsrf.bat to start the game.
pause
exit /b 0

:config_fail
echo.
echo SETUP FAILED - the first error is just above, after "Configuring...".
echo Usually Visual Studio 2022 is missing the "Desktop development with C++"
echo workload or a Windows SDK: open the Visual Studio Installer, Modify, and
echo tick them. Then run rebuild.bat again.
if exist "build" rmdir /s /q build
if /i "%~1"=="nopause" exit /b 1
pause
exit /b 1

:fail
echo.
echo BUILD FAILED - scroll up for the first error.
if /i "%~1"=="nopause" exit /b 1
pause
exit /b 1
