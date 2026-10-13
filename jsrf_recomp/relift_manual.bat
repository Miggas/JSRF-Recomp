@echo off
setlocal
rem v115b: re-generate the recompiled code after recomp_manual.c gains or loses a
rem wrapped function (the last two lift steps; the analysis is unchanged).
rem Usage: relift_manual.bat [path to the xboxrecomp clone]
set "JR=%~dp0"
set "XR=%~1"
if "%XR%"=="" set "XR=%JR%..\xboxrecomp"
echo jsrf_recomp : %JR%
echo xboxrecomp  : %XR%
if not exist "%XR%\tools\recomp\__main__.py" (
  echo.
  echo ERROR: no xboxrecomp clone at that path ^(tools\recomp\__main__.py not found^).
  echo Run it as:  relift_manual.bat "C:\path\to\xboxrecomp"
  goto :fail
)
set "PY=py -3"
where py >nul 2>&1 || set "PY=python"
%PY% --version || (echo ERROR: Python not found & goto :fail)
pushd "%XR%" || goto :fail
if not exist game_files\default.xbe (
  if exist "%JR%game\default.xbe" (
    if not exist game_files mkdir game_files
    copy /y "%JR%game\default.xbe" game_files\ >nul
  ) else (
    echo ERROR: game_files\default.xbe missing in the xboxrecomp clone
    popd & goto :fail
  )
)
echo.
echo Lifting ^(about a minute^)...
%PY% -m tools.recomp game_files\default.xbe --all --split 250 --game-name "Jet Set Radio Future" --gen-dir "%JR%src\recomp\gen" --exclude-manual "%JR%src\recomp_manual.c"
if errorlevel 1 (popd & echo. & echo LIFT FAILED & goto :fail)
%PY% "%JR%tools\log_stubs.py" "%JR%src\recomp\gen\recomp_stubs_unresolved.c"
if errorlevel 1 (popd & echo. & echo log_stubs FAILED & goto :fail)
%PY% "%JR%tools\wrap_dsound.py" "%JR%src\recomp\gen"
if errorlevel 1 (popd & echo. & echo wrap_dsound FAILED & goto :fail)
popd
echo.
echo Lift done -- now run rebuild.bat
pause
exit /b 0
:fail
echo.
pause
exit /b 1
