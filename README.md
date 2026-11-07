<div align="center">

# Jet Set Radio Future — PC recompilation

**The original Xbox game, translated into a native Windows and Linux program.**<br>
Runs on Windows 10/11, Linux and the Steam Deck.

![Windows](https://img.shields.io/badge/Windows-10%20%7C%2011-0078D6?logo=windows&logoColor=white)
![Linux](https://img.shields.io/badge/Linux-native-FCC624?logo=linux&logoColor=black)
![Steam Deck](https://img.shields.io/badge/Steam%20Deck-native-1A9FFF?logo=steamdeck&logoColor=white)
![Version](https://img.shields.io/badge/game-NTSC%20(US)%20%2B%20PAL%20(EU)%20%2B%20NTSC--J%20(JP,%20testing)-orange)
![Bring your own game](https://img.shields.io/badge/game%20files-not%20included-lightgrey)

</div>

This is **not an emulator**. The game's own program is translated, once, into C
code on your computer, and that code is compiled into a normal program. The
Xbox's hardware — graphics, sound, controllers, memory card — is recreated by
the runtime in `xboxrecomp/`.

> [!NOTE]
> **Made with AI.** Most of this project's code was written by Claude
> (Anthropic's AI), directed, tested and played through by Miggas. See
> [how this was made](#how-this-was-made).

> [!IMPORTANT]
> **You need your own copy of the game.** Nothing from the game is in this
> repository: no code, no graphics, no music. The translation is made on your
> computer, from your copy.

> [!WARNING]
> **Please read the instructions first before proceeding with the recompilation.**

### How hard is each step?

| Badge | Meaning |
|---|---|
| ![Easy](https://img.shields.io/badge/difficulty-easy-brightgreen) | Click a button or copy a file. Anyone can do it. |
| ![Medium](https://img.shields.io/badge/difficulty-medium-yellow) | A few more steps. Follow the instructions closely. |
| ![Hard](https://img.shields.io/badge/difficulty-hard-red) | Typing commands. Optional — there is always an easier way. |

---

## Contents

| | Section | Difficulty |
|---|---|---|
| 1 | [What you need](#1-what-you-need) | — |
| 2 | [Get the launcher](#2-get-the-launcher) | ![Easy](https://img.shields.io/badge/-easy-brightgreen) |
| 3 | [Add your game files](#3-add-your-game-files) | ![Easy](https://img.shields.io/badge/-easy-brightgreen) |
| 4 | [Build the game](#4-build-the-game) | ![Easy](https://img.shields.io/badge/-easy-brightgreen) |
| 5 | [Play](#5-play) | ![Easy](https://img.shields.io/badge/-easy-brightgreen) |
| 6 | [The launcher](#6-the-launcher) | — |
| 7 | [Playing on a Steam Deck](#7-playing-on-a-steam-deck) | ![Easy](https://img.shields.io/badge/-easy-brightgreen) |
| 8 | [Controls](#8-controls) · [Closing the game](#closing-the-game) | — |
| 9 | [Settings (jsrf.ini)](#9-settings-jsrfini) | ![Easy](https://img.shields.io/badge/-easy-brightgreen) |
| 10 | [Saves](#10-saves) | ![Easy](https://img.shields.io/badge/-easy-brightgreen) |
| 11 | [Updating to a newer version](#11-updating-to-a-newer-version) | ![Easy](https://img.shields.io/badge/-easy-brightgreen) |
| 12 | [Troubleshooting](#12-troubleshooting) | — |
| 13 | [Reporting a problem](#13-reporting-a-problem) | ![Easy](https://img.shields.io/badge/-easy-brightgreen) |
| 14 | [What's in the folders](#14-whats-in-the-folders) | — |
| 15 | [Credits and licences](#15-credits-and-licences) | — |

Debug and advanced settings, not in `jsrf.ini`: [DEBUG.md](DEBUG.md).

---

## 1. What you need

| | Requirement |
|---|---|
| 💻 | **A Windows 10/11 PC, a Linux PC or a Steam Deck.** The game is built on the same system it runs on. |
| 💿 | **Your copy of Jet Set Radio Future for the original Xbox — the NTSC (North American), PAL (European) or NTSC-J (Japanese) retail release**, as its files (`default.xbe` plus the folders next to it) or as a disc image (`.iso`). |
| 💾 | **About 3 GB of free disk space** (the build tools, the build, and the game's cache). |
| 🌐 | **An internet connection** the first time: the launcher downloads its tools and the project. |
| 🎮 | **A graphics card with OpenGL 3.3** or newer — practically any GPU from the last ten years. |

Nothing else has to be installed: the launcher brings its own tools (Python,
the Zig compiler, CMake).

> [!WARNING]
> **Releases:** NTSC (North America) and PAL (Europe) work. NTSC-J (Japan)
> and the US Sega GT 2002 + JSRF disc are new and still being tested; the
> European and Asian combo discs are not supported yet. On a combo disc the
> game is `SegaJSRF.xbe` (its `default.xbe` is the menu): copy the whole disc,
> and the build and the exe use `SegaJSRF.xbe`. The build checks the game's code and data inside your
> `default.xbe`, finds which release it is, and stops if it is none of them.
> It does **not** compare the whole file: many dumps have a patched header
> (region, allowed media), which changes the file's SHA-256 but not the game,
> and those work fine.

---

## 2. Get the launcher

![Easy](https://img.shields.io/badge/difficulty-easy-brightgreen) &nbsp; ⏱️ 1 minute

1. Download it from [Releases](https://github.com/Miggas/JSRF-Recomp/releases):
   `jsrf_launcher-windows.zip` or `jsrf_launcher-linux.tar.gz`.
2. Unpack it into an **empty folder** with a short path, for example `C:\JSRF\`
   or `~/Games/JSRF`.
3. Linux / Steam Deck: mark `jsrf_launcher` as executable (right-click →
   **Properties → Permissions → Is executable**).
4. Start it. It makes a `jsrf` folder next to itself, for your game files.

> [!NOTE]
> Avoid folders that are synced (OneDrive, Dropbox): the build creates many files.

---

## 3. Add your game files

![Easy](https://img.shields.io/badge/difficulty-easy-brightgreen) &nbsp; ⏱️ a few minutes

Copy **all** of the game's files into the `jsrf` folder — `default.xbe` and
every folder next to it — so that `jsrf\default.xbe` exists.

Your game files are somewhere else? In **Play**, click **Choose folder** and
pick that folder, or drop `default.xbe` onto the launcher's window.

<details>
<summary>💿 I only have a disc image (.iso) &nbsp; <img src="https://img.shields.io/badge/-hard-red"></summary>

First do **Update → Download** in the launcher ([section 4](#4-build-the-game)):
it gets the project and the tools. Then, in a terminal (Command Prompt on
Windows) in the launcher's folder:

```
cd source\xboxrecomp
..\..\tools\python\python.exe -m tools.xiso unpack "C:\path\to\game.iso" -o ..\..\jsrf
```

On Linux: `cd source/xboxrecomp`, then
`../../tools/python/bin/python3 -m tools.xiso unpack /path/to/game.iso -o ../../jsrf`
</details>

---

## 4. Build the game

![Easy](https://img.shields.io/badge/difficulty-easy-brightgreen) &nbsp; ⏱️ 10–20 minutes the first time

| Step | In the launcher | What it does | How long |
|:---:|---|---|---|
| **1** | **Update → Download** | Downloads the project and the build tools (about 150 MB). | a few minutes |
| **2** | **Build → Generate code** (first time, and when an update needs it) | Checks your `default.xbe` and translates the game's program into C. | a few minutes |
| **3** | **Build → Build** | Compiles it into `jsrf_recomp` (`.exe` on Windows) in your game folder. | 5–15 min the first time |

**Play → Build** does steps 2 and 3 in one go.

The **Play** tab shows a ✓ or ✗ for each part. Click a ✗ to go where it is
fixed. The **Build** tab has **Generate code** and **Build** separately; each
checks first and skips what is already up to date.

---

## 5. Play

![Easy](https://img.shields.io/badge/difficulty-easy-brightgreen)

Click **PLAY** in the launcher, or start `jsrf_recomp` (`jsrf_recomp.exe` on
Windows) in your game folder directly. The game folder can be copied to
another computer of the same system as it is.

> [!TIP]
> **`F11`** or **`Alt+Enter`** switches between window and full screen at any time.

---

## 6. The launcher

| Tab | What it does |
|---|---|
| **Play** | The game folder, what is ready (✓ / ✗), **PLAY**. |
| **Settings** | `jsrf.ini` in tabs: Gameplay, Video, Input, Audio, Debug, and **All settings** for every line. |
| **Build** | **Generate code** and **Build**, each skipping what is up to date. |
| **Update** | Gets the newest version of the project. Your game files, `jsrf.ini` and saves are kept. Then **Build**. |

`launcher.ini`, next to the launcher, holds its own settings:

| Setting | What it does |
|---|---|
| `BRANCH=test` | Update downloads the `test` branch (newest, less tested). Not set: `main`. |
| `DEBUG_MENU=1` | Shows the **Debug** tab: Force Generate/Build, the checks (generated code, `default.xbe`, the hard-disk cache), Play stopping after 30 s with a dump, Sample a hang, Trace one frame, and the whole log. |

Everything the launcher does is also written to `launcher_log.txt` next to it.
The launcher's code is on the [`launcher`](https://github.com/Miggas/JSRF-Recomp/tree/launcher) branch.

---

## 7. Playing on a Steam Deck

![Easy](https://img.shields.io/badge/difficulty-easy-brightgreen) &nbsp; ⏱️ 20 minutes the first time

The game is built on the Deck itself and runs natively, without Proton.

1. Switch to **Desktop Mode**.
2. Do sections [2](#2-get-the-launcher) to [4](#4-build-the-game) there, with
   the Linux launcher.
3. In Steam, choose **Games → Add a Non-Steam Game to My Library… → Browse…**,
   set the file type to *All Files*, pick `jsrf_launcher` (or `jsrf_recomp` in
   the game folder, to start the game directly) and add it. **Do not** force a
   Proton version.
4. Go back to **Gaming Mode** and play. The launcher works with the
   controller and the touchscreen, and closes when the game starts. To quit
   the game, click both sticks ([Closing the game](#closing-the-game)).

Nothing else to set up: the native build doesn't need PowerTools or the
performance governor.

> [!NOTE]
> The provided `jsrf.ini` already has the settings that suit the Deck:
> `RECOMP_GL_SCALE=2` (1280×960 inside the game).

### With Proton instead (the Windows build)

![Medium](https://img.shields.io/badge/difficulty-medium-yellow)

Only needed if you want to run `jsrf_recomp.exe`. Build on a Windows PC, copy
the game folder to the Deck, add `jsrf_recomp.exe` as a non-Steam game, and in
**Properties → Compatibility** force **Proton 11**. Only Proton 11 is tested.

#### Choose a power mode

Both modes give a steady **60 fps**. They differ in power use and setup.

| | 🔋 **Mode 1 — Standard** | 🌿 **Mode 2 — Power saving** |
|---|---|---|
| **Difficulty** | ![Easy](https://img.shields.io/badge/-nothing%20to%20do-brightgreen) | ![Medium](https://img.shields.io/badge/-medium-yellow) |
| **Setup** | none | one line in `jsrf.ini` + the PowerTools plugin |
| **Power use (measured)** | about 7 W | 3–4 W in quieter areas, 5–7 W in busy ones |
| **Battery and heat** | normal for a demanding game | noticeably longer battery, cooler, quieter fan |
| **Affects other games** | no | no, when set per game in PowerTools |

<details>
<summary>💡 Why is there a difference?</summary>

While the game runs, its threads constantly hand work to each other and wait
for the other side to finish. They can wait in two ways:

- **Spinning:** the thread keeps checking, over and over, whether the other side
  is done. It reacts instantly, but a CPU core stays busy the whole time — that
  is power spent on waiting.
- **Sleeping:** the thread tells the system "wake me when it's done" and the core
  rests. That saves a lot of power, but it has a side effect on the Deck: with
  its cores resting, the Deck lowers the CPU clock, and its default settings
  take 2–3 seconds to raise it again when the game suddenly needs it. In busy
  areas like 99th Street that shows up as a few seconds under 50 fps.

Mode 1 keeps the spinning, so the Deck always runs fast. Mode 2 sleeps, and
fixes the side effect by telling the Deck to raise its clock straight away (the
**performance** CPU governor).
</details>

##### 🔋 Mode 1 — Standard

![Easy](https://img.shields.io/badge/difficulty-nothing%20to%20do-brightgreen)

Leave `jsrf.ini` as it is. The line `;RECOMP_SLEEP_WAITS=vblank` stays switched
off (it starts with `;`), so the default is used.

##### 🌿 Mode 2 — Power saving

![Medium](https://img.shields.io/badge/difficulty-medium-yellow) &nbsp; ⏱️ 15 minutes the first time

> [!IMPORTANT]
> Do **both** steps. With only Step 1, busy areas drop under 50 fps for a few
> seconds whenever the CPU has to speed up.

**Step 1 — let the game sleep while it waits** ![Easy](https://img.shields.io/badge/-easy-brightgreen)

In the launcher: **Settings → All settings**, tick `RECOMP_SLEEP_WAITS` and set
it to `all`, then **Save**. Or in `jsrf.ini` (in the game folder):

```diff
- ;RECOMP_SLEEP_WAITS=vblank
+ RECOMP_SLEEP_WAITS=all
```

**Step 2 — set the CPU governor to "performance" with PowerTools** ![Medium](https://img.shields.io/badge/-medium-yellow)

PowerTools is a plugin for **Decky Loader**, a free plugin system for the
Deck's Quick Access menu.

1. **Install Decky Loader (once).** Switch to **Desktop Mode**, open a web
   browser, go to <https://decky.xyz/>, download the installer and run it
   (choose the normal *release* version). Then go back to **Gaming Mode**.
2. **Install PowerTools (once).** Press the **`…`** (Quick Access) button. A new
   **plug icon** (Decky) is now at the bottom of the menu. Open it, then the
   **store icon** at its top, search for **PowerTools** and install it.
3. **Set the governor for this game.** Start Jet Set Radio Future, then press
   **`…`** → plug icon → **PowerTools**:
   - turn on the **per-game** setting, so the change only applies to this game;
   - in the **CPU** section, set the **governor** to **performance**. If the
     governor is not shown, turn on the advanced or per-core CPU options first.

> [!NOTE]
> The labels can differ a little between PowerTools versions — look for
> *governor* in the CPU options. PowerTools remembers the setting, applies it
> every time you start the game, and puts your normal setting back for other
> games.

<details>
<summary>⌨️ Without PowerTools — by hand in Konsole &nbsp; <img src="https://img.shields.io/badge/-hard-red"></summary>

In **Desktop Mode**, open **Konsole** and type (it asks for your sudo password —
if you have never set one, run `passwd` first):

```
echo performance | sudo tee /sys/devices/system/cpu/cpufreq/policy*/scaling_governor
```

⚠️ **This applies to every game until you restart the Deck.**
</details>

---

## 8. Controls

**A controller** (Xbox, PlayStation, the Steam Deck's own) works as an Xbox
controller. **The keyboard** works as controller 1:

| ⌨️ Keyboard | 🎮 Xbox controller |
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

> [!NOTE]
> The game window must have the focus for keys to count.

### Closing the game

| 🎮 Controller | ⌨️ Keyboard | Then |
|---|---|---|
| Click **both sticks** (L3 + R3) | `Shift` + `Ctrl` | Pick **YES** with left / right and press **A** (`Z`). **B** (`X`) goes back to the game. |

The game pauses while the window is open. Closing the game window or `Alt+F4`
also quits.

### Skipping a cutscene

Hold **START** (`Enter`) for about 1.3 seconds, also the first time you see it.

---

## 9. Settings (jsrf.ini)

![Easy](https://img.shields.io/badge/difficulty-easy-brightgreen)

Settings live in `jsrf.ini`, in the game folder. The easiest way to change
them is the launcher's **Settings** tab. In a text editor: each setting is one
line, `NAME=value`; a line starting with `;` is switched off: delete the `;` to
switch it on. Steam launch options override the file.

> [!CAUTION]
> **Do not change** the first lines of the `[game]` section. The game needs
> them to start.

**`[game]`**

| Setting | What it does |
|---|---|
| `RECOMP_QUIET=1` | Fewer log messages (faster). Put a `;` in front when making a log for a bug report. |
| `RECOMP_SKIP_INTRO=1` | Skip the intro logos. *By Piaka.* |
| `RECOMP_HOLD_SKIP=0` | Turns off skipping cutscenes by holding START for about 1.3 seconds (on by default; works the first time you see one too). *Idea from mstan's JSRFRecomp.* |
| `RECOMP_LANGUAGE=french` | The language the game is told the Xbox is set to: `english` (default), `french`, `german`, `spanish`, `italian`. The PAL (European) release has all five; the NTSC release is in English. *Idea by Miggas.* |
| `RECOMP_SAVE_STRICT=1` | Refuse saves from other sources (a real Xbox, emulators, save editors), as the Xbox does. Off by default, so those saves load. |

**`[display]`**

| Setting | What it does |
|---|---|
| `RECOMP_GL_SCALE=2` | Internal resolution: `1` = 640×480, `2` = 1280×960, `3` = 1920×1440, `4` = 2560×1920. Higher is sharper and needs a stronger GPU. `2` is best for the Steam Deck. |
| `RECOMP_GL_ANISO=16` | Anisotropic filtering: `2`, `4`, `8` or `16`. |
| `RECOMP_GL_AA=smaa_high` | Anti-aliasing: `fxaa`, `smaa_low`, `smaa_medium`, `smaa_high` or `smaa_ultra`. Off when not set. A higher `RECOMP_GL_SCALE` also smooths edges. |
| `RECOMP_FULLSCREEN=1` | Start in full screen. F11 or Alt+Enter switches. |
| `RECOMP_GL_PRESENT=dxgi` | Borderless window (Windows only). May lower performance. *Tip from Piaka.* |
| `RECOMP_WIDESCREEN=auto` | Widescreen: `1` = 16:9, `auto` = your monitor's shape. For other shapes use `RECOMP_ASPECT=w:h` instead (for example `RECOMP_ASPECT=21:9`). The HUD moves to the screen edges and the menus are wide too. *HUD placement from Piaka's widescreen work.* |
| `RECOMP_GL_VSYNC=1` | VSync. |
| `RECOMP_GL_PREBUILD=0` | Turns off building the missing shaders at start ("Building Shaders"); they're built during play instead. |
| `RECOMP_CURSOR_HIDE=0` | Keeps the mouse pointer visible. By default it hides over the game window after 2 s without moving. |

**`[input]`**

| Setting | What it does |
|---|---|
| `RECOMP_USB_PADS=1` | Number of players (1 to 4). With 2 or more, players join in the order they press a button. |
| `RECOMP_RUMBLE=100` | Controller rumble: `0` = off, `1` to `100` = strength in percent. |
| `RECOMP_CAM_ORBIT=0` | The original camera. By default the right stick turns the camera around the skater (the left stick follows the view). While skating it swings back behind 1 s after you let go; standing still it stays until you move: then up / down swings back and the left / right you chose becomes the way you go. The L trigger puts it behind at once. *How it works: from JuMatt's JetSetRadioFuture-Unleashed.* |
| `RECOMP_CAM_INVERT_X=1` / `RECOMP_CAM_INVERT_Y=1` | Inverts the camera's left / right or up / down. |
| `RECOMP_CAM_SENS=1.0` | Camera speed (`2.0` = twice as fast). |
| `RECOMP_QUIT_COMBO=0` | Turns the quit window off (on by default: click both sticks, L3 + R3; left / right picks YES or NO, A chooses, B goes back). |

**`[audio]`**

| Setting | What it does |
|---|---|
| `RECOMP_APU_GAIN=0.5` | Master volume (default `0.5`). |

**`[files]`**

| Setting | What it does |
|---|---|
| `RECOMP_LOG=stderr.txt` | Write a log, for bug reports ([section 13](#13-reporting-a-problem)). |
| `RECOMP_PROFILE=1` | Performance profiling in the log. Slows the game down. |

**`[draw distance]`** — leave these alone unless you know what you're doing.

| Setting | What it does |
|---|---|
| `RECOMP_DRAW_AREAS=…` | Pop-in (buildings appearing as you get close). `hops` (the default) fixes almost all of it; `hops3`, `hops4`… reach further. `game` = original behaviour, as on the Xbox. `all` = draw everything (can show far-away copies on looping maps). *Idea by Miggas.* |
| `RECOMP_DRAW_AREAS_SKIP=EE7A:96,105` | Map pieces that should never be drawn. Format: `MapID:area,area;MapID:area`. A map ID alone (`EE7A`) means the whole map: no pop-in fix there. |
| `RECOMP_DRAW_AREAS_ADD=` | Map pieces that should always be drawn. Same format; a map ID alone draws the whole map. |
| `RECOMP_AREA_PICKER=1` | Finds the numbers for the two lists above. `1` = skip mode, `2` = add mode. F6/F7 select an area (shown in the window title), F8 toggles it, F10 clears all. The list is written to the log, ready to copy. |
| `RECOMP_OBJ_DRAW_DIST=2` | Draw distance of street objects (tanks, police, vehicles…). `1` = original. *Idea by Miggas.* |

**`[tools]`**

| Setting | What it does |
|---|---|
| `RECOMP_FREECAM=1` | Free camera. F5 = on/off, WASD = move, Shift = up, Ctrl = down, mouse = look, mouse wheel = speed (shown in the window title). The game gets no keyboard input while the camera is flying. |
| `RECOMP_FREECAM_SPEED=2` | The free camera's start speed (default `2`). |
| `RECOMP_FREECAM_SENS=0.003` | The free camera's mouse sensitivity (default `0.003`). |

**`[compatibility]`**

| Setting | What it does |
|---|---|
| `RECOMP_ACK_FAST=0` | Turns off running the game a frame ahead of the GPU emulation (on by default: the two work at the same time, faster in busy areas). Try it if sound, visuals or input go wrong. |
| `RECOMP_GL_TEX_WATCH=0` | Turns the texture write-watch off (it is on by default on Windows and in the Linux build, off under Proton). Try it if the game stutters. |
| `RECOMP_SLEEP_WAITS=…` | GPU waits. Default on Windows and Linux (the Steam Deck's native build too): `all` (the game's wait spins 0.2 ms, then sleeps). Default under Proton: `vblank`. `none` = highest CPU use. Steam Deck with Proton: see [section 7](#choose-a-power-mode). |

More settings, for testing and for finding problems, are listed in
**[DEBUG.md](DEBUG.md)**. They aren't in `jsrf.ini`; add a line yourself to use one.

---

## 10. Saves

![Easy](https://img.shields.io/badge/difficulty-easy-brightgreen)

Your save is in the **`UDATA`** folder of your game folder (for example
`jsrf\UDATA\`), next to `TDATA`, which holds the game's own
settings. These two folders are tiny: copy them to back up your progress, or to
move it to another PC or the Deck.

| Folder in the game folder | What it holds | Size |
|---|---|---|
| `UDATA\` | 💾 **your save** | tiny |
| `TDATA\` | the game's own settings | tiny |
| `savegame\` | the Xbox hard-disk cache (`Cache\`) and partition images (`.img`) — rebuilt automatically if deleted | up to ~5 GB |
| `shader_cache\` | compiled shaders, so effects don't stutter the second time — rebuilt automatically if deleted | small |

> [!TIP]
> The `.img` files show a size of up to 2 GB each but are *sparse*: their
> **Size on disk** (right-click → Properties) should be about 0. If it is the
> full size — files made by an older build — close the game and delete them;
> they come back empty and sparse on the next start. When copying the game to a
> Steam Deck, leave `savegame\` out: a FAT32/exFAT stick would copy the full
> ~5 GB, and the Deck recreates it by itself.

> [!NOTE]
> Despite its name, `savegame\` does **not** hold your save — only data the game
> rebuilds by itself. You can delete it to free space (with the game closed);
> the first run after that loads a little slower while the game copies its
> files again.

---

## 11. Updating to a newer version

![Easy](https://img.shields.io/badge/difficulty-easy-brightgreen) &nbsp; ⏱️ 5–10 minutes

1. In the launcher: **Update → Update**. It downloads only when there is a newer
   version.
2. **Play → Build**. The code is translated again only if what it is made from
   changed.

Your game files, `jsrf.ini` and saves are kept.

---

## 12. Troubleshooting

| ❌ You see | ✅ What to do |
|---|---|
| `default.xbe not found` | The game folder must hold `default.xbe` directly (not in a subfolder). **Play → Choose folder** to pick the right one. |
| `this default.xbe's code is not a release the project knows` | Your copy is a release not supported yet — most likely a European or Asian Sega GT 2002 + JSRF disc. NTSC (North America), PAL (Europe), NTSC-J (Japan) and the US combo disc work. |
| `Could not reach GitHub` / a download fails | Check the internet connection and try again. |
| A build fails | **Open log file** (shown under the buttons after a failure), find the first line with `error`, and report it ([section 13](#13-reporting-a-problem)). |
| `Could not copy the game … is the game still running?` | Close the game and **Build** again. |
| The game is slow or stutters | Lower the resolution scale (**Settings → Video**); turn **Logging** off (**Settings → Debug**); on a laptop, plug it in. |
| Busy areas on the Deck (Proton) drop below 50 fps for a few seconds | You set `RECOMP_SLEEP_WAITS=all` without the performance governor — do both steps of [Mode 2](#-mode-2--power-saving), or go back to Mode 1. |
| Game opens up to a black screen | Add `RECOMP_GL_CPU_VERTEX=1` to `jsrf.ini` ([DEBUG.md](DEBUG.md)). There's a known bug on NVIDIA systems that causes this; a fix is in the works. |

---

## 13. Reporting a problem

![Easy](https://img.shields.io/badge/difficulty-easy-brightgreen)

A log lets someone see what happened.

1. In the launcher: **Settings → Debug → Logging**, then **Save**. On Windows
   this also puts `jsrf_recomp.pdb` next to the game, so the log can name the
   game's functions.
2. Play until the problem happens and close the game.
3. Send `stderr.txt` from the game folder. For a build problem, send
   `launcher_log.txt` from the launcher's folder.

Say what you did, what you expected and what happened instead, and whether it
was on a PC or a Steam Deck.

> [!TIP]
> Turn **Logging** off again afterwards — the log makes the game slower.

---

## 14. What's in the folders

**Next to the launcher:**

| | What it is |
|---|---|
| `jsrf/` | your game files (or the folder set in Play), with the built game, `jsrf.ini` and your saves |
| `source/` | the project, downloaded by Update |
| `tools/` | Python, Zig, CMake, Ninja (about 400 MB) |
| `build/` | the compiler's work files |
| `launcher.ini`, `launcher_log.txt` | the launcher's settings and log |

<details>
<summary>📁 Inside the project (<code>source/</code>)</summary>

| Folder | What it is |
|---|---|
| `jsrf_recomp/src/` | the project's own code (`main.c`, `recomp_manual.c`) and the generated code in `src/recomp/gen/` (made from your `default.xbe`, never uploaded) |
| `jsrf_recomp/analysis/` | the map of the game's program the translator works from: where each function starts and ends, names, calling conventions — addresses and names, no game code |
| `jsrf_recomp/tools/` | the translator's scripts and developer tools |
| `jsrf_recomp/cmake/zig.cmake` | the Zig compiler set-up |
| `jsrf_recomp/portable/` | the default `jsrf.ini` |
| `jsrf_recomp/README_JSRF.md` | the development history of every version, with measurements |
| `xboxrecomp/` | the toolkit: the translator (Python) and the runtime that recreates the Xbox (kernel, NV2A graphics on OpenGL, MCPX audio, controllers) |
</details>

---

## 15. Credits and licences

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
- Piaka for the borderless window tip.
- The quit window and the "Building Shaders" message use the
  [NiseJSRF](https://www.dafont.com/nisejsrf.font) font by Yoyo ("100% Free"),
  drawn with [stb_truetype](https://github.com/nothings/stb) (public domain).
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
