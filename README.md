<div align="center">

# Jet Set Radio Future — PC recompilation

**The original Xbox game, translated into a native Windows program.**<br>
Runs on Windows 10/11, and on the Steam Deck through Proton 11.

![Windows](https://img.shields.io/badge/Windows-10%20%7C%2011-0078D6?logo=windows&logoColor=white)
![Steam Deck](https://img.shields.io/badge/Steam%20Deck-Proton%2011-1A9FFF?logo=steamdeck&logoColor=white)
![Version](https://img.shields.io/badge/game-NTSC%20(US)%20retail%20only-orange)
![Bring your own game](https://img.shields.io/badge/game%20files-not%20included-lightgrey)

</div>

This is **not an emulator**. The game's own program is translated, once, into C
code on your computer, and that code is compiled into a normal Windows `.exe`.
The Xbox's hardware — graphics, sound, controllers, memory card — is recreated
by the runtime in `xboxrecomp/`.

> [!NOTE]
> **Made with AI.** Most of this project's code was written by Claude
> (Anthropic's AI), directed, tested and played through by Miggas. See
> [how this was made](#how-this-was-made).

> [!IMPORTANT]
> **You need your own copy of the game.** Nothing from the game is in this
> repository: no code, no graphics, no music. The translation is made on your
> computer, from your copy.

### How hard is each step?

Each section is marked with how much effort it takes:

| Badge | Meaning |
|---|---|
| ![Easy](https://img.shields.io/badge/difficulty-easy-brightgreen) | Double-click or copy a file. Anyone can do it. |
| ![Medium](https://img.shields.io/badge/difficulty-medium-yellow) | A few more steps, or installing something. Follow the instructions closely. |
| ![Hard](https://img.shields.io/badge/difficulty-hard-red) | Typing commands, or system settings. Optional — there is always an easier way. |

---

## Contents

| | Section | Difficulty |
|---|---|---|
| 1 | [What you need](#1-what-you-need) | — |
| 2 | [Install the tools (once)](#2-install-the-tools-once) | ![Medium](https://img.shields.io/badge/-medium-yellow) |
| 3 | [Get this project](#3-get-this-project) | ![Easy](https://img.shields.io/badge/-easy-brightgreen) |
| 4 | [Add your game files](#4-add-your-game-files) | ![Easy](https://img.shields.io/badge/-easy-brightgreen) |
| 5 | [Build the game](#5-build-the-game) | ![Easy](https://img.shields.io/badge/-easy-brightgreen) |
| 6 | [Play — Option A or Option B](#6-play) | ![Easy](https://img.shields.io/badge/-easy-brightgreen) |
| 7 | [Playing on a Steam Deck](#7-playing-on-a-steam-deck) | ![Medium](https://img.shields.io/badge/-medium-yellow) |
| 8 | [Controls](#8-controls) | — |
| 9 | [Settings (jsrf.ini)](#9-settings-jsrfini) | ![Easy](https://img.shields.io/badge/-easy-brightgreen) |
| 10 | [Saves](#10-saves) | ![Easy](https://img.shields.io/badge/-easy-brightgreen) |
| 11 | [Updating to a newer version](#11-updating-to-a-newer-version) | ![Easy](https://img.shields.io/badge/-easy-brightgreen) |
| 12 | [Troubleshooting](#12-troubleshooting) | — |
| 13 | [Reporting a problem](#13-reporting-a-problem) | ![Easy](https://img.shields.io/badge/-easy-brightgreen) |
| 14 | [What's in the folders](#14-whats-in-the-folders) | — |
| 15 | [Credits and licences](#15-credits-and-licences) | — |

---

## 1. What you need

| | Requirement |
|---|---|
| 💻 | **A Windows 10 or 11 PC** to build the game. Once built, it also runs on a Steam Deck with **Proton 11** (building on the Deck itself is not supported). |
| 💿 | **Your copy of Jet Set Radio Future for the original Xbox — the NTSC (North American) retail release**, as a disc image (`.iso`) or as its files (`default.xbe` plus the folders next to it). |
| 💾 | **About 5 GB of free disk space** (mostly Visual Studio). |
| 🎮 | **A graphics card with OpenGL 3.3** or newer — practically any GPU from the last ten years. |

> [!WARNING]
> **Only the NTSC (North American) release works.** The PAL (European) and
> NTSC-J (Japanese) releases are built differently inside. The build checks the
> game's code and data inside your `default.xbe` and stops if they are not the
> right ones. It does **not** compare the whole file: many dumps have a patched
> header (region, allowed media), which changes the file's SHA-256 but not the
> game, and those work fine.
>
> To check your copy before building, open a terminal in `jsrf_recomp` and run
> `py -3 tools\xbe_info.py` (or `py -3 tools\xbe_info.py path\to\default.xbe`).
> It says whether the code matches and, if not, which part differs.

---

## 2. Install the tools (once)

![Medium](https://img.shields.io/badge/difficulty-medium-yellow) &nbsp; ⏱️ 15–30 minutes, mostly waiting

You need **two free programs**. A third piece, *capstone*, installs itself.
Install them in this order.

### Step 1 — Visual Studio 2022 Community (the C compiler)

1. Download it from <https://visualstudio.microsoft.com/vs/community/>.
2. Run the installer. When it asks what to install, tick
   **☑ Desktop development with C++**. Leave the options on the right as they
   are; they include CMake, which the build also needs.
3. Click **Install** and wait (it is large).

> [!TIP]
> You never need to open Visual Studio afterwards — the build scripts use it on
> their own.

### Step 2 — Python 3 (runs the translator)

1. Download the latest Python 3 from <https://www.python.org/downloads/>.
2. Run the installer and, **on the first screen, tick ☑ Add python.exe to PATH**,
   then click **Install Now**.

> [!CAUTION]
> If you forget to tick **Add python.exe to PATH**, the scripts cannot find
> Python. Run the installer again, choose *Modify*, and tick it.

### Step 3 — capstone ![Easy](https://img.shields.io/badge/-automatic-brightgreen)

Nothing to do: `generate_code.bat` installs it the first time it runs (it needs
an internet connection then).

<details>
<summary>Install it yourself instead (optional)</summary>

Open **Command Prompt** and type:

```
py -3 -m pip install capstone==5.0.7
```
</details>

---

## 3. Get this project

![Easy](https://img.shields.io/badge/difficulty-easy-brightgreen) &nbsp; ⏱️ 1 minute

Pick one:

- **Download the ZIP** — on the GitHub page, click the green **Code** button →
  **Download ZIP**, then unzip it somewhere with a short path, for example
  `C:\JSRF\`.
- **With git** — `git clone <this repository's URL> C:\JSRF`

You should end up with:

```
C:\JSRF\
├── xboxrecomp\      the runtime and the translator
├── jsrf_recomp\     the game project — you work in here
└── README.md        this file
```

> [!NOTE]
> Avoid folders that are synced (OneDrive, Dropbox) or have very long paths;
> the build creates many files.

---

## 4. Add your game files

![Easy](https://img.shields.io/badge/difficulty-easy-brightgreen) &nbsp; ⏱️ a few minutes

Copy **all** of the game's files into `jsrf_recomp\game\` — `default.xbe` and
every folder next to it — so that this file exists:

```
C:\JSRF\jsrf_recomp\game\default.xbe
```

<details>
<summary>💿 I only have a disc image (.iso) &nbsp; <img src="https://img.shields.io/badge/-medium-yellow"></summary>

The toolkit can unpack it. Open **Command Prompt** and type:

```
cd C:\JSRF\xboxrecomp
py -3 -m tools.xiso unpack "C:\path\to\your\game.iso" -o ..\jsrf_recomp\game
```
</details>

---

## 5. Build the game

![Easy](https://img.shields.io/badge/difficulty-easy-brightgreen) &nbsp; ⏱️ 10–20 minutes the first time

Open the `C:\JSRF\jsrf_recomp\` folder in Explorer and double-click, **in order**:

| Step | Double-click | What it does | How long |
|:---:|---|---|---|
| **1** | `generate_code.bat` | Checks your `default.xbe`, then translates the game's program into C code (in `src\recomp\gen\`). | 2–3 min |
| **2** | `rebuild.bat` | Compiles everything into `jsrf_recomp.exe`. The first time it also sets up the build folder. | 5–15 min the first time, faster after |

Each window ends with ✅ **Done** / **Build OK** when it worked, or
❌ **FAILED — scroll up for the first error** when it didn't
(see [Troubleshooting](#12-troubleshooting)).

> [!TIP]
> You only need `generate_code.bat` again when you update the project
> ([section 11](#11-updating-to-a-newer-version)). `rebuild.bat` is what you run
> after any change.

---

## 6. Play

There are two ways to play. **Pick one.**

<table>
<tr>
<th width="50%">🅰️ &nbsp;Option A — Ready-to-play game folder</th>
<th width="50%">🅱️ &nbsp;Option B — Straight from the project</th>
</tr>
<tr>
<td valign="top">

![Recommended](https://img.shields.io/badge/-recommended-brightgreen) ![Easy](https://img.shields.io/badge/-easy-brightgreen)

Double-click **`build_portable.bat`**. It builds the game and copies
`jsrf_recomp.exe` and a settings file, `jsrf.ini`, into your game folder
(`jsrf_recomp\game\`).

From then on, just double-click **`jsrf_recomp.exe` in the game folder**. You
can make a desktop shortcut to it, or copy the whole game folder to another PC
or a Steam Deck.

**Best for:** everyday playing, and the Steam Deck.

</td>
<td valign="top">

![Easy](https://img.shields.io/badge/-easy-brightgreen)

Double-click **`play_jsrf.bat`** in `jsrf_recomp\`.

The game runs from the build folder; its settings are inside the `.bat` file
instead of `jsrf.ini`.

**Best for:** quick tests after a rebuild, without copying anything.

</td>
</tr>
</table>

> [!TIP]
> **`F11`** or **`Alt+Enter`** switches between window and full screen at any time.<br>
> To put the game in a different folder with Option A: `build_portable.bat D:\Games\JSRF`

---

## 7. Playing on a Steam Deck

![Medium](https://img.shields.io/badge/difficulty-medium-yellow) &nbsp; ⏱️ 10 minutes

1. On your PC, do **[Option A](#6-play)**. Your game folder now holds the game
   files, `jsrf_recomp.exe` and `jsrf.ini`.
2. Copy that whole folder to the Deck (USB stick, SD card, or over the
   network), for example to `/home/deck/Games/JSRF`.
3. On the Deck, switch to **Desktop Mode**, open Steam, and choose
   **Games → Add a Non-Steam Game to My Library… → Browse…**. Set the file type
   to *All Files*, pick `jsrf_recomp.exe`, and add it.
4. In Steam, right-click the new entry → **Properties → Compatibility** → tick
   **Force the use of a specific Steam Play compatibility tool** and choose
   **Proton 11**.
5. Go back to **Gaming Mode** and play.

> [!WARNING]
> **Tested with Proton 11 only.** Earlier Proton versions have not been tested,
> and there are no plans to support them. If something goes wrong on an older
> version, switch to Proton 11 first.

> [!NOTE]
> The provided `jsrf.ini` already has the settings that suit the Deck:
> `RECOMP_GL_SCALE=2` (1280×960 inside the game) and `RECOMP_GL_TEX_WATCH=0`
> (avoids stutter).

### Choose a power mode

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

#### 🔋 Mode 1 — Standard

![Easy](https://img.shields.io/badge/difficulty-nothing%20to%20do-brightgreen)

Leave `jsrf.ini` as it is. The line `;RECOMP_SLEEP_WAITS=vblank` stays switched
off (it starts with `;`), so the default is used: only one small wait sleeps —
the one that avoids music crackling — and the rest spin.

#### 🌿 Mode 2 — Power saving

![Medium](https://img.shields.io/badge/difficulty-medium-yellow) &nbsp; ⏱️ 15 minutes the first time

> [!IMPORTANT]
> Do **both** steps. With only Step 1, busy areas drop under 50 fps for a few
> seconds whenever the CPU has to speed up.

**Step 1 — let the game sleep while it waits** ![Easy](https://img.shields.io/badge/-easy-brightgreen)

Open `jsrf.ini` (in the game folder) with a text editor and change this line:

```diff
- ;RECOMP_SLEEP_WAITS=vblank
+ RECOMP_SLEEP_WAITS=all
```

That is: remove the `;` and change `vblank` to `all`. Save the file.

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

> [!NOTE]
> The game window must have the focus for keys to count.

---

## 9. Settings (jsrf.ini)

![Easy](https://img.shields.io/badge/difficulty-easy-brightgreen)

Settings live in `jsrf.ini`, next to `jsrf_recomp.exe`. Open it with Notepad.
Each setting is one line, `NAME=value`. A line starting with `;` is switched
off — delete the `;` to switch it on. The full list with explanations is in
`jsrf_recomp\jsrf.ini.example`. The ones you are most likely to want:

| Setting | What it does |
|---|---|
| `RECOMP_GL_SCALE=2` | Internal resolution: `1` = 640×480 (original), `2` = 1280×960, `3` = 1920×1440, `4` = 2560×1920. Higher is sharper and needs a stronger GPU. |
| `RECOMP_GL_ANISO=16` | **Anisotropic filtering** (`2`, `4`, `8` or `16`): sharper ground and walls seen at an angle. Off when not set, as on the Xbox. |
| `RECOMP_FULLSCREEN=1` | Start in full screen. |
| `RECOMP_WIDESCREEN=1` | **Widescreen** 16:9: more of the scene to the left and right instead of black bars; the HUD moves out to the screen edges, and the garage menus, map, character select, VS setup and graffiti select are shown 4:3 while open. `auto` uses your monitor's shape (16:10 on a Steam Deck), or set any shape with `RECOMP_ASPECT=21:9`. *HUD placement and the 4:3 menus from Piaka's widescreen work.* |
| `RECOMP_SKIP_INTRO=1` | Skip the logos and the graffiti warning at start-up, straight to the title. *By Piaka.* |
| `RECOMP_SAVE_STRICT=1` | Refuse save files whose signature does not match, as the Xbox does. By default the recomp accepts them, so saves from a real Xbox, another emulator or a save editor load instead of showing as damaged. |
| `RECOMP_GL_PRESENT=dxgi` | **Borderless window** (Windows, NVIDIA or AMD GPU): with full screen on, Alt+Tab is instant and overlays work, with no black flash. Needs a strong PC — switch it off again if frame times get worse. Leave it off on the Steam Deck. *Tip from Piaka.* |
| `RECOMP_GL_VSYNC=1` | Wait for the screen's refresh (no tearing). |
| `RECOMP_USB_PADS=2` | How many players (1–4, default 1). With 1, every controller and the keyboard control player 1. With 2 or more, for the **multiplayer modes**, players join in the order they press a button: the first controller or keyboard to press one is player 1, the next player 2, and so on. |
| `RECOMP_RUMBLE=50` | Controller rumble strength in percent (default 100); `0` turns it off. |
| `RECOMP_SDL_PADS=0` | **PlayStation and other controllers** (DualSense, DualShock 4, Switch Pro and others XInput does not see) work on Windows out of the box, rumble included, alongside Xbox pads and the keyboard. This setting turns that off. Buttons go by position (the bottom one is A on every pad); `RECOMP_PAD_LABELS=1` uses the letters printed on Nintendo pads instead. |
| `RECOMP_APU_GAIN=0.5` | Master volume. |
| `RECOMP_APU_REVERB=0` | Turns off the echo effect in some areas. |
| `RECOMP_SAVE_DIR=…` | Where the `savegame` folder (the cache and partition images) goes, if not in the game folder. Your save itself stays in `UDATA`. |
| `RECOMP_SLEEP_WAITS=…` | Steam Deck power mode — see [section 7](#choose-a-power-mode). |
| `RECOMP_QUIET=1` | Leave this on: it turns off diagnostics that cost speed. |

> [!CAUTION]
> **Do not change** the lines in the `[game]` section of `jsrf.ini` — the game
> needs them to start.

If you play with **Option B** (`play_jsrf.bat`), its settings are inside the
`.bat` file instead (same names).

---

## 10. Saves

![Easy](https://img.shields.io/badge/difficulty-easy-brightgreen)

Your save is in the **`UDATA`** folder of your game folder (for example
`jsrf_recomp\game\UDATA\`), next to `TDATA`, which holds the game's own
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

1. Get the new version of the project (download the ZIP again, or `git pull`).
   If you download a ZIP, unzip it **into a new folder** and move your `game`
   folder across — don't copy the old `build` folder.
2. Double-click **`generate_code.bat`**, then **`rebuild.bat`** — or
   **`build_portable.bat`** if you play with Option A.

Your `jsrf.ini` and saves are kept.

---

## 12. Troubleshooting

| ❌ You see | ✅ What to do |
|---|---|
| `Python 3 not found` | Install Python ([section 2](#step-2--python-3-runs-the-translator)) with **Add python.exe to PATH** ticked. Close and reopen the window afterwards. |
| `could not install capstone` | Check the internet connection and run `generate_code.bat` again, or run `py -3 -m pip install capstone==5.0.7` in Command Prompt. |
| `default.xbe not found` | The game files must be in `jsrf_recomp\game\`, with `default.xbe` directly in it (not in a subfolder). |
| `this default.xbe is not the one the analysis was made from` | Your copy is a different version — most likely the PAL (European) or NTSC-J (Japanese) release. Only the NTSC (North American) retail release works. |
| `No recompiled code yet in src\recomp\gen` | Run `generate_code.bat` before `rebuild.bat`. |
| `cmake` not found, or `Visual Studio 17 2022` not found | Install Visual Studio 2022 with **Desktop development with C++** ([section 2](#2-install-the-tools-once)). |
| `MSB1009: Project file does not exist` (`ALL_BUILD.vcxproj`) | An earlier setup failed halfway. Delete the `jsrf_recomp\build` folder and run `rebuild.bat` again (newer versions do this for you); if it fails again, the real error is right after **Configuring...** |
| CMake says the cache or source folder doesn't match | You moved or copied the project. Delete the `jsrf_recomp\build` folder and run `rebuild.bat` again. |
| `BUILD FAILED` with something else | Scroll up to the first line that says `error`, and report it ([section 13](#13-reporting-a-problem)). |
| `Could not copy the exe … is the game still running?` | Close the game and run `build_portable.bat` again. |
| The game is slow or stutters | Lower `RECOMP_GL_SCALE`; make sure `RECOMP_QUIET=1` is on; on a laptop, plug it in. |
| Problems on the Steam Deck (or under Proton/Linux) with a Proton version other than 11 | Switch to **Proton 11** ([section 7](#7-playing-on-a-steam-deck)). Only Proton 11 is tested; older versions are not supported. |
| Busy areas on the Deck drop below 50 fps for a few seconds | You set `RECOMP_SLEEP_WAITS=all` without the performance governor — do both steps of [Mode 2](#-mode-2--power-saving), or go back to Mode 1. |

---

## 13. Reporting a problem

![Easy](https://img.shields.io/badge/difficulty-easy-brightgreen)

A log lets someone see what happened.

| If you play with… | Do this |
|---|---|
| 🅰️ **Option A** (exe in the game folder) | In `jsrf.ini`, remove the `;` in front of `RECOMP_LOG=stderr.txt`, and put a `;` in front of `RECOMP_QUIET=1`. Play until the problem happens, close the game, and send `stderr.txt` from the game folder. |
| 🅱️ **Option B** (from the project) | Double-click `run_jsrf.bat` instead of `play_jsrf.bat`; it writes `stderr.txt` in `jsrf_recomp\`. |

Say what you did, what you expected and what happened instead, and whether it
was on a PC or a Steam Deck.

> [!TIP]
> Put the `;` back in front of `RECOMP_LOG` afterwards — the log makes the game
> slower.

---

## 14. What's in the folders

**`jsrf_recomp\` — the scripts you use:**

| File | Use it to |
|---|---|
| `generate_code.bat` | translate your `default.xbe` into C (after getting or updating the project) |
| `rebuild.bat` | build the game |
| `build_portable.bat` | build and put the exe + `jsrf.ini` in the game folder (**Option A**) |
| `play_jsrf.bat` | play from the project (**Option B**) |
| `run_jsrf.bat` | play with a full log, for reporting a problem |
| `debug\` | tools for diagnosing freezes and graphics problems (developers) |

<details>
<summary>📁 The rest of the folders</summary>

| Folder | What it is |
|---|---|
| `jsrf_recomp\game\` | your game files go here (never uploaded) |
| `jsrf_recomp\src\` | the project's own code (`main.c`, `recomp_manual.c`) and, after step 5, the generated code in `src\recomp\gen\` (never uploaded) |
| `jsrf_recomp\analysis\` | the map of the game's program the translator works from: where each function starts and ends, names, calling conventions — addresses and names, no game code |
| `jsrf_recomp\tools\` | the scripts behind the `.bat` files, and developer tools |
| `jsrf_recomp\portable\` | the default `jsrf.ini` that `build_portable.bat` copies |
| `jsrf_recomp\README_JSRF.md` | the development history of every version, with measurements |
| `xboxrecomp\` | the toolkit: the translator (Python) and the runtime that recreates the Xbox (kernel, NV2A graphics on OpenGL, MCPX audio, controllers) |
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
- This project's own files (everything outside `xboxrecomp/`) are MIT
  licensed — see [`LICENSE`](LICENSE).
- `xboxrecomp/` is MIT licensed (see `xboxrecomp/LICENSE`). Some of its audio
  files come from [xemu](https://github.com/xemu-project/xemu) under
  LGPL-2.1 — see `xboxrecomp/NOTICE` for the full list and their authors.
- *Jet Set Radio Future* is © SEGA. Xbox is a trademark of Microsoft. This
  project is not affiliated with or endorsed by SEGA or Microsoft, and includes
  none of their code or content.
