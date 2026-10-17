@echo off
rem Check the generated code (src\recomp\gen) against the code this version
rem expects (analysis\gen_hashes.txt). Run after generate_code.bat and rebuild.bat.
rem "all 42 files match" means you are compiling exactly the expected code.
setlocal
set "PY=py -3"
where py >nul 2>&1 || set "PY=python"
%PY% "%~dp0..\tools\check_gen.py"
echo.
pause
