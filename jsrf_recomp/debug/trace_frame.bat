@echo off
rem Double-click while the game is running: the next whole frame is logged to
rem stderr.txt ([FTRACE] lines). The flag is removed again after 5 seconds so a
rem later double-click can trace another moment.
rem The flag goes in jsrf_recomp, where run_jsrf.bat runs the game.
cd /d "%~dp0.."
type nul > trace.flag
echo Tracing one frame...
timeout /t 5 /nobreak > nul
del trace.flag
echo Done.
