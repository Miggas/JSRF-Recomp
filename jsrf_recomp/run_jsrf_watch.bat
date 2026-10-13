@echo off
rem Jet Set Radio Future recomp launcher (heap-corruption watch build): sets the xboxrecomp runtime switches, then runs.
rem Double-click it, or run it from cmd or PowerShell. Output goes to stderr.txt here.
setlocal
cd /d "%~dp0"

rem --- GPU: let the XDK's own D3D run, and actually consume the pushbuffer it builds
set RECOMP_PB_EXEC=1
rem --- 60 Hz vblank interrupt, so D3D's Swap / BlockUntilVerticalBlank waits complete
set RECOMP_VBLANK=1
rem --- Audio: report the AC97 codec ready and route the APU to the emulated one.
rem     The game links the XDK's DirectSound; if its probe fails, games often skip engine init.
set RECOMP_AC97_READY=1
rem --- If nothing progresses for 15 s, dump registers, push buffer PUT/GET and the guest stack
set RECOMP_WATCHDOG_SECS=15
rem --- Log every write to the heap free-list link found corrupted in run 3
set RECOMP_WATCH=0x0105EE68

if not exist "build\Release\jsrf_recomp.exe" (
  echo build\Release\jsrf_recomp.exe not found - build first.
  pause
  exit /b 1
)
echo Running... output goes to stderr.txt. Close the window or end the process when it stalls.
"build\Release\jsrf_recomp.exe" 2> stderr.txt
echo Exit code %ERRORLEVEL%
