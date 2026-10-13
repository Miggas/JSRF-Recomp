@echo off
rem Double-click while the game is frozen: the main game thread is sampled three
rem times and where it is stuck goes to stderr.txt ([SAMPLE] lines).
rem The flag goes in jsrf_recomp, where run_jsrf.bat runs the game.
cd /d "%~dp0.."
type nul > sample.flag
echo Sampling... (stderr.txt will have [SAMPLE] lines in a few seconds)
timeout /t 4 /nobreak > nul
