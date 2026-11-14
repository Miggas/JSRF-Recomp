# Changelog

Every version of the Jet Set Radio Future recompilation, newest first. The
details of each, with the how and the why, are in `README_JSRF.md`.

- Lettered builds (v307b, v307c, ...) are folded into their main version.
- **Developer** lists log switches and diagnostic builds; players can skip it.
- Dates come from the git history, which starts at v244; older versions have
  none.
- Settings are `RECOMP_*` names, set in `jsrf.ini` or by the launcher.

---

## v308 — 2026-11-14

### Added
- `RECOMP_LETTERBOX=0` leaves out the black bars of cutscenes and
  conversations (launcher: Gameplay → Cutscene black bars). The game runs
  exactly as before, in 4:3 and widescreen.

## v307 — 2026-11-13

### Fixed
- Widescreen: the trick name ("Back Slide") and the "x" between the trick
  count's numbers move to the screen edge with the rest of the HUD.

### Developer
- `RECOMP_WS_TEXT_LOG=1` logs each string the game adds, where from and in
  which HUD group (`[WSTEXT]`).

## v306 — 2026-11-13

### Fixed
- Right-stick camera: let go while skating, the view stops where it was turned
  instead of turning on for a second; up / down eases back to the game's
  height.

## v305 — 2026-11-13

### Changed
- Widescreen VS race HUD: the position and the lap count go to the right edge,
  and FINAL LAP sits centred under the time instead of over "3/3 LAP" (the
  original game overlaps them too). `RECOMP_BANNER_DX` / `RECOMP_BANNER_DY`
  move the banner.

## v304 — 2026-11-13

### Fixed
- Widescreen VS mode: Professor K's text ran past his box to the right.

## v303 — 2026-11-12

### Fixed
- A rare damaged-disc screen at a mission's start (something overwrote the
  mission data): a copy is kept and the overwritten bytes put back.

### Developer
- `[WATCH]` names what writes over the mission data.

## v302 — 2026-11-12

### Fixed
- A black screen at boot on drives with 4096-byte sectors (files are no
  longer opened unbuffered).

## v301 — 2026-11-11

### Added
- Personal bests: a panel beside the mission select with the five best times
  of the picked mission and the character used, kept in `pbtimes.dat`.

### Changed
- In picked races the clear screen's own time is hidden; the mission timer
  shows big at the bottom middle of the clear screen, half a second after it
  stops.
- 0122 picked from the mission select skips its first start cutscene.

## v300 — 2026-11-11

### Added
- A timer for picked missions, top-left (00:00.00). It starts after any
  opening cutscene or countdown, stops for cutscenes, talks and the pause, and
  holds its time on the clear screen. A retry starts it from 0.

### Changed
- The game's own race timer is hidden in picked races.

## v299 — 2026-11-10

### Fixed
- 0750 picked from the mission select plays both its phases, cutscene
  included, and ends at the second phase's clear, before Professor K speaks.

## v298 — 2026-11-10

### Fixed
- Yoyo (and NT 3000) lost their voice for the rest of the stage after the
  chapter 7 garage: freeing YoyoB unloaded the voice bank they share. The
  game's own bug, in the NTSC releases (PAL gives YoyoB a bank of its own).

## v297 — 2026-11-10

### Fixed
- Widescreen: the capture-the-flag "FLAG" label stays under its arrow.

### Changed
- The "FLAG" label sits 14 guest pixels closer to its arrow, in 4:3 too
  (`RECOMP_FLAG_DY`).

## v296 — 2026-11-10

### Added
- Built-in start spots for 0451 (inside the drones' arena) and 0751.

## v295 — 2026-11-10

### Changed
- 0812 picked from the mission select skips its three start cutscenes.

## v294 — 2026-11-10

### Fixed
- 0812 never cleared after both Zero Beats.
- "Shibuya Terminal" in the mission list (it read "Termina").

## v293 — 2026-11-10

### Changed
- The overlay font's V, X and K redrawn (they looked like U and H);
  renamed "NiseJSRF Recomp edit", the author's credit kept.

## v292 — 2026-11-10

### Added
- After picking a mission, "Start with": No cans, 20 cans or Max cans.

### Fixed
- Quitting to the title during a race countdown left the player stuck.

## v291 — 2026-11-09

### Added
- A picked mission starts with 20 spray cans, again after a retry.

## v290 — 2026-11-09

### Changed
- The mission select's missions end with the cleared animation instead of
  their closing cutscenes (13 missions).

## v289 — 2026-11-09

### Fixed
- 0666, 0671 and 0746 went on to the rival's talk: a picked mission goes back
  to the title once cleared, even through an exit to itself.

## v288 — 2026-11-09

### Changed
- The mission select offers a list of 29 missions, under chapter
  headings, labelled "Location - Objective". Left / right go a chapter back or
  on.

## v287 — 2026-11-09

### Fixed
- PAL: the standings bar frozen on the cleared screen (a missing destructor
  and draw function).

## v285 — 2026-11-09

### Fixed
- Mission select: the chapter 5 races (0511, 0512) finish, and 0513 ends with
  the cleared screen and music instead of going on to 0514.

## v284 — 2026-11-09

### Added
- With `RECOMP_SHOW_MISSION=1` the window title shows the player's position
  and facing; F9 writes them to the log.
- `RECOMP_MISSION_SPAWN=CCMM:x,y,z,facing` puts a picked mission's player
  there (several missions separated by `;`).

## v283 — 2026-11-09

### Fixed
- Mission select: 0360 (Sewage Facility) starts after Garam joins, so the
  gates and switches are there.

## v282 — 2026-11-08

### Fixed
- Mission select: 0481 never ended.

## v281 — 2026-11-08

### Changed
- Mission select: a pick in chapters 1-9 opens the chapter's character select
  first, with every character unlocked.

## v280 — 2026-11-08

### Fixed
- Mission select starts a challenge where its area would (start point, chapter
  flags).

## v279 — 2026-11-08

### Added
- Mission select shows each mission's objective; a picked mission goes back to
  the title when it ends.
- `RECOMP_SHOW_MISSION=1`: the current mission and objective in the window
  title.

## v278 — 2026-11-08

### Added
- Mission select: LB + RB at the title opens a list of every mission on the
  disc, armed for the next NEW GAME.
- The quit window offers Quit Game, Quit to Title and Return.

## v277 — 2026-11-08

### Added
- MSAA by s-ilent (pull request #4): `RECOMP_GL_MSAA` = 2, 4 or 8. Off by
  default.

## v276 — 2026-11-08

### Added
- Chapter select from andeecollard/jsrf-recomp (MIT):
  `RECOMP_CHAPTER_SELECT=<chapter>[:<mission>]` (launcher: Settings → Debug).

### Changed
- PC sound mixing (the DSOUND lift) is on by default on every release
  (`RECOMP_DSOUND_LIFT=0` for the emulated APU).

### Fixed
- A freeze after opening and closing the quit window a few times.
- The A or B that closed the quit window reached the game, and the controller
  was sometimes ignored for a second after it.
- The quit window didn't hold the sounds with PC sound mixing.
- PC sound mixing played music and voices twice as loud.

## v275 — 2026-11-07

### Added
- PC sound mixing from andeecollard/jsrf-recomp (MIT): `RECOMP_DSOUND_LIFT=1`
  plays the game's DirectSound calls on the PC, for sounds dropped or cut off
  with several skaters.

## v274 — 2026-11-07

### Fixed
- Japanese text on the US and Asian Sega GT 2002 + JSRF discs:
  `RECOMP_LANGUAGE` sets the game's text language there too.

## v273 — 2026-11-07

### Added
- The US Sega GT 2002 + JSRF disc (`SegaJSRF.xbe`). The European and Asian
  versions of that disc work as the PAL and US-combo releases.

## v272 — 2026-11-07

### Added
- The NTSC-J (Japanese) release.

## v271 — 2026-11-07

### Added
- `RECOMP_LANGUAGE`: the language the Xbox reports (English by default). The
  PAL release has English, German, French, Spanish and Italian.

## v270 — 2026-11-07

### Added
- The PAL (European) release, matched function by function to the US one.
- The launcher asks to generate the code again when the game folder holds
  another `default.xbe`.

### Fixed
- PAL: the window never opening, a hang on the first loading screen after the
  tutorial, the Professor K scene showing only the background, and a black
  screen at cutscene changes.

## v267 — 2026-11-05

### Added
- Hold START (1.3 s) to skip a cutscene, the first time too (the idea
  from mstan's JSRFRecomp, no code taken). `RECOMP_HOLD_SKIP=0`
  turns it off.

## v266 — 2026-11-02

### Changed
- The quit window and the "Building Shaders" message use the NiseJSRF font.
- Right-stick camera: turning it standing still and then setting off, the
  game's camera follows the skater from the new angle.

## v265 — 2026-11-02

### Added
- Camera on the right stick, asked for by players (the method from JuMatt's
  JetSetRadioFuture-Unleashed, MIT). The L trigger puts it
  behind at once. `RECOMP_CAM_ORBIT=0` for the original camera;
  `RECOMP_CAM_INVERT_X`, `RECOMP_CAM_INVERT_Y`, `RECOMP_CAM_SENS`.

## v264 — withdrawn

A first try at the right-stick camera; it did nothing and was taken out.

## v263 — 2026-11-01

### Fixed
- Widescreen: the VS mode menu's panels cut at both sides.

## v262 — 2026-11-01

### Changed
- The mouse pointer hides over the game window after 2 s
  (`RECOMP_CURSOR_HIDE=0`).

## v261 — 2026-11-01

### Changed
- The game and the GPU emulation run overlapped by default: 99th Street on the
  Steam Deck from ~45 to 57-60 fps. `RECOMP_ACK_FAST=0` takes turns again.

## v260 — 2026-11-01

### Changed
- Faster GPU emulation: the profiler no longer times every method, and the
  method dispatcher remembers where each method goes.

## v259 — 2026-11-01

### Added
- `RECOMP_ACK_FAST=1` (experimental): the game and the GPU emulation
  overlapped, as the NV2A's command FIFO does.

## v258 — 2026-11-01

### Changed
- Linux: the GL thread spins before sleeping, and timing uses rdtsc (faster on
  the Steam Deck).

## v257 — 2026-11-01 — (diagnostic)

### Developer
- `RECOMP_PROFILE=1` in the Linux build, naming every function.

## v256 — 2026-11-01 — (diagnostic)

### Developer
- The stall sampler in the Linux build.

## v255 — 2026-11-01

### Fixed
- The Linux build failed to link on CachyOS and other Arch-based systems.

## v254 — 2026-10-31

### Fixed
- A freeze in the fight with Hayashi's Terror Drone (DirectSound waiting for
  room the emulated APU never gave).

## v253 — 2026-10-31

### Added
- Shaders built before the game starts: "Building Shaders (xx out of yyy
  built)" when any are missing from the shader cache.

### Changed
- The quit window has no sound.

## v252 — 2026-10-31

### Changed
- The quit window pauses the game and its sound.

## v251 — 2026-10-31

### Changed
- The quit window looks like the game's own Yes / No box.

## v250 — 2026-10-31

### Added
- Quit window: L3 + R3 (keyboard Shift + Ctrl). `RECOMP_QUIT_COMBO=0` turns it
  off.

## v249 — 2026-10-31

### Changed
- Built with Zig through the launcher only; the `.bat` scripts and the MSVC
  build rules are removed. The README is written around the launcher.

## v248 — 2026-10-31

### Fixed
- The Zig build for Windows loaded slower than MSVC's (byte-at-a-time
  `memcpy`), was a console program with no log, and had no function names in
  crash reports.

## v247 — 2026-10-31

### Fixed
- The Zig build for Windows crashed at boot.

## v246 — 2026-10-30

### Added
- The all-in-one launcher (Play, Settings, Build, Update), now on its own
  `launcher` branch and in Releases. The game builds with Zig on Windows and
  Linux.

## v245 — 2026-10-29

### Changed
- The texture write-watch on Linux too.

## v244 — 2026-10-29

### Added
- A native Linux build (the Steam Deck without Proton).

### Fixed
- Linux: very high CPU use and the title music breaking up (threads now really
  suspend), and no music in the garage.
- On the Deck's desktop the game starts in a window.

## v243

### Changed
- Faster in busy spots: short GPU waits spin, long ones sleep (Windows), and
  the GL thread stays awake longer under Proton (Deck: 38 to 47-49 fps in
  99th Street).

## v242

### Added
- A map ID alone in the area skip and add lists stands for the whole map.

### Changed
- A build clears the shader cache (keeping its recipes), so shaders are built
  again in the menus.

## v241

### Added
- Shaders built ahead on light frames from a list of what was built before
  (`shader_cache/recipes.bin`), so new areas stutter less. The idea from
  JuMatt's JetSetRadioFuture-Unleashed.
  `RECOMP_GL_PREBUILD=0` turns it off.

## v240

### Fixed
- After a while of play, graffiti select's graffiti came out squeezed and
  stretching floors came back: stale render targets are forgotten.

## v239

### Fixed
- Graffiti souls and secret tapes drawn black (from v237): the depth rule only
  applies to the map's own passes, the light pillars' pass included.

## v238 — Anti-aliasing

### Added
- Anti-aliasing of the finished frame: `RECOMP_GL_AA` = `fxaa`, `smaa_low`,
  `smaa_medium`, `smaa_high` or `smaa_ultra` (SMAA's reference implementation,
  MIT). Off by default.

## v237

### Fixed
- Skyscraper District's chain-link floor shows behind the light pillars:
  additive glows no longer write depth (the game's own bug, on the Xbox too).
  `RECOMP_GL_ADDITIVE_DEPTH=1` keeps the game's behaviour.

## v236

### Added
- `RECOMP_DRAW_AREAS_ADD`: areas always drawn, per map, and
  `RECOMP_AREA_PICKER=2` to pick them in game.

## v235

### Changed
- Widescreen: the start menu in battles laid out for the wide screen.

## v233

### Added
- Street objects (tanks, police, vehicles) are drawn further away:
  `RECOMP_OBJ_DRAW_DIST` (default 2).

## v232

### Changed
- Widescreen: the street sign naming where a path leads goes to the left with
  the HUD, clear of the trick counter.

## v231

### Fixed
- The start menu's map name cut at the 4:3 edge on ultrawide screens.

## v230

### Fixed
- The GG-notebook's empty graffiti box is stretched too.

## v229

### Fixed
- Graffiti select's create and edit screens shown wide, without black sides.

## v228

### Fixed
- Graffiti select's 3D scene no longer smeared at the sides.

## v227

### Fixed
- The garage's graffiti menu has its sides filled again, without the grey
  corner square.

## v226

### Fixed
- Graffiti select's graffiti full width again (large textures were squeezed).

## v225

### Changed
- Graffiti select shown wide as it is, without the side fill.

## v224

### Fixed
- Filled menu sides stop with the menu's last frame, without flashing.

## v223

### Changed
- No more shrinking to 4:3 and stretching back between menus: the menus stay
  wide with their sides filled from the 4:3 edge.
  `RECOMP_WS_MENU_HOLD43=1` brings the old hold back.

## v222

### Changed
- Widescreen: the test run's course screen.

## v221

### Changed
- Widescreen: the test run menu.

## v220

### Changed
- Widescreen: the garage's graffiti menu.

## v219

### Changed
- Widescreen: the music selection menu.

## v218

### Changed
- Widescreen: the practice menu.

## v217

### Changed
- The save/load menu and Roboy's menus fill the screen sides with their own
  colours instead of black bars.

## v216

### Changed
- Widescreen: Roboy's options menu.

## v215

### Changed
- Widescreen: Roboy's system menu.

## v214

### Changed
- Widescreen: Roboy's garage menu.

## v213

### Changed
- The garage's character select stays 4:3 but its side bars are its own grey.

## v212

### Changed
- Widescreen: the start menu's character select.

## v211

### Changed
- Widescreen: the save menu, shown like the title's load menu.

## v210

### Changed
- Widescreen: the GG-notebook (start menu), rows widened with their text.

## v209

### Changed
- Widescreen: the pause menu's map laid out for the wide screen. `RECOMP_WS_MENU_MAP=0` keeps it 4:3.

## v208 — (diagnostic)

### Developer
- `RECOMP_MENU_LOG=1`: F9 snapshots of how menus are drawn (`[SPRITE]`,
  `[DRAWSNAP]`, `[DRAWLOG]`).

## v207

### Fixed
- The graffiti menu's icons (the selected one, the third button's): menu
  screens get their own depth, so icons are never lost behind their buttons.
  `RECOMP_GL_MENU_DEPTH=0` turns it off.

### Developer
- `RECOMP_GL_DRAWLOG=1`: F9 logs the next draws in detail.

## v206

### Fixed
- The first boost of a run flashed the stretched 4:3 view (v205 gave black
  bars instead). `RECOMP_WS_BOOST_TARGET=0` for the old way.

## v205 — withdrawn

Withdrawn in v206.

## v204

### Fixed
- The stretching floor in Chuo Street, from other angles.

## v203

### Fixed
- The electric signs' buzz in Chuo Street heard everywhere: muted by default
  (`RECOMP_SND_MUTE`; `none` plays everything). From the notes of Gurten's
  jsrf-rustlang. `RECOMP_SND_NEAR` plays it at the nearest sign instead.

### Developer
- `RECOMP_SND_DIST=1` logs sound distances.

## v202

### Fixed
- Rebuilds recompiled three files with nothing changed.

## v201

### Added
- Free camera: `RECOMP_FREECAM=1`, F5 to fly, WASD and the mouse.
- `RECOMP_DRAW_AREAS=hops`: draws further ahead without the "copy in the sky"
  of `all`. The default, with a skip list for the maps that need one.

## v200

### Added
- `RECOMP_AREA_PICKER=1`: find areas to skip in game (F6/F7/F8/F10), with
  per-map skip lists (`RECOMP_DRAW_AREAS_SKIP`).
- `RECOMP_DRAW_AREAS=<distance>`.

### Fixed
- With `RECOMP_DRAW_AREAS=all`, the world backdrop no longer covers far map
  pieces.

### Developer
- `[VOICE3D]` lines for sounds heard everywhere; F9 `[DRAWSNAP]` snapshots.

## v199

### Changed
- Faster in widescreen: most 3D batches skip the full-width check. 99th Street
  holds 60 fps with `RECOMP_DRAW_AREAS=all`.

### Fixed
- `RECOMP_DRAW_AREAS=all` no longer draws a white rectangle across the garage
  road.

## v198

### Added
- `RECOMP_DRAW_AREAS=all` draws every area of a map, which ends the pop-in of
  buildings (the game's own visibility, on the console too). Off by default;
  costs frame rate in 99th Street.

### Developer
- `RECOMP_DRAW_WHO=1` logs new draw call chains; F9 writes a mark line.

## v197 — Depth flicker fix

### Fixed
- Distant textures and shadows flickering: reversed float depth, with the
  game's own depth kept for near things, menus and the HUD
  (`RECOMP_GL_WDEPTH_NEAR`, default 128). `RECOMP_GL_WDEPTH=0` for the old
  24-bit depth.

### Developer
- `RECOMP_WBUFFER=1`: the software rasteriser tests w instead of z.

## v196

### Fixed
- Music and dialogue crackle: vertical blanks were being lost, so the music
  decoder ran too rarely and fell behind.
- A crash on start-up with `RECOMP_AUDIO_DEBUG` set.
- Cutscene bars shown 4:3 in Rokkaku Dai Heights in widescreen.

### Developer
- The crash report gives the exe offset, registers and code bytes.
- `[VBL]` and `[KEV]` lines follow vertical blanks to the threads waiting on
  them.

## v195 — (diagnostic)

### Developer
- `RECOMP_AUDIO_DEBUG=1` logs every voice, music buffer, decoder run and audio
  frame, and captures the output.

## v194

### Fixed
- Widescreen: floors jumping to a stretched shape and back (the halfpipe
  area).

## v193 — withdrawn

Withdrawn in v194 (perspective for pass-through programs, a wrong lead).

## v192

### Fixed
- Float-to-integer conversions round like the CPU instead of truncating
  (values with a fraction of .5 or more were one too low).

## v191

### Fixed
- A line along the top and left of the screen (missing shadows there, the
  scene showing past the cutscene bars), in 4:3 and widescreen. From Piaka's
  widescreen notes. `RECOMP_GL_EDGE=0` turns it off.

## v190 — (diagnostic)

### Developer
- Each `[GL] vertex program N ready` line gives its compile time.

## v189

### Added
- DualSense and other non-Xbox controllers on Windows, with rumble
  (`RECOMP_SDL_PADS=0` turns it off).

### Changed
- Buttons go by position (bottom = A), so Nintendo-layout pads match
  (`RECOMP_PAD_LABELS=1` goes by the labels).

## v188

### Fixed
- Saves from a real Xbox or another emulator are no longer called damaged
  (`RECOMP_SAVE_STRICT=1` checks signatures).

## v187 — Widescreen

### Added
- Widescreen: `RECOMP_WIDESCREEN=1` (16:9) or `auto` (the monitor's shape),
  `RECOMP_ASPECT=w:h`. The 3D shows more at the sides and is culled for the
  wider view; 2D keeps its shape.
- From Piaka's patches: `RECOMP_SKIP_INTRO=1` skips the logos and the warning;
  the HUD sits at the screen edges (`RECOMP_WS_HUD=0` turns it off); screens
  built for 4:3 (garage, map, character select, VS setup, graffiti select) are
  shown at 4:3.
- Split screen in widescreen: each player's HUD in its own view.
- `RECOMP_GL_ANISO=2/4/8/16`: anisotropic filtering.

### Fixed
- Boost's blur and shadow layers cover the whole wide screen (the strip on the
  left while boosting found by a Discord user).
- The title animation's black bars rest outside the wide edges.
- The garage's textures scrambled after leaving graffiti select.

### Developer
- `RECOMP_WS_LOG=1`, `RECOMP_WS_PROBE=1`, `[MSSN]` mission data log.

## v186

### Added
- Controller rumble (`RECOMP_RUMBLE=0` off, 1..100 scales it).
- Multiplayer: `RECOMP_USB_PADS` in `jsrf.ini`; players are given out in the
  order pads press a button (`RECOMP_PAD_ASSIGN=fixed`).

### Fixed
- Every pad showed up as controller 3.
- Split screen: the window clip narrows drawing.
- A missing function added to the build.

## v185

### Fixed
- The future-site scene (Rokkaku Expo stadium): the experimental machine in
  the right place and Professor K's text staying English. A function the game
  only reaches through data is now in the build.
- Stutter from logging every file read.

## v184 — (diagnostic)

### Developer
- `[READ]` lines hash every byte read; `[NVRAM]` logs language requests.
- A stalled game samples itself (`[STALL]`); `debug\compare_cache.bat`
  compares cached files with the disc.

## v183

### Changed
- The vertical-blank wait sleeps again, so the music thread keeps time on the
  Steam Deck; the game's kick-off wait still spins (`RECOMP_SLEEP_WAITS`:
  `vblank`, `all`, `none`).

## v182

### Changed
- Back to v175 plus v179's cheaper GL queue; v176-v181 withdrawn.

## v176–v181 — withdrawn

Withdrawn in v182: a `[CPU]` report, a music timing nudge, sleeping waits and
a sleeping executor.

## v175 — (diagnostic)

### Developer
- The music watch checks whether each write into the music buffer joins the
  previous one cleanly.

## v174 — (diagnostic)

### Developer
- `[BGM] voice` lines: where the music decoder writes relative to the play
  position.

## v173

### Changed
- `RECOMP_GL_THREAD_LAG` defaults to 0 (no extra frame of delay).

### Developer
- `RECOMP_AUDIO_REPORT=1` keeps the music reports with `RECOMP_QUIET`.

## v172

### Changed
- All GL calls are made on a thread of their own, so the GPU emulation and
  the driver work in parallel. `RECOMP_GL_THREAD=0` for the old way.

### Developer
- `[PERF] GL calls:` shows how long finished frames wait to be shown.

## v171

### Changed
- Fewer GL calls: a vertex array object per vertex layout
  (`RECOMP_GL_VAO_CACHE=0` for one).

## v170

### Changed
- Fewer GL calls a draw (constant inputs, texture units, vertex streams).

## v169

### Changed
- Cheaper timing reads and fewer GL calls a draw: 58-59 fps in the Deck's busy
  street.

### Developer
- `[PERF] GL calls:` counts GL calls per draw.

## v168 — (diagnostic)

### Developer
- The profiler samples the GPU executor thread too.

## v167

### Changed
- Fewer GL calls and less texture hashing a draw.

## v153–v166 — withdrawn

Withdrawn in v152r: letting the game run ahead of the GPU emulation
(`RECOMP_ACK_FAST`). It played with correct visuals but broken audio.

## v152

### Changed
- 55-57 fps in busy areas on the Steam Deck: render state and uniforms are
  only sent when they change.

## v151

### Fixed
- About 2 s in every 10 lost in busy areas under Proton: settings are read
  once instead of on every draw.

## v150

### Changed
- Idle vertex threads sleep until woken.

## v149

### Changed
- More batches copied as stored (those with a constant colour).

## v148

### Fixed
- A crash on a vertex helper thread (a race in the helper pool).

## v147

### Changed
- Vertices copied to the GPU as the game stored them, the GPU converting the
  formats (`RECOMP_VX_NATIVE=0` for the old path).

### Fixed
- Under Proton the window refused the GL format at startup and every frame went
  through the CPU; it now retries.

## v146

### Changed
- Runs of index words go to the executor in one call (`RECOMP_PB_BULK=0`).

### Developer
- The log starts with the build version and date.

## v145

### Changed
- The direct ring write is off again by default; the faster vertex fetch
  stays.

### Developer
- The profiler times every GPU method.

## v144

### Changed
- Faster vertex fill.

## v143

### Changed
- Without the write-watch, large textures are checked with a sparse hash, in
  full every 16 frames.

## v142

### Changed
- Texture hashes about twice as fast (CRC32C).

## v141

### Fixed
- Crashes on the Steam Deck: the texture write-watch is off under Wine/Proton
  (`RECOMP_GL_TEX_WATCH=1` forces it).

## v140

### Changed
- Saves go in a `savegame` folder inside the game folder; old saves are copied
  there on the first run.

## v139

### Added
- Full screen: `RECOMP_FULLSCREEN=1`, F11 or Alt+Enter.
- Portable build: `build_portable.bat` puts the exe next to the game, with a
  `jsrf.ini` for its settings. `RECOMP_SAVE_DIR` moves the saves.

## v138

### Changed
- Back to v130: v131-v137 withdrawn.

## v131–v137 — withdrawn

Withdrawn in v138: a first widescreen (the skater vanished behind walls),
`RECOMP_RESOLUTION`, full screen, the portable layout and `jsrf.ini`. Full
screen and the portable layout came back in v139, widescreen in v187.

## v130

### Fixed
- The river drawn as a flat haze: fog is applied per pixel, as on the Xbox.
  `RECOMP_GL_FOG_VERTEX=1` for per-vertex fog.
- A crash in DirectSound's voice list (`RECOMP_DSOUND_GUARD=0` turns the guard
  off).

## v129

### Fixed
- Water at low camera angles drawn as one flat colour: bump-mapped stages take
  their mip level from their own coordinates.

## v128

### Fixed
- Reverb rings as long as on the console (`RECOMP_APU_REVERB_DECAY`).

## v127

### Fixed
- Brake sparks and smoke, with correct geometry and shading: one reciprocal
  square root uses the Pentium III's value, the rest stay exact.

## v126

### Fixed
- v125 broke geometry and shading on AMD CPUs: the Pentium III results are
  reproduced bit for bit on any CPU. `RECOMP_SSE=p3` / `host` / `exact`.

## v125

### Fixed
- Brake sparks: approximate reciprocals use the host CPU's instructions, as on
  the console.

## v124 — (diagnostic)

### Developer
- `RECOMP_TEX_WHO` logs the code that draws a chosen texture.

## v123

### Fixed
- Effects the game hides behind a visibility test (glows, brake sparks and
  smoke) are drawn: visibility tests count pixels on the GPU.
  `RECOMP_GL_VISTEST=0` turns it off.

## v122 — (diagnostic)

### Developer
- `RECOMP_GL_EFFECTLOG=1` logs additive draws and the left stick.

## v121 — (diagnostic)

### Developer
- Traced frames count the pixels each draw writes, and read the depth under
  them.

## v120

### Fixed
- Smoke puffs: a disabled texture stage reads white, as on the Xbox.

## v119 — (diagnostic)

### Developer
- Traced frames save each bound texture as a BMP.

## v118

### Fixed
- Wrong textures after loading a new area (the write-watch missed reused
  memory).
- A crash after loading a game (an audio interrupt delivered twice).
- A guard for the game's flag-list checks when a list runs outside memory
  (answered element by element, as the original does).

### Developer
- One clean texture is re-hashed each frame as a check
  (`RECOMP_GL_TEX_VERIFY=0`).

## v117

### Changed
- Much less CPU time in texture checks: written pages are caught by a
  write-watch instead of hashing (`RECOMP_GL_TEX_WATCH=0`).
- Vertex data in persistently mapped buffers; only changed vertex-program
  constants are sent.

## v116

### Changed
- The audio investigation logs are off unless `RECOMP_APU_GP_LOG=1`.

## v115 — Reverb

### Added
- Reverb and echo in areas that use them (sewers, tunnels): a native I3DL2
  reverb fed by the game's own listener settings. `RECOMP_APU_REVERB=0` turns
  it off, `RECOMP_APU_REVERB_GAIN` sets its level.

## v114 — (diagnostic)

### Developer
- `[APU-RV]` finds and prints DirectSound's reverb state in memory, and logs
  its changes.

## v113 — (diagnostic)

### Developer
- Each audio DSP command logs a copy of its scratch memory and the words that
  changed (`RECOMP_APU_GP_DUMP=1` saves them).

## v112 — (diagnostic)

### Developer
- The once-a-second audio DSP comparison also covers the DSP's own memory.

## v111 — (diagnostic)

### Developer
- Logs every command sent to the audio DSP and the scratch memory it changes.

## v110 — (diagnostic)

### Developer
- `[APU-GP]` and `[APU-BINS]`: what the game writes to the audio DSPs, and the
  level of each mixbin, every 10 s.

## v109

### Changed
- DXGI presentation is opt-in again (`RECOMP_GL_PRESENT=dxgi`); SwapBuffers is
  the default.

## v108

### Added
- Frames can be shown through a DXGI flip-model swap chain, the way Windows
  expects a game to present.
- The game's main thread registers with Windows as a "Games" task (MMCSS).
  `RECOMP_MMCSS=0` turns it off.

## v107

### Fixed
- The shader table never throws out a live shader while it has free slots.

## v106

### Fixed
- Areas with many effects no longer rebuild shaders over and over: the shader
  table is much larger.

### Developer
- The `GPU side:` line counts shaders in use, new and evicted.

## v105

### Changed
- Faster GPU rendering: each combiner setup gets its own specialised fragment
  shader, kept in the shader cache. `RECOMP_GL_RC_SPECIAL=0` uses the general
  one.

### Developer
- Frame traces print each bump-mapped stage's settings.

## v104

### Fixed
- Water draws: bump-environment-mapped texture stages are implemented.

### Developer
- `RECOMP_GL_SIMPLE_FS=1` measures what the combiners cost the GPU.

## v103

### Changed
- Faster 3D: only 2D drawing writes the game's own depth; 3D lets the GPU test
  depth before shading. The near/far cull became clip distances.

## v102

### Fixed
- v101's GPU timing slowed the game; it now uses two timestamp queries a frame
  and never waits for them.

## v101

### Developer
- `[PERF] GPU side:` shows when the GPU really reaches each frame.

## v100

### Fixed
- Glitches where something drew in front of the sewer floor and shadows: the
  game's depth is only written where it agrees with the real depth.

## v99 — (diagnostic)

### Developer
- Frame-trace probes give the depth value too; `RECOMP_FTRACE_VERTS` lists
  chosen draws' vertices.

## v98 — (diagnostic)

### Developer
- Frame-trace probes at chosen points (`RECOMP_FTRACE_PROBE`), on the GPU
  vertex path too.

## v97

### Fixed
- The leftover lines of the police spotlight: D3D's pass-through program is
  drawn flat, as screen positions.

## v96

### Fixed
- The yellow band across the screen from the police spotlight beam: pixels
  outside the depth range are dropped, as on the Xbox.

## v95

### Changed
- v94's position path is off by default again (`RECOMP_GL_XEMU_CLIP=1`).

### Developer
- Frame traces record vertex programs and inputs in full, to replay draws.

## v94

### Changed
- Positions go to the GPU the way xemu hands them over, with
  perspective-correct texturing. `RECOMP_GL_OLD_CLIP=1` for the old way.

## v93 — (diagnostic)

### Developer
- Frame traces show the first vertex's inputs as the game supplied them.

## v92 — (diagnostic)

### Developer
- Frame traces show colours across the top of the screen after every draw, to
  find the draw that paints the band.

## v91 — (diagnostic)

### Developer
- Frame traces print the first vertices of each fixed-function draw.

## v90

### Fixed
- Frame pacing: frames reach the window 16.7 ms apart.

## v89 — (diagnostic)

### Developer
- `[PERF] window:` line: frames shown, how far the GPU is behind, spacing.

## v88

### Changed
- Frames are drawn into the game window on the GPU instead of copied through
  the CPU. `RECOMP_GL_VSYNC=1` for vsync.

## v87

### Added
- Internal resolution scale, `RECOMP_GL_SCALE=N` (1-8). The window opens at
  that size, can be resized, and keeps 4:3.

## v86

### Fixed
- Sound crackle: the audio thread waits out a busy moment instead of
  outputting a silent frame.

## v85

### Added
- WASAPI sound output (`RECOMP_APU_BACKEND=wasapi` or `wasapi_excl`).

## v84 — (diagnostic)

### Developer
- The audio report counts XAudio2's own glitches and its real queue depth.

## v83 — (diagnostic)

### Developer
- `RECOMP_APU_CAPTURE=1` records the sound output to WAV files.

## v82

### Fixed
- Crackle in busy streets: voice volumes glide between frames instead of
  stepping. New voices fade in.

## v81

### Fixed
- Crash after clicking Load on the title screen (rare sound crashes): an
  interrupt can no longer run in the middle of code
  that had raised the IRQL.

## v80

### Changed
- DXT textures go to the GPU compressed, with the game's own mip levels: four
  to eight times less data. `RECOMP_GL_DXT=0` for the old CPU decode.
- Sound: a limiter instead of a soft knee, so loud moments keep their shape.
  `RECOMP_APU_LIMITER=0` for the old way.
## v79

### Added
- Shader cache: built shaders are saved to `shader_cache` and loaded on later
  runs, so each one compiles once ever. `RECOMP_GL_NO_SHADER_CACHE=1` turns it
  off.

### Developer
- `[PERF] frames:` breaks the longest frame down (shader compiles, texture
  uploads, GPU waits).
## v78

### Changed
- Each vertex program becomes its own GPU shader, built once.

## v77

### Changed
- Vertices are processed on the GPU (vertex programs and the fixed-function
  pipeline).

## v76

### Changed
- The separate flush acknowledgement is off again (`RECOMP_ACK_FAST=1`).

## v75

### Fixed
- Crash on the loading screen before the title: the executor no longer reads
  outside memory.

## v74

### Changed
- The game builds the next frame while the GPU draws this one.

## v73 — (diagnostic)

### Developer
- `RECOMP_PROFILE=1` samples the main game thread and prints its busiest
  functions.

## v72

### Fixed
- Title music crackle and slowdowns: the vertical blank keeps an exact 60 Hz.

## v71 — (diagnostic)

### Developer
- `[BGM] music buffer` reports the music decoder's buffer.

## v70

### Fixed
- Crackle and drag in looping sounds: the resampler no longer reads ahead into
  old audio, and a looping buffer no longer drops a sample (a click) each time
  it wraps.
## v69 — (diagnostic)

### Developer
- `[VOICE]` lines describe every voice that played.

## v68

### Fixed
- Sound gaps: the output queue starts with a lead of silence (`RECOMP_APU_LEAD`).
- A frame shown twice and the next one never: the window draws each frame when
  it is flipped.

### Developer
- `[PERF] frames:` reports the longest gaps between flips.
## v67

### Changed
- Faster: textures made from render targets are copied on the GPU, and the
  finished frame goes to the window without a trip through game memory.
## v66

### Fixed
- Two functions the game reaches only through data are now in the build.

## v65

### Fixed
- Batches with a vertex behind the camera use the GPU's own depth.

## v64

### Fixed
- Depth as the Xbox computes it, written by the fragment shader.
  `RECOMP_GL_FRAGDEPTH=0` restores GL's depth.

## v63

### Fixed
- Clears respect the clear rectangle.

### Changed
- Big batches split across the vertex threads.
- `play_jsrf.bat` runs without diagnostics (`RECOMP_QUIET=1`).
## v62

### Changed
- About 10% faster: vertex threads spin between batches, and presenting no
  longer waits for the frame's read-back (one frame of latency;
  `RECOMP_GL_SYNC_PRESENT=1` for the old way).
- Sound master level 0.5.
## v61

### Added
- Fixed-function lighting: menus and characters lit correctly.

## v60

### Changed
- Sound mixdown by speaker role; master level 0.2.

## v59

### Fixed
- Constant colours set without a vertex array are used (D3D's diffuse
  colour); they used to read white.

### Changed
- Frame rate about doubled in the slow areas: per-word counters in the
  pushbuffer survey are constant-time, and small textures are re-checked less.
## v58

### Fixed
- A crash after file reads (no stack fix-up after a file-I/O APC).
- Ending voices fade out instead of clicking; master level 0.4.
## v57

### Fixed
- Sound effects at the right speed and pitch: each voice is resampled to its
  own rate.
## v56

### Added
- Sound effects are heard: file reads complete the way the Xbox delivers them,
  and voices are mixed into full XAudio2 buffers.
## v55 — (diagnostic)

### Developer
- `[CRI] ADX server` counters.

## v54

### Changed
- Surfaces are only written back to memory when something reads them.

### Developer
- `[CRI]` reports while the loading screen is stuck.

## v53

### Fixed
- Memory protection calls are recorded but not applied.

## v52

### Fixed
- Re-used memory is made writable first.

## v51

### Fixed
- Crash right after the new-game fade.

### Changed
- Faster texture decoding.

## v50

### Changed
- Freed memory is reused (`RECOMP_HEAP_RECLAIM=0` turns it off).
- Faster GPU path: vertices transformed on several threads
  (`RECOMP_VERTEX_THREADS`), GL state only re-sent when it changes.
  `RECOMP_GL_LEGACY_SUBMIT=1` for the old path.
## v49

### Fixed
- The braking smoke and freezes in play (D3D's notify hook).

## v48

### Fixed
- Pre-transformed detection no longer catches immediate-mode batches.

### Developer
- `sample_hang.bat` samples a frozen game thread.

## v47

### Fixed
- Meshes spiking to infinity and the game freezing: the pushbuffer is read
  along the stream, following jumps.

## v46 — (diagnostic)

### Developer
- `[STRETCH]` logs stretched triangles; bisection switches.

## v45

### Added
- Texture coordinate generation (reflections, sphere maps).

## v44

### Fixed
- Flickering decals: textures re-checked on every bind; polygon offset.

## v43

### Added
- 60 fps frame cap (`RECOMP_FPS_CAP`), since the game logic runs once a frame.

## v42

### Fixed
- Pre-transformed detection back on with v41's fix.

## v41

### Fixed
- Single missing triangles and flicker.

## v40

### Fixed
- Pre-transformed (screen-space) batches draw correctly.

## v39

### Added
- GPU renderer (OpenGL 3.3): 25-40 fps instead of ~4.
  `RECOMP_RENDERER=sw` keeps the software rasteriser.

## v38

### Changed
- Faster software rasteriser.

## v37

### Added
- Stencil buffer: shadows and proper sunlight.

## v36

### Changed
- Faster: a vertex cache per batch.

## v35

### Changed
- Triangles rasterised in parallel across threads: ~4 fps.

## v34 — (diagnostic)

### Developer
- `[PERF]` line: time in the rasteriser.

## v33

### Fixed
- Characters render correctly: face culling.

## v32

### Fixed
- Streaks across the screen: triangles clipped against a guard band.

## v31

### Added
- Fixed-function transform: the stage renders (city, overpasses, traffic,
  HUD).

## v1–v30 — Bring-up

### Added
- The game recompiled to native code, from its own disc.
- From the first runs to PRESS START and in game: the game boots, shows the
  SEGA, Smilebit and Dolby logos and the warning screen, loads its files,
  reaches the title screen, and starts a new game with keyboard and pad.
