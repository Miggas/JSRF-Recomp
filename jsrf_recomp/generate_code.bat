@echo off
rem Generate the recompiled code (src\recomp\gen) from your own default.xbe.
rem Put your game files in jsrf_recomp\game\ first. Then run rebuild.bat.
rem Usage: generate_code.bat [path\to\default.xbe]
setlocal
set "PY=py -3"
where py >nul 2>&1 || set "PY=python"
%PY% --version >nul 2>&1 || (echo ERROR: Python 3 not found -- install it from python.org, ticking "Add python.exe to PATH" & goto :fail)
rem The translator needs the capstone package; install it if it is missing.
%PY% -c "import capstone" >nul 2>&1
if errorlevel 1 (
  echo Installing the Python package capstone ^(once^)...
  %PY% -m pip install --user capstone==5.0.7
  %PY% -c "import capstone" >nul 2>&1 || (echo ERROR: could not install capstone -- run: py -3 -m pip install capstone==5.0.7 & goto :fail)
)
%PY% "%~dp0tools\generate_code.py" %*
if errorlevel 1 goto :fail
echo.
pause
exit /b 0
:fail
echo.
echo GENERATION FAILED - scroll up for the first error.
pause
exit /b 1
