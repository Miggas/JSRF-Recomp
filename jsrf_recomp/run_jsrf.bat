@echo off
rem Jet Set Radio Future recomp launcher: sets the xboxrecomp runtime switches, then runs.
rem Double-click it, or run it from cmd or PowerShell. Output goes to stderr.txt here.
setlocal
cd /d "%~dp0"

rem --- GPU: let the XDK's own D3D run, and actually consume the pushbuffer it builds
set RECOMP_PB_EXEC=1
rem --- 60 Hz vblank interrupt, so D3D's Swap / BlockUntilVerticalBlank waits complete
set RECOMP_VBLANK=1
rem --- Show the game's framebuffer in a window. Without it nothing is ever displayed:
rem     the runtime renders into guest memory and only this window shows it.
set RECOMP_FB_WINDOW=1
rem --- Events the game builds itself (D3D's vertical-blank event, CRI's threads wait on it):
rem     without this every such wait fails at once and those threads spin at 100% CPU.
set RECOMP_TITLE_KEVENTS=1
rem --- Audio: report the AC97 codec ready and route the APU to the emulated one.
rem     The game links the XDK's DirectSound; if its probe fails, games often skip engine init.
set RECOMP_AC97_READY=1
rem --- Controllers: emulate the USB host controller with one gamepad. Your real
rem     controller drives it through XInput; RECOMP_KEYBOARD=1 also maps the keyboard.
rem     Without this XAPI finds no USB hardware and the game's input setup fails.
set RECOMP_USB=1
set RECOMP_USB_PADS=1
set RECOMP_KEYBOARD=1
rem     Keyboard: Enter=START, Z X A S = A B X Y, arrows = d-pad, numpad 8/4/2/6 = left stick.
rem     The game window must have focus. INPUT_DIAG prints the input chain once a second,
rem     KEY_TRACE prints each key the window receives (diagnostics; remove when input works).
rem set RECOMP_INPUT_DIAG=1
rem set RECOMP_KEY_TRACE=1
rem set RECOMP_USB_STATS=1
rem --- Renderer (v39): triangles are drawn on the GPU through OpenGL 3.3. If the picture
rem     is wrong or the window stays black, uncomment the next line to compare with the
rem     software rasteriser (slow, but the reference the GPU path is checked against).
rem set RECOMP_RENDERER=sw
rem --- Speed (v50): vertices are transformed on several threads (default half the
rem     logical processors, at most 8) and drawn indexed. RECOMP_VERTEX_THREADS=0 keeps
rem     it on one thread; RECOMP_GL_LEGACY_SUBMIT=1 restores the v49 draw path.
rem set RECOMP_VERTEX_THREADS=8
rem set RECOMP_GL_LEGACY_SUBMIT=1
rem --- Profiler (v73): samples the main game thread ~1000 times a second and lists
rem     where it spent its time every 10 s ([PROF] lines), separately for frames that ran
rem     past 18 ms. Remove or set to 0 to turn it off.
set RECOMP_PROFILE=1
rem --- Smoothness (v68): sound keeps ~64 ms queued ahead (RECOMP_APU_LEAD=<buffers of
rem     21 ms>, default 3; 0 = old behaviour), and the window draws each frame when it is
rem     flipped instead of on its own 16 ms timer (RECOMP_FB_POLL=1 restores the timer).
rem set RECOMP_APU_LEAD=3
rem set RECOMP_FB_POLL=1
rem --- GPU vertex processing (v77): vertex programs and the fixed-function pipeline
rem     (transform, lighting, skinning, texgen, fog) run in the graphics card's vertex shader.
rem     RECOMP_GL_CPU_VERTEX=1 keeps them on the CPU as before.
rem set RECOMP_GL_CPU_VERTEX=1
rem --- v80: DXT textures go to the GPU compressed with their own mip levels
rem     (RECOMP_GL_DXT=0 decodes them on the CPU as before), and loud sound is held
rem     down by a limiter instead of a soft clip (RECOMP_APU_LIMITER=0 for the old one).
rem set RECOMP_GL_DXT=0
rem set RECOMP_APU_LIMITER=0
rem --- v81: interrupts wait for any thread at raised IRQL (one-CPU behaviour).
rem     RECOMP_IRQL_GATE=0 turns that off.
rem set RECOMP_IRQL_GATE=0
rem --- v82: voice volume changes glide instead of stepping (RECOMP_APU_VOL_RAMP=0 steps).
rem set RECOMP_APU_VOL_RAMP=0
rem --- v83: record the sound output to apu_capture_0.wav / apu_capture_1.wav (30 s each,
rem     taking turns), for finding the crackle. About 11 MB per file.
rem set RECOMP_APU_CAPTURE=1
rem --- v84 test: a quieter master level (default 0.5) leaves headroom for the Windows audio chain.
rem set RECOMP_APU_GAIN=0.3
rem --- v85: sound output. Unset = XAudio2 (as before); wasapi = WASAPI shared;
rem     wasapi_excl = WASAPI exclusive (other apps go silent while the game runs).
rem     RECOMP_APU_WASAPI_MS = device buffer in ms (default 20).
rem set RECOMP_APU_BACKEND=wasapi_excl
rem set RECOMP_APU_WASAPI_MS=40
rem --- v86: sound frames wait for a DirectSound command to finish instead of going out
rem     as silence. RECOMP_APU_TRAP_WAIT=0 restores the old gaps.
rem set RECOMP_APU_TRAP_WAIT=0
rem --- v87: internal resolution. 2 = 1280x960, 3 = 1920x1440, 4 = 2560x1920 (1 = native 640x480).
set RECOMP_GL_SCALE=2
rem --- v88: frames go to the window on the GPU. RECOMP_GL_VSYNC=1 waits for vertical blank
rem     (no tearing); RECOMP_GL_WINDOW_CPU=1 restores the old CPU copy.
rem set RECOMP_GL_VSYNC=1
rem set RECOMP_GL_WINDOW_CPU=1
rem --- GPU-side frames (v67): a texture made from a render target is copied on the GPU,
rem     and the finished frame goes straight to the window. RECOMP_GL_TEX_READBACK=1 and
rem     RECOMP_GL_PRESENT_GUEST=1 restore the old read-backs through guest memory.
rem set RECOMP_GL_TEX_READBACK=1
rem set RECOMP_GL_PRESENT_GUEST=1
rem --- Memory (v50): freed memory goes back to the guest heap. RECOMP_HEAP_RECLAIM=0
rem     turns that off (the "problem with the disc" after the tutorial was the heap running out).
rem set RECOMP_HEAP_RECLAIM=0
rem --- Frame cap (v43): the game advances once per frame, so it is held to 60 like the
rem     console. RECOMP_FPS_CAP=0 removes the cap.
rem set RECOMP_FPS_CAP=60
rem --- Rendering diagnostics (v32): a few sample batches of each transform path, and a
rem     full one-frame trace whenever debug\trace_frame.bat is double-clicked during play.
set RECOMP_FF_TRACE=4
set RECOMP_VSH_TRACE=12
set RECOMP_FRAME_TRACE=%~dp0trace.flag
if exist "%~dp0trace.flag" del "%~dp0trace.flag"
rem --- No watchdog here: it ends the run after N seconds whether or not the game is stuck.
rem     debug\run_jsrf_debug.bat sets it (and the hang sampler) for diagnosing a stall.
if not defined RECOMP_WATCHDOG_SECS set RECOMP_WATCHDOG_SECS=0

if not exist "build\Release\jsrf_recomp.exe" (
  echo build\Release\jsrf_recomp.exe not found - build first.
  pause
  exit /b 1
)
echo Running... output goes to stdout.txt and stderr.txt. Close the window or end the process when it stalls.
"build\Release\jsrf_recomp.exe" > stdout.txt 2> stderr.txt
set RC=%ERRORLEVEL%
echo Exit code %RC%
echo.>> stdout.txt
echo [run_jsrf] process exit code %RC%>> stdout.txt
rem -1073741571 = 0xC00000FD stack overflow, -1073740791 = 0xC0000409 fast-fail, -1073740940 = 0xC0000374 heap corruption
