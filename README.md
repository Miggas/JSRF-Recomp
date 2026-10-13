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

1. Copy your game files into `jsrf_recomp/game/`.
2. Generate the recompiled code from your `default.xbe` into
   `jsrf_recomp/src/recomp/gen/` (a one-step script for this is still to come).
3. Run `jsrf_recomp/rebuild.bat` (Visual Studio 2022 with C++; CMake and
   Python 3 on the path).
4. `jsrf_recomp/build_portable.bat` puts `jsrf_recomp.exe` and `jsrf.ini` in the
   game folder.

Settings are in `jsrf.ini` — see `jsrf_recomp/jsrf.ini.example`. The history
of every version is in `jsrf_recomp/README_JSRF.md`.

## Licences

`xboxrecomp/` is MIT licensed (see `xboxrecomp/LICENSE`); some of its files
come from [xemu](https://github.com/xemu-project/xemu) under LGPL-2.1 — see
`xboxrecomp/NOTICE`. *Jet Set Radio Future* is © SEGA; this project is not
affiliated with or endorsed by SEGA or Microsoft.
