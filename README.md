<div align="center">

# Jet Set Radio Future — PC recompilation

**The original Xbox game, translated into a native Windows and Linux program.**<br>
Runs on Windows 10/11, Linux and the Steam Deck.

![Windows](https://img.shields.io/badge/Windows-10%20%7C%2011-0078D6?logo=windows&logoColor=white)
![Linux](https://img.shields.io/badge/Linux-native-FCC624?logo=linux&logoColor=black)
![Steam Deck](https://img.shields.io/badge/Steam%20Deck-native-1A9FFF?logo=steamdeck&logoColor=white)
![Version](https://img.shields.io/badge/game-every%20release%3A%20US%20%7C%20EU%20%7C%20JP%20%7C%20Sega%20GT%202002%20combo%20discs-orange)
![Bring your own game](https://img.shields.io/badge/game%20files-not%20included-lightgrey)

</div>

This is **not an emulator**. The game's own program is translated, once, into C
code on your computer, and compiled into a normal program. The Xbox's hardware
(graphics, sound, controllers, memory card) is recreated by the runtime in
`xboxrecomp/`.

> [!IMPORTANT]
> **You need your own copy of the game.** Nothing from the game is in this
> repository. The translation is made on your computer, from your copy.

> [!NOTE]
> **Made with AI.** Most of the code was written by Claude (Anthropic's AI),
> directed, tested and played through by Miggas. See
> [how this was made](#how-this-was-made).

## Contents

1. [What you need](#1-what-you-need)
2. [Install and play](#2-install-and-play)
3. [Steam Deck](#3-steam-deck)
4. [The launcher](#4-the-launcher)
5. [Controls](#5-controls)
6. [Settings (jsrf.ini)](#6-settings-jsrfini)
7. [Saves](#7-saves)
8. [Troubleshooting](#8-troubleshooting)
9. [What's in the folders](#9-whats-in-the-folders)
10. [Credits and licences](#10-credits-and-licences)

Debug and advanced settings: [DEBUG.md](DEBUG.md).

---

## 1. What you need

- **A Windows 10/11 PC, a Linux PC or a Steam Deck.** The game is built on the
  system it runs on.
- **Your copy of Jet Set Radio Future for the original Xbox**, any retail
  release, as its files or as a disc image (`.iso`).
- **About 3 GB of free disk space.**
- **An internet connection** the first time, for the launcher's tools.
- **A graphics card with OpenGL 3.3** or newer.

Nothing else has to be installed: the launcher brings its own tools.

Every retail release works:

| Release | Disc | Game file | Languages |
|---|---|---|---|
| North America (NTSC) | standalone | `default.xbe` | English, Japanese |
| Europe (PAL) | standalone | `default.xbe` | English, French, German, Spanish, Italian |
| Japan (NTSC-J) | standalone | `default.xbe` | Japanese, English |
| North America | Sega GT 2002 + JSRF | `SegaJSRF.xbe` | English, Japanese |
| Europe | Sega GT 2002 + JSRF | `SegaJSRF.xbe` | English, French, German, Spanish, Italian |
| Asia | Sega GT 2002 + JSRF | `SegaJSRF.xbe` | English, Japanese |

On a combo disc, `default.xbe` is only the menu that picks the game; the
launcher and the game use `SegaJSRF.xbe`. Dumps with a patched header (region,
allowed media) work too: the build checks the game's code, not the whole file.

---

## 2. Install and play

1. **Get the launcher** from [Releases](https://github.com/Miggas/JSRF-Recomp/releases)
   (`jsrf_launcher-windows.zip` or `jsrf_launcher-linux.tar.gz`) and unpack it
   into an **empty folder** of your choosing. Avoid synced folders (OneDrive, Dropbox).
   On Linux, mark `jsrf_launcher` as executable (right-click →
   **Properties → Permissions**).
2. **Copy the game's folder** into the launcher's folder and rename it to `jsrf`.
   Files somewhere else? **Play → Choose folder**.
3. **Play → Build.** The first time it downloads the tools and the project,
   translates the game and compiles it: 10–20 minutes, with a progress bar.
4. **PLAY.** You can also start `jsrf_recomp` (`jsrf_recomp.exe` on Windows) in
   the game folder directly.

**F11** or **Alt+Enter** switches between window and full screen.

<details>
<summary>💿 I only have a disc image (.iso)</summary>

First do **Update → Download** in the launcher. Then, in a terminal in the
launcher's folder:

```
cd source\xboxrecomp
..\..\tools\python\python.exe -m tools.xiso unpack "C:\path\to\game.iso" -o ..\..\jsrf
```

On Linux: `cd source/xboxrecomp`, then
`../../tools/python/bin/python3 -m tools.xiso unpack /path/to/game.iso -o ../../jsrf`
</details>

**Updating:** **Update → Update**, then **Play → Build**. Your game files,
`jsrf.ini` and saves are kept.

---

## 3. Steam Deck

The Linux version runs natively on the Deck.

1. In **Desktop Mode**, follow [section 2](#2-install-and-play) with the Linux
   launcher.
2. In Steam: **Games → Add a Non-Steam Game to My Library… → Browse…**, set the
   file type to *All Files* and pick `jsrf_launcher` (or `jsrf_recomp` in the
   game folder, to start the game directly). Don't force a Proton version.
3. Back in **Gaming Mode**, play. The launcher works with the controller and
   the touchscreen. To quit the game, click both sticks.

The provided `jsrf.ini` already suits the Deck (`RECOMP_GL_SCALE=2`).

---

## 4. The launcher

| Tab | What it does |
|---|---|
| **Play** | The game folder, what is ready (✓ / ✗, click one to fix it), **Build** and **PLAY**. |
| **Settings** | `jsrf.ini` in tabs: Gameplay, Video, Input, Audio, Debug. |
| **Build** | **Generate code** and **Build** separately; each skips what is up to date. |
| **Update** | Gets the newest version of the project and of the launcher. |
| **Launcher** | Close the launcher when the game starts, the project branch (`main` / `test`), and Debug (the Debug tab and **Settings → All settings**). |

These are kept in `launcher.ini`. Everything the launcher does is written to
`launcher_log.txt`. Its code is on the
[`launcher`](https://github.com/Miggas/JSRF-Recomp/tree/launcher) branch.

---

## 5. Controls

Any controller (Xbox, PlayStation, the Steam Deck's own) works as an Xbox
controller. The keyboard works as controller 1:

| Keyboard | Xbox controller |
|---|---|
| Arrow keys | D-pad |
| Numpad `8` `2` `4` `6` | Left stick |
| `I` `K` `J` `L` | Right stick |
| `Z` `X` `A` `S` | A / B / X / Y |
| `Q` `E` | White / Black |
| `1` `3` | Left / Right trigger |
| `Enter` | START |
| `Backspace` | BACK |
| `Shift` / `Ctrl` | Click the left / right stick (L3 / R3) |

- **Quit:** click both sticks (`Shift` + `Ctrl`): **Quit Game**, **Quit to Title** (while in a level) or **Return**, then **A**.
  Closing the window or `Alt+F4` works too.
- **Skip a cutscene:** hold **START** (`Enter`) for about 2 seconds.
- **Mission select:** on the title screen, press **LB + RB** together (White + Black; `Q` + `E` on the keyboard) for a list of every mission, then choose **NEW GAME**: pick your character (all are unlocked) and the mission starts. Each mission shows its objective. Refight a boss or redo a race without touching your save (just don't save afterwards): when the mission ends, the game goes back to the title. *Idea by Miggas.*

---

## 6. Settings (jsrf.ini)

Settings live in `jsrf.ini`, in the game folder. The easiest way to change
them is the launcher's **Settings** tab. In a text editor: one setting per
line, `NAME=value`; a line starting with `;` is off. Steam launch options
override the file.

> [!CAUTION]
> **Do not change** the first lines of the `[game]` section. The game needs
> them to start.

**`[game]`**

| Setting | What it does |
|---|---|
| `RECOMP_QUIET=1` | Fewer log messages (faster). |
| `RECOMP_SKIP_INTRO=1` | Skip the intro logos. |
| `RECOMP_HOLD_SKIP=0` | Turns off skipping cutscenes by holding START. |
| `RECOMP_LANGUAGE=french` | The game's language: `english` (default), `french`, `german`, `spanish`, `italian`, `japanese`. Which ones each release has: see [section 1](#1-what-you-need). |
| `RECOMP_SAVE_STRICT=1` | Refuse saves from other sources (a real Xbox, emulators, save editors), as the Xbox does. |

**`[display]`**

| Setting | What it does |
|---|---|
| `RECOMP_GL_SCALE=2` | Internal resolution: `1` = 640×480, `2` = 1280×960, `3` = 1920×1440, `4` = 2560×1920, `5` = 3200×2400, `6` = 3840×2880 (4K) (up to `8`, if the graphics card allows). |
| `RECOMP_GL_ANISO=16` | Anisotropic filtering: `2`, `4`, `8` or `16`. |
| `RECOMP_GL_AA=smaa_high` | Anti-aliasing: `fxaa`, `smaa_low`, `smaa_medium`, `smaa_high` or `smaa_ultra`. |
| `RECOMP_GL_MSAA=4` | MSAA (multisample anti-aliasing): `2`, `4` or `8`. Smooths 3D edges at a lower cost than a higher `RECOMP_GL_SCALE`, good for 4K; cut-out textures (fences, leaves) aren't smoothed. *By s-ilent.* |
| `RECOMP_FULLSCREEN=1` | Start in full screen. |
| `RECOMP_GL_PRESENT=dxgi` | Borderless window (Windows only). May lower performance. |
| `RECOMP_WIDESCREEN=auto` | Widescreen: `1` = 16:9, `auto` = your monitor's shape, or `RECOMP_ASPECT=21:9` for others. |
| `RECOMP_GL_VSYNC=1` | VSync. |
| `RECOMP_GL_PREBUILD=0` | Don't build the shaders at start; build them during play instead. |
| `RECOMP_CURSOR_HIDE=0` | Keep the mouse pointer visible over the game. |

**`[input]`**

| Setting | What it does |
|---|---|
| `RECOMP_USB_PADS=1` | Number of players (1 to 4). |
| `RECOMP_RUMBLE=100` | Rumble strength in percent (`0` = off). |
| `RECOMP_CAM_ORBIT=0` | The original camera. By default the right stick turns the camera around the skater, and it swings back behind you; the L trigger puts it behind at once. |
| `RECOMP_CAM_INVERT_X=1` / `RECOMP_CAM_INVERT_Y=1` | Invert the camera's left / right or up / down. |
| `RECOMP_CAM_SENS=1.0` | Camera speed. |
| `RECOMP_QUIT_COMBO=0` | Turn the quit window (both sticks) off. |

**`[audio]`**

| Setting | What it does |
|---|---|
| `RECOMP_APU_GAIN=0.5` | Master volume. |
| `RECOMP_DSOUND_LIFT=0` | Turns PC sound mixing off (on by default: the game's sounds are mixed on the PC and played through its audio directly) and uses the emulated Xbox sound chip instead. *From andeecollard's jsrf-recomp.* |

**`[files]`**

| Setting | What it does |
|---|---|
| `RECOMP_LOG=stderr.txt` | Write a log, for bug reports. |
| `RECOMP_PROFILE=1` | Performance profiling in the log. Slows the game down. |

**`[draw distance]`**: leave these alone unless you know what you're doing.

| Setting | What it does |
|---|---|
| `RECOMP_DRAW_AREAS=…` | Pop-in fix. `hops` (default) fixes almost all of it; `hops3`, `hops4`… reach further; `game` = as on the Xbox; `all` = draw everything. |
| `RECOMP_DRAW_AREAS_SKIP=EE7A:96,105` | Map pieces never to draw (`MapID:area,area;MapID:area`; a map ID alone = no fix on that map). |
| `RECOMP_DRAW_AREAS_ADD=` | Map pieces always to draw. Same format. |
| `RECOMP_AREA_PICKER=1` | Finds the numbers for the two lists above (`1` = skip, `2` = add). F6/F7 select an area, F8 toggles it, F10 clears all; the list goes to the log. |
| `RECOMP_OBJ_DRAW_DIST=2` | Draw distance of street objects (tanks, police, vehicles). `1` = original. |

**`[tools]`**

| Setting | What it does |
|---|---|
| `RECOMP_FREECAM=1` | Free camera. F5 = on/off, WASD = move, Shift / Ctrl = up / down, mouse = look, wheel = speed. |
| `RECOMP_FREECAM_SPEED=2` | The free camera's start speed. |
| `RECOMP_FREECAM_SENS=0.003` | The free camera's mouse sensitivity. |
| `RECOMP_CHAPTER_SELECT=2` | Chapter select: a new game starts at chapter 2's opening; `2:0` its Garage (chapter 5: `5:10`). Skips the chapter's earlier events, and the game's save points save over your progress: back up `UDATA` first. Also in the launcher's **Settings → Debug**. *From andeecollard's jsrf-recomp.* |
| `RECOMP_SHOW_MISSION=1` | The window title shows the current mission (`mssnCCMM`), your position and the objective. F9 writes your position to the log. |
| `RECOMP_MISSION_SPAWN=CCMM:x,y,z,facing` | A mission picked in mission select starts at that spot, facing that way (from F9: it logs the way you were skating). |

**`[compatibility]`**: try these if something goes wrong.

| Setting | What it does |
|---|---|
| `RECOMP_ACK_FAST=0` | Stop running the game a frame ahead of the graphics. Try it if sound, visuals or input go wrong. |
| `RECOMP_GL_TEX_WATCH=0` | Turn the texture write-watch off. Try it if the game stutters. |
| `RECOMP_SLEEP_WAITS=…` | How the game waits for the graphics: `all` (default), `vblank`, or `none` (highest CPU use). |

More settings, for testing and finding problems: [DEBUG.md](DEBUG.md).

---

## 7. Saves

Your save is in the **`UDATA`** folder of the game folder, and the game's own
settings in **`TDATA`**. Both are tiny: copy them to back up your progress or
move it to another PC or the Deck.

The `savegame` folder, despite its name, is only the Xbox hard-disk cache. The
game rebuilds it by itself, so you can delete it (with the game closed) and
leave it out when copying the game somewhere.

---

## 8. Troubleshooting

| You see | What to do |
|---|---|
| `default.xbe not found` | The game folder must hold `default.xbe` directly, not in a subfolder. **Play → Choose folder**. |
| `… not a release the project knows` | Your copy isn't a known release ([section 1](#1-what-you-need)); it may be modified or damaged. |
| A download fails | Check the internet connection and try again. |
| A build fails | **Open log file** (under the buttons), find the first line with `error`, and report it. |
| `Could not copy the game … is the game still running?` | Close the game and **Build** again. |
| The game is slow or stutters | Lower **Settings → Video → resolution scale**; turn **Settings → Debug → Logging** off. |
| A black screen at start | Add `RECOMP_GL_CPU_VERTEX=1` to `jsrf.ini`. A known bug on NVIDIA systems; a fix is in the works. |

**Reporting a problem:** turn on **Settings → Debug → Logging**, play until
it happens, close the game and send `stderr.txt` from the game folder (for a
build problem, `launcher_log.txt` from the launcher's folder). Say what you
did, what happened, and whether it was on a PC or a Steam Deck. Turn Logging
off afterwards: it slows the game.

---

## 9. What's in the folders

Next to the launcher:

| | What it is |
|---|---|
| `jsrf/` | your game files, the built game, `jsrf.ini` and your saves |
| `source/` | the project, downloaded by Update |
| `tools/` | Python, Zig, CMake, Ninja (about 400 MB) |
| `build/` | the compiler's work files |
| `launcher.ini`, `launcher_log.txt` | the launcher's settings and log |

<details>
<summary>📁 Inside the project (<code>source/</code>)</summary>

| Folder | What it is |
|---|---|
| `jsrf_recomp/src/` | the project's own code and the generated code (`src/recomp/gen/`, made from your game file, never uploaded) |
| `jsrf_recomp/analysis/` | the map of the game's program: where each function starts and ends, names, calling conventions, per release (no game code) |
| `jsrf_recomp/tools/` | the translator's scripts and developer tools |
| `jsrf_recomp/portable/` | the default `jsrf.ini` |
| `jsrf_recomp/README_JSRF.md` | the development history of every version |
| `xboxrecomp/` | the toolkit: the translator and the runtime that recreates the Xbox |
</details>

---

## 10. Credits and licences

### How this was made

Most of the code here — the JSRF-specific fixes, the changes to the
`xboxrecomp` runtime, the tools and this README — was written by
[Claude](https://claude.ai) (Anthropic's AI). Miggas directed the work: chose
what to fix and when, tested each build on Windows and the Steam Deck, sent the
logs, found the bugs by playing, and made the calls on the approach. The game
was played from start to finish this way. The commit history marks Claude as
co-author.

### Credits

- [sp00nznet](https://github.com/sp00nznet/xboxrecomp) for the `xboxrecomp` toolkit this port is
  built on.
- Piaka for the borderless window tip, skipping the intro logos, and the HUD
  placement from his widescreen work.
- mstan's JSRFRecomp for the idea of holding START to skip cutscenes.
- The quit window and the "Building Shaders" message use the
  [NiseJSRF](https://www.dafont.com/nisejsrf.font) font by Yoyo ("100% Free"),
  drawn with [stb_truetype](https://github.com/nothings/stb) (public domain).
- s-ilent for MSAA (`RECOMP_GL_MSAA`, [pull request #4](https://github.com/Miggas/JSRF-Recomp/pull/4)).
- [andeecollard's jsrf-recomp](https://github.com/andeecollard/jsrf-recomp) (MIT)
  for the DirectSound lift (PC sound mixing, `RECOMP_DSOUND_LIFT`) and chapter
  select (`RECOMP_CHAPTER_SELECT`).
- [JuMatt's JetSetRadioFuture-Unleashed](https://github.com/JuMatt/JetSetRadioFuture-Unleashed)
  (MIT): building the shaders ahead ("Building Shaders") and the modern 3rd
  person camera were inspired by it, and improved upon by Miggas (the camera
  turning when standing still and staying until you move, easing back as soon
  as you set off, invert and sensitivity settings). Parts of the camera's code
  are adapted from Unleashed's; its licence notice is in [`NOTICE`](NOTICE).
- The launcher draws its window with [Nuklear](https://github.com/Immediate-Mode-UI/Nuklear)
  (public domain / MIT) on [SDL2](https://libsdl.org) (zlib), and builds with
  [Zig](https://ziglang.org) (MIT).
- This project's own files (everything outside `xboxrecomp/`) are MIT
  licensed — see [`LICENSE`](LICENSE).
- `xboxrecomp/` is MIT licensed (see `xboxrecomp/LICENSE`). Some of its audio
  files come from [xemu](https://github.com/xemu-project/xemu) under
  LGPL-2.1 — see `xboxrecomp/NOTICE` for the full list and their authors.
- *Jet Set Radio Future* is © SEGA. Xbox is a trademark of Microsoft. This
  project is not affiliated with or endorsed by SEGA or Microsoft, and includes
  none of their code or content.
