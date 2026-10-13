# Jet Set Radio Future — static recompilation

A static recompilation of *Jet Set Radio Future* (Xbox, 2002) into a native
Windows executable, built on the [xboxrecomp](xboxrecomp/README.md) toolkit.
Runs on Windows and on the Steam Deck through Proton.

**No game content is in this repository.** You need your own copy of the game
(`default.xbe` and its data files). The recompiled C code is generated from
your `default.xbe` on your machine and is never committed.

## Layout

| Folder | What it is |
|---|---|
| `xboxrecomp/` | the toolkit: the recompiler (Python) and the runtime (Xbox kernel, NV2A GPU on OpenGL, MCPX audio, input) |
| `jsrf_recomp/` | the game project: hand-written code, analysis data (function addresses and names), tools and build scripts |

## Building

You need Visual Studio 2022 (C++ desktop workload), CMake, and Python 3 with
`capstone` (`py -3 -m pip install capstone==5.0.7`).

1. Copy your game files (`default.xbe` and the rest) into `jsrf_recomp/game/`.
2. Run `jsrf_recomp/generate_code.bat`. It translates your `default.xbe` into
   C in `jsrf_recomp/src/recomp/gen/` (about two minutes). It checks first that
   the xbe is the retail version the project's analysis was made for.
3. Run `jsrf_recomp/rebuild.bat`.
4. `jsrf_recomp/play_jsrf.bat` to play, or `jsrf_recomp/build_portable.bat` to
   put `jsrf_recomp.exe` and `jsrf.ini` in the game folder.

Settings are in `jsrf.ini` — see `jsrf_recomp/jsrf.ini.example`. The history
of every version is in `jsrf_recomp/README_JSRF.md`.

## Licences

`xboxrecomp/` is MIT licensed (see `xboxrecomp/LICENSE`); some of its files
come from [xemu](https://github.com/xemu-project/xemu) under LGPL-2.1 — see
`xboxrecomp/NOTICE`. *Jet Set Radio Future* is © SEGA; this project is not
affiliated with or endorsed by SEGA or Microsoft.
