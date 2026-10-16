@echo off
rem Jet Set Radio Future recomp -- play, without the diagnostics.
rem Same game settings as run_jsrf.bat, but no traces, no periodic reports and
rem no pushbuffer survey, so the frame rate is the game's own. Crashes are
rem still written to stderr.txt. Use run_jsrf.bat when sending a log.
setlocal
cd /d "%~dp0"

set RECOMP_PB_EXEC=1
set RECOMP_VBLANK=1
set RECOMP_FB_WINDOW=1
set RECOMP_TITLE_KEVENTS=1
set RECOMP_AC97_READY=1
set RECOMP_USB=1
rem How many controllers (1-4, default 1); 2 or more for multiplayer:
rem set RECOMP_USB_PADS=2
set RECOMP_KEYBOARD=1
rem Diagnostics off:
set RECOMP_QUIET=1
set RECOMP_KERNEL_LOG_BUDGET=0
set RECOMP_FF_TRACE=
set RECOMP_VSH_TRACE=
set RECOMP_FRAME_TRACE=
if not defined RECOMP_WATCHDOG_SECS set RECOMP_WATCHDOG_SECS=0
rem Optional, as in run_jsrf.bat:
rem set RECOMP_APU_GAIN=0.5
rem internal resolution: 2 = 1280x960, 3 = 1920x1440, 4 = 2560x1920
set RECOMP_GL_SCALE=2
rem set RECOMP_FPS_CAP=60

if not exist "build\Release\jsrf_recomp.exe" (
  echo build\Release\jsrf_recomp.exe not found - build first.
  pause
  exit /b 1
)
"build\Release\jsrf_recomp.exe" > nul 2> stderr.txt
