@echo off
rem Check that default.xbe holds the game code the project is made for
rem (the NTSC retail release). Usage: check_xbe.bat [path\to\default.xbe]
rem (default: jsrf_recomp\game\default.xbe)
setlocal
set "PY=py -3"
where py >nul 2>&1 || set "PY=python"
%PY% "%~dp0..\tools\xbe_info.py" %*
echo.
pause
