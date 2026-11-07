# JSRF launcher

The launcher for the [Jet Set Radio Future recompilation](https://github.com/Miggas/JSRF-Recomp):
downloads the project, builds the game from your `default.xbe`, updates it,
edits `jsrf.ini` and starts the game. Windows and Linux (Steam Deck included).
Nothing has to be installed: it downloads its own tools.

This branch is the launcher's code only. To play, download the launcher from
the [Releases](https://github.com/Miggas/JSRF-Recomp/releases) page and see the
README on the main branch.

## Controller

| Button | Does |
|---|---|
| D-pad / left stick | Moves between buttons (the highlighted one has a yellow frame) |
| A | Presses the highlighted button, opens a list, picks from it |
| B | Closes an open list |
| Left / right on a slider | Changes its value |
| Right stick | Scrolls |
| LB / RB | Previous / next tab |
| Start | PLAY |

In the settings lists, up / down go through the whole list and it scrolls along; up from the top of a list goes back to the tabs.

The launcher closes once the game has started (can be turned off in the
**Launcher** tab).

## The Launcher tab

- **Close the launcher when the game starts** (on by default).
- **Project branch**: `main` (tested releases) or `test` (newest, less tested).
- **Debug**: shows the Debug tab and **Settings → All settings**.

Generate code, Build and Update show a progress bar.

## Updating the launcher

When it opens, the launcher checks for a newer game and a newer launcher; if
there is one, the **Play** tab says so, and clicking that line opens **Update**.
PLAY and Build can be used while it checks.

**Update → Check for updates** also looks for a newer launcher on the
[Releases](https://github.com/Miggas/JSRF-Recomp/releases) page; **Update launcher**
downloads it, puts it in place of this one and starts it. From a terminal:
`jsrf_launcher --self-update`.

## How it decides what to do

- **Update** only downloads the project (nothing when GitHub says it is
  already the newest). The generated code is kept.
- **Generate code** runs only when what it is made from changed: `default.xbe`,
  `jsrf_recomp/analysis`, `jsrf_recomp/tools`, `recomp_manual.c`,
  `xboxrecomp/tools`.
- **Build** runs only when the source, the generated code or the compiler
  changed. It asks for Generate code first when the code is missing or old.
- **Play → Build** does both, staying on Play. **Force Generate/Build** (Debug tab) forces them.

## The Debug tab

**Launcher → Debug** shows it: Force Generate/Build, Download
tools, the checks (generated code, `default.xbe`, the
hard-disk cache), Play stopping after 30 s with a dump, Sample a hang, Trace
one frame, and the whole log.

## Building it

| | |
|---|---|
| Windows 10 / 11 | double-click `build_launcher.bat` → `jsrf_launcher.exe` |
| Linux / Steam Deck | `bash build_launcher.sh` → `jsrf_launcher` |

Nothing else is needed. The first time, the script downloads Zig (the C
compiler, about 80 MB) and SDL2 into `deps/`. On Linux the launcher uses the
system's SDL2 (SteamOS has it).

## Files

| | |
|---|---|
| `src/main.c` | the window: Play, Settings, Build, Update, Launcher |
| `src/jobs.c` | setting up the tools, building, updating, starting the game |
| `src/ini.c` | reading and saving `jsrf.ini`, keeping its comments |
| `src/util.c` | files, the log, running programs |
| `src/version.h` | the launcher's version |
| `third_party/nuklear` | [Nuklear](https://github.com/Immediate-Mode-UI/Nuklear), draws the window (public domain / MIT) |
| `third_party/sdl2_linux` | SDL2's configuration header for Linux (zlib) |

## Folders it makes

Next to the launcher:

| | |
|---|---|
| `jsrf/` | your game files (or the folder set in Play), and the built game; made at the first start |
| `source/` | the project, downloaded by Update |
| `tools/` | Python, Zig, CMake, Ninja (about 400 MB) |
| `build/` | the compiler's work files |
| `launcher_log.txt` | the log of the last run (the one before: `launcher_log.txt.old`) |
| `launcher.ini` | the Launcher tab's settings: `BRANCH`, `DEBUG_MENU`, `CLOSE_ON_PLAY` |
