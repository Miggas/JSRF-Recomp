@echo off
rem Compare the game's hard-disk cache (game\savegame\Cache) with the disc files.
rem Usage: compare_cache.bat [game folder]   (default: jsrf_recomp\game)
setlocal
set "PY=py -3"
where py >nul 2>&1 || set "PY=python"
%PY% "%~dp0..\tools\compare_cache.py" %*
echo.
pause
