# Jet Set Radio Future — PC recompilation

This project turns the original Xbox version of *Jet Set Radio Future* (2002)
into a game you can run natively on Windows, and on a Steam Deck through
Proton. It is not an emulator. The game's own program is translated, once,
into C code, and that code is compiled into a normal Windows `.exe`. The
Xbox's hardware (graphics, sound, controllers, memory card) is recreated by
the runtime in `xboxrecomp/`.

> **You need your own copy of the game.** Nothing from the game is in this
> repository: no code, no graphics, no music. The translation is made on your
> computer, from your copy.

---

## Contents

1. [What you need](#1-what-you-need)
2. [Install the tools (once)](#2-install-the-tools-once)
3. [Get this project](#3-get-this-project)
4. [Add your game files](#4-add-your-game-files)
5. [Build the game](#5-build-the-game)
6. [Play](#6-play)
7. [Playing on a Steam Deck](#7-playing-on-a-steam-deck)
8. [Controls](#8-controls)
9. [Settings (jsrf.ini)](#9-settings-jsrfini)
10. [Saves](#10-saves)
11. [Updating to a newer version](#11-updating-to-a-newer-version)
12. [Troubleshooting](#12-troubleshooting)
13. [Reporting a problem](#13-reporting-a-problem)
14. [What's in the folders](#14-whats-in-the-folders)
15. [Credits and licences](#15-credits-and-licences)

---

## 1. What you need

- **A Windows 10 or 11 PC** to build the game. (Once built, the game also runs
  on a Steam Deck. Building on the Deck itself is not supported.)
- **Your copy of Jet Set Radio Future for the original Xbox — the NTSC
  (North American) retail release.** The PAL (European) and NTSC-J (Japanese)
  releases are built differently inside and **will not work**. You can use a
  disc image (`.iso`) or the game's files (`default.xbe` plus the folders next
  to it). The build checks that your `default.xbe` is the right one by its
  SHA-256 fingerprint:
  `bb2410618c35ccab1ab8ad989194bbd50619eeb648a03b21483efca57f36547d`
- **About 5 GB of free disk space** (mostly Visual Studio).
- **A graphics card with OpenGL 3.3** or newer (practically any GPU from the
  last ten years).

## 2. Install the tools (once)

You need three free programs. Install them in this order.

### Visual Studio 2022 Community (the C compiler)

1. Download it from <https://visualstudio.microsoft.com/vs/community/>.
2. Run the installer. When it asks what to install, tick
   **"Desktop development with C++"**. Leave the options on the right as they
   are; they include CMake, which the build also needs.
3. Click *Install* and wait (it is large). You do not need to open Visual
   Studio afterwards — the build scripts use it on their own.

### Python 3 (runs the translator)

1. Download the latest Python 3 from <https://www.python.org/downloads/>.
2. Run the installer and, **on the first screen, tick
   "Add python.exe to PATH"**, then click *Install Now*.

### capstone (a Python package the translator uses)

1. Open the Start menu, type `cmd` and open **Command Prompt**.
2. Type this and press Enter:
   ```
   py -3 -m pip install capstone==5.0.7
   ```
3. It should end with `Successfully installed capstone-5.0.7`.

## 3. Get this project

Either:

- **Download the ZIP** — on the GitHub page, click the green **Code** button →
  **Download ZIP**, then unzip it somewhere with a short path, for example
  `C:\JSRF\`. Or:
- **With git** — `git clone <this repository's URL> C:\JSRF`

You should end up with:

```
C:\JSRF\
    xboxrecomp\        the runtime and the translator
    jsrf_recomp\       the game project — you will work in here
    README.md          this file
```

> Avoid folders that are synced (OneDrive, Dropbox) or have very long paths;
> the build creates many files.

## 4. Add your game files

Copy **all** of the game's files into `jsrf_recomp\game\` — `default.xbe` and
every folder next to it — so that you have `C:\JSRF\jsrf_recomp\game\default.xbe`.

**If you only have a disc image (`.iso`)**, the toolkit can unpack it. In
Command Prompt:

```
cd C:\JSRF\xboxrecomp
py -3 -m tools.xiso unpack "C:\path\to\your\game.iso" -o ..\jsrf_recomp\game
```

## 5. Build the game

Open the `C:\JSRF\jsrf_recomp\` folder in Explorer and double-click, in order:

| Step | Double-click | What it does | How long |
|---|---|---|---|
| 1 | `generate_code.bat` | Checks your `default.xbe`, then translates the game's program into C code (in `src\recomp\gen\`). | 2–3 min |
| 2 | `rebuild.bat` | Compiles everything into `jsrf_recomp.exe`. The first time it also sets up the build folder. | 5–15 min the first time, faster after |

Each window ends with **"Done"** / **"Build OK"** when it worked, or
**"FAILED — scroll up for the first error"** when it didn't (see
[Troubleshooting](#12-troubleshooting)).

You only need to run `generate_code.bat` again when you update the project
(section 11). `rebuild.bat` is what you run after any change.

## 6. Play

You have two ways to play:

**A. A ready-to-play game folder (recommended).** Double-click
`build_portable.bat`. It builds the game and copies `jsrf_recomp.exe` and a
settings file, `jsrf.ini`, into your game folder (`jsrf_recomp\game\`). From
then on you can simply double-click **`jsrf_recomp.exe` in the game folder**,
make a desktop shortcut to it, or copy the whole game folder to another PC or
a Steam Deck. To put them in a different game folder:
`build_portable.bat D:\Games\JSRF`.

**B. Straight from the project.** Double-click `play_jsrf.bat`.

`F11` or `Alt+Enter` switches between window and full screen at any time.

## 7. Playing on a Steam Deck

1. On your PC, do **6A** above. Your game folder now holds the game files,
   `jsrf_recomp.exe` and `jsrf.ini`.
2. Copy that whole folder to the Deck (USB stick, SD card, or over the network),
   for example to `/home/deck/Games/JSRF`.
3. On the Deck, switch to **Desktop Mode**, open Steam, and choose
   **Games → Add a Non-Steam Game to My Library… → Browse…**. Set the file type
   to *All Files*, pick `jsrf_recomp.exe`, and add it.
4. In Steam, right-click the new entry → **Properties → Compatibility** → tick
   **"Force the use of a specific Steam Play compatibility tool"** and choose a
   recent **Proton** version.
5. Go back to Gaming Mode and play.

Settings that suit the Deck (already the defaults in the provided `jsrf.ini`):
`RECOMP_GL_SCALE=2` (1280×960 inside the game) and `RECOMP_GL_TEX_WATCH=0`
(avoids stutter).

### Choose a power mode

There are two ways to run the game on the Deck. Both give a steady 60 fps;
they differ in power use and in how much setup they need.

| | **Option 1 — Standard** | **Option 2 — Power saving** |
|---|---|---|
| Setup | none | one line in `jsrf.ini` + PowerTools |
| Power use (measured) | about 7 W | 3–4 W in quieter areas, 5–7 W in busy ones |
| Battery and heat | normal for a demanding game | noticeably longer battery, cooler, quieter fan |
| Affects other games | no | only if you set the governor for all games (see below) |

**Why there is a difference.** While the game runs, its threads constantly hand
work to each other and wait for the other side to finish. They can wait in two
ways:

- **Spinning:** the thread keeps checking, over and over, whether the other side
  is done. It reacts instantly, but a CPU core stays busy the whole time —
  that is power spent on waiting.
- **Sleeping:** the thread tells the system "wake me when it's done" and the core
  rests. That saves a lot of power, but it has a side effect on the Deck: with
  its cores resting, the Deck lowers the CPU clock to save even more, and its
  default settings take 2–3 seconds to raise it again when the game suddenly
  needs it. In busy areas like 99th Street that shows up as a few seconds under
  50 fps.

Option 1 keeps the spinning, so the Deck always runs fast. Option 2 sleeps, and
fixes the side effect by telling the Deck to raise its clock straight away
(the **performance** CPU governor).

#### Option 1 — Standard (nothing to do)

Leave `jsrf.ini` as it is. In the file, the line reads `;RECOMP_SLEEP_WAITS=vblank`
(off, so the default `vblank` is used): only one small wait sleeps — the one
that avoids music crackling — and the rest spin.

#### Option 2 — Power saving

**Step 1: tell the game to sleep while waiting.** Open `jsrf.ini` (in the game
folder) with a text editor and change the line

```
;RECOMP_SLEEP_WAITS=vblank
```
to
```
RECOMP_SLEEP_WAITS=all
```

(remove the `;` and change `vblank` to `all`). Save the file.

**Step 2: set the CPU governor to "performance" with PowerTools.** PowerTools
is a plugin for **Decky Loader**, a free plugin system for the Deck's Quick
Access menu.

1. *Install Decky Loader (once).* Switch to **Desktop Mode**, open a web browser,
   go to <https://decky.xyz/>, download the installer and run it (choose the
   normal *release* version). Then go back to **Gaming Mode**.
2. *Install PowerTools (once).* Press the **`…`** (Quick Access) button. A new
   **plug icon** (Decky) is now at the bottom of the menu. Open it, then the
   **store icon** at its top, search for **PowerTools** and install it.
3. *Set the governor for this game.* Start Jet Set Radio Future, then press
   **`…`** → plug icon → **PowerTools**:
   - turn on the per-game setting (so the change only applies to this game);
   - in the **CPU** section, set the **governor** to **performance** (if the
     governor is not shown, turn on the advanced or per-core CPU options first).

   The labels can differ a little between PowerTools versions; look for
   *governor* in the CPU options. PowerTools remembers the setting and applies
   it every time you start the game, and puts your normal setting back for
   other games.

**Without PowerTools** you can set it by hand: in Desktop Mode, open **Konsole**
and type (it asks for your sudo password — if you have never set one, run
`passwd` first):

```
echo performance | sudo tee /sys/devices/system/cpu/cpufreq/policy*/scaling_governor
```

This applies to **every** game until you restart the Deck.

**If you use only Step 1 without Step 2**, the game still works, but busy areas
drop under 50 fps for a few seconds whenever the CPU has to speed up. Use both
steps, or neither.

## 8. Controls

**A controller** (Xbox, PlayStation, the Steam Deck's own) works as an Xbox
controller. **The keyboard** works as controller 1:

| Keyboard | Xbox controller |
|---|---|
| Arrow keys | D-pad |
| Numpad 8 / 2 / 4 / 6 | Left stick |
| I / K / J / L | Right stick |
| Z / X / A / S | A / B / X / Y |
| Q / E | White / Black |
| 1 / 3 | Left / Right trigger |
| Enter | START |
| Backspace | BACK |

The game window must have the focus for keys to count.

## 9. Settings (jsrf.ini)

Settings live in `jsrf.ini`, next to `jsrf_recomp.exe`. Open it with Notepad.
Each setting is one line, `NAME=value`. A line starting with `;` is switched
off: delete the `;` to switch it on. The full list with explanations is in
`jsrf_recomp\jsrf.ini.example`. The ones you are most likely to want:

| Setting | What it does |
|---|---|
| `RECOMP_GL_SCALE=2` | Internal resolution: 1 = 640×480 (original), 2 = 1280×960, 3 = 1920×1440, 4 = 2560×1920. Higher is sharper and needs a stronger GPU. |
| `RECOMP_FULLSCREEN=1` | Start in full screen. |
| `RECOMP_GL_VSYNC=1` | Wait for the screen's refresh (no tearing). |
| `RECOMP_APU_GAIN=0.5` | Master volume. |
| `RECOMP_APU_REVERB=0` | Turns off the echo effect in some areas. |
| `RECOMP_SAVE_DIR=…` | Keep saves somewhere other than the game folder. |
| `RECOMP_QUIET=1` | Leave this on: it turns off diagnostics that cost speed. |

**Do not change** the lines in the `[game]` section of `jsrf.ini` — the game
needs them to start.

If you play with `play_jsrf.bat` instead of the exe in the game folder, its
settings are inside the `.bat` file (same names).

## 10. Saves

Saves are kept in a `savegame` folder inside your game folder (for example
`jsrf_recomp\game\savegame\`). Copy that folder to back them up, or to move
your progress to another PC or the Deck.

## 11. Updating to a newer version

1. Get the new version of the project (download the ZIP again, or `git pull`).
   If you download a ZIP, unzip it **into a new folder** and move your `game`
   folder across — don't copy the old `build` folder.
2. Double-click `generate_code.bat`, then `rebuild.bat` — or
   `build_portable.bat` if you play from the game folder. Your `jsrf.ini` and
   saves are kept.

## 12. Troubleshooting

| You see | What to do |
|---|---|
| `Python 3 not found` | Install Python (section 2) with **"Add python.exe to PATH"** ticked. Close and reopen the window afterwards. |
| `Python package 'capstone' is missing` | Run `py -3 -m pip install capstone==5.0.7` in Command Prompt. |
| `default.xbe not found` | The game files must be in `jsrf_recomp\game\`, with `default.xbe` directly in it (not in a subfolder). |
| `this default.xbe is not the one the analysis was made from` | Your copy is a different version — most likely the PAL (European) or NTSC-J (Japanese) release. Only the NTSC (North American) retail release works (section 1). |
| `No recompiled code yet in src\recomp\gen` | Run `generate_code.bat` before `rebuild.bat`. |
| `cmake` not found, or `Visual Studio 17 2022` not found | Install Visual Studio 2022 with **"Desktop development with C++"** (section 2). |
| CMake says the cache or source folder doesn't match | You moved or copied the project. Delete the `jsrf_recomp\build` folder and run `rebuild.bat` again. |
| `BUILD FAILED` with something else | Scroll up to the first line that says `error`, and report it (section 13). |
| `Could not copy the exe … is the game still running?` | Close the game and run `build_portable.bat` again. |
| The game is slow or stutters | Lower `RECOMP_GL_SCALE`; make sure `RECOMP_QUIET=1` is on; on a laptop, plug it in. |

## 13. Reporting a problem

A log lets someone see what happened:

- **Playing from the game folder:** in `jsrf.ini`, remove the `;` in front of
  `RECOMP_LOG=stderr.txt`, and put a `;` in front of `RECOMP_QUIET=1`. Play
  until the problem happens, close the game, and send `stderr.txt` from the
  game folder.
- **From the project:** double-click `run_jsrf.bat` instead of
  `play_jsrf.bat`; it writes `stderr.txt` in `jsrf_recomp\`.

Say what you did, what you expected and what happened instead, and whether
it was on a PC or a Steam Deck. Put the `;` back in front of `RECOMP_LOG`
afterwards — the log makes the game slower.

## 14. What's in the folders

**`jsrf_recomp\` — the scripts you use:**

| File | Use it to |
|---|---|
| `generate_code.bat` | translate your `default.xbe` into C (after getting or updating the project) |
| `rebuild.bat` | build the game |
| `build_portable.bat` | build and put the exe + `jsrf.ini` in the game folder |
| `play_jsrf.bat` | play from the project |
| `run_jsrf.bat` | play with a full log, for reporting a problem |
| `debug\` | tools for diagnosing freezes and graphics problems (developers) |

**The rest:**

| Folder | What it is |
|---|---|
| `jsrf_recomp\game\` | your game files go here (never uploaded) |
| `jsrf_recomp\src\` | the project's own code (`main.c`, `recomp_manual.c`) and, after step 5, the generated code in `src\recomp\gen\` (never uploaded) |
| `jsrf_recomp\analysis\` | the map of the game's program the translator works from: where each function starts and ends, names, calling conventions — addresses and names, no game code |
| `jsrf_recomp\tools\` | the scripts behind the `.bat` files, and developer tools |
| `jsrf_recomp\portable\` | the default `jsrf.ini` that `build_portable.bat` copies |
| `jsrf_recomp\README_JSRF.md` | the development history of every version, with measurements |
| `xboxrecomp\` | the toolkit: the translator (Python) and the runtime that recreates the Xbox (kernel, NV2A graphics on OpenGL, MCPX audio, controllers) |

## 15. Credits and licences

- `xboxrecomp/` is MIT licensed (see `xboxrecomp/LICENSE`). Some of its audio
  files come from [xemu](https://github.com/xemu-project/xemu) under
  LGPL-2.1 — see `xboxrecomp/NOTICE` for the full list and their authors.
- *Jet Set Radio Future* is © SEGA. Xbox is a trademark of Microsoft. This
  project is not affiliated with or endorsed by SEGA or Microsoft, and
  includes none of their code or content.
