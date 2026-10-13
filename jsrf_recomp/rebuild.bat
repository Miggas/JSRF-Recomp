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

if not exist "build\CMakeCache.txt" (
  echo Configuring...
  "%CMAKE%" -S . -B build -G "Visual Studio 17 2022" -A x64
  if errorlevel 1 goto fail
)

rem v118b: the generated bodies recomp_manual.c wraps get their _gen names
rem first, every time (idempotent), so a skipped wrap_dsound.bat cannot break
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

:fail
echo.
echo BUILD FAILED - scroll up for the first error.
if /i "%~1"=="nopause" exit /b 1
pause
exit /b 1
