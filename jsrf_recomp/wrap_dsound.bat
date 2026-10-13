@echo off
rem v115c: rename the two generated DirectSound bodies so recomp_manual.c
rem can wrap them (no re-lift needed). Run once, then rebuild.bat.
setlocal
set "PY=py -3"
where py >nul 2>&1 || set "PY=python"
%PY% "%~dp0tools\wrap_dsound.py" "%~dp0src\recomp\gen"
if errorlevel 1 (echo. & echo FAILED & pause & exit /b 1)
echo.
echo Done -- now run rebuild.bat
pause
