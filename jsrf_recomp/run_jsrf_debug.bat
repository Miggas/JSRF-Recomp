@echo off
rem Same as run_jsrf.bat, but stops after 30 s with a watchdog dump and hang samples at 27 s.
setlocal
set RECOMP_WATCHDOG_SECS=30
call "%~dp0run_jsrf.bat"
