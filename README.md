# JSRF launcher

The launcher for the [Jet Set Radio Future recompilation](https://github.com/Miggas/JSRF-Recomp):
downloads the project, builds the game from your `default.xbe`, updates it,
edits `jsrf.ini` and starts the game. Windows and Linux (Steam Deck included).
Nothing has to be installed: it downloads its own tools.

This branch is the launcher's code only. To play, download the launcher from
the [Releases](https://github.com/Miggas/JSRF-Recomp/releases) page and see the
README on the main branch.

## How it decides what to do

- **Update** only downloads the project (nothing when GitHub says it is
  already the newest). The generated code is kept.
- **Generate code** runs only when what it is made from changed: `default.xbe`,
  `jsrf_recomp/analysis`, `jsrf_recomp/tools`, `recomp_manual.c`,
  `xboxrecomp/tools`.
- **Build** runs only when the source, the generated code or the compiler
  changed. It asks for Generate code first when the code is missing or old.
- **Play → Build** does both. **Even if nothing changed** forces them.

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
| `src/main.c` | the window: Play, Settings, Build, Update |
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
| `game/` | your game files (or the folder set in Play), and the built game |
| `source/` | the project, downloaded by Update |
| `tools/` | Python, Zig, CMake, Ninja (about 400 MB) |
| `build/` | the compiler's work files |
| `launcher_log.txt` | the log of the last run (the one before: `launcher_log.txt.old`) |
| `launcher.ini` | the launcher's settings: `BRANCH=test` makes Update download the `test` branch (missing = `main`) |
