@echo off
rem Release build: builds the game (as rebuild.bat) and puts jsrf_recomp.exe
rem into the game folder, next to default.xbe, with jsrf.ini for its
rem settings. Nothing else is copied. Double-click the exe to play; the
rem folder can be moved to another PC or a Steam Deck as it is.
rem
rem   build_portable.bat            game folder = game\ (here)
rem   build_portable.bat D:\JSRF    a game folder somewhere else
setlocal
cd /d "%~dp0"
set "DEST=%~1"
if "%DEST%"=="" set "DEST=%~dp0game"

if not exist "%DEST%\default.xbe" (
  echo default.xbe not found in "%DEST%".
  echo Put the game files there first, or pass the game folder:
  echo   build_portable.bat D:\path\to\game
  pause
  exit /b 1
)

call "%~dp0rebuild.bat" nopause
if errorlevel 1 (
  echo.
  echo BUILD FAILED - scroll up for the first error.
  pause
  exit /b 1
)

copy /y "build\Release\jsrf_recomp.exe" "%DEST%\jsrf_recomp.exe" > nul
if errorlevel 1 (
  echo Could not copy the exe to "%DEST%" ^(is the game still running?^).
  pause
  exit /b 1
)

rem Old compiled shaders cleared, recipes.bin kept (rebuild.bat does the
rem game\ folder; this is for a game folder somewhere else).
call :clear_cache "%DEST%\shader_cache"

rem Your jsrf.ini is never overwritten.
if not exist "%DEST%\jsrf.ini" (
  copy /y "portable\jsrf.ini" "%DEST%\jsrf.ini" > nul
  echo Created "%DEST%\jsrf.ini".
) else (
  echo Kept your "%DEST%\jsrf.ini".
)

echo.
echo Release build ready: "%DEST%\jsrf_recomp.exe"
pause
exit /b 0

rem v242b: empty a shader cache folder, keeping recipes.bin (the list of
rem shaders the game used, so the new build compiles them again in menus)
:clear_cache
if not exist "%~1\" exit /b 0
for %%F in ("%~1\*.bin") do if /i not "%%~nxF"=="recipes.bin" del /q "%%F" 2>nul
echo Cleared the shader cache in "%~1" ^(kept recipes.bin^).
exit /b 0
