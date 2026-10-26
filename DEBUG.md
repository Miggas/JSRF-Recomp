# Jet Set Radio Future — debug and advanced settings

These settings aren't in `jsrf.ini` because most players never need them. To
use one, add it as a new line in `jsrf.ini` (`NAME=value`), or set it in the
`.bat` file if you play with Option B. Remove it again when you're done.

> [!WARNING]
> Most of these are for testing and finding problems. Some slow the game down
> or write very large logs. If something breaks, remove the line first.

Back to the main guide: [README.md](README.md#9-settings-jsrfini).

---

## Graphics

| Setting | What it does |
|---|---|
| `RECOMP_GL_CPU_VERTEX=1` | Process vertices on the CPU. Workaround for the black screen some NVIDIA systems get at start-up. |
| `RECOMP_RENDERER=sw` | Use the software renderer instead of OpenGL. Very slow; for comparing when the GPU renderer draws something wrong. |
| `RECOMP_GL_WDEPTH=0` | Turn off the distance-precise depth buffer and go back to the console's 24-bit depth. On by default, it stops far-away textures and shadows from flickering. `RECOMP_GL_WDEPTH_NEAR` (default `128`) sets the distance up to which the game's own depth is kept. *Idea by Miggas.* |
| `RECOMP_GL_EDGE=0` | Turn off the edge fix (full-screen layers pushed one pixel past the screen edges, so no line shows along the top and left). |
| `RECOMP_GL_ADDITIVE_DEPTH=1` | Let the map's glows and light beams write depth again, as the original does. Brings back the Skyscraper District bug where the chain-link floor disappears behind the light pillars. |
| `RECOMP_GL_MENU_DEPTH=0` | Turn off the menu depth fix (graffiti menu icons hidden behind their buttons). |
| `RECOMP_GL_NO_WINDOW_CLIP=1` | Ignore the game's window clipping (used by split screen). |

## Widescreen

| Setting | What it does |
|---|---|
| `RECOMP_ASPECT=w:h` | A specific screen shape instead of `RECOMP_WIDESCREEN`, for example `16:10` or `21:9`. |
| `RECOMP_WS_HUD=0` | Keep the HUD in the middle (4:3 positions). |
| `RECOMP_WS_MENU_HOLD43=1` | Show the menus 4:3 again, with black bars. |
| `RECOMP_WS_MENU_MAP=0` | Turn off the widescreen menu layouts (pieces moved to the screen edges or stretched). |

## Map and objects

| Setting | What it does |
|---|---|
| `RECOMP_DRAW_AREAS=<number>` | Instead of `hops`: add the map areas within this distance of the camera. |
| `RECOMP_DRAW_AREAS_PASSES=11` | Which of the map's four draw passes get the extra areas (bit mask: `1`, `2`, `4`, `8`; default `11`). |

## Audio

| Setting | What it does |
|---|---|
| `RECOMP_APU_REVERB=0` | Turn off the echo effect in some areas. |
| `RECOMP_SND_MUTE=…` | Sounds never played, as `bank:sound,bank:sound`. The default, `52:35`, mutes the Chuo Street signs' electric buzz that is heard all over the map. `none` plays everything. |
| `RECOMP_AUDIO_DEBUG=1` | Audio debugging: logs every voice, the music and dialogue streams, timing and slow file reads, and records the last 30–60 s of sound to `apu_capture_0.wav` / `apu_capture_1.wav`. Very large log. |
| `RECOMP_SND_DIST=1` | Log 3D sound distances (which sounds play or are cut off, and why). |
| `RECOMP_VBLANK_ACK=executor` | Testing only: the old vblank handling, which loses vblanks (music and dialogue crackle). |

## Input

| Setting | What it does |
|---|---|
| `RECOMP_SDL_PADS=0` | Turn off support for PlayStation, Switch Pro and other non-Xbox controllers (they work on Windows by default, rumble included). |
| `RECOMP_PAD_LABELS=1` | Map Nintendo pads by the letters printed on them instead of by position (by default the bottom button is A on every pad). |

## Files

| Setting | What it does |
|---|---|
| `RECOMP_SAVE_DIR=…` | Where the `savegame` folder (the cache and partition images) goes, if not in the game folder. Your save itself stays in `UDATA`. |

## Logs for finding problems

All of these need `RECOMP_LOG=stderr.txt`. Put a `;` in front of
`RECOMP_QUIET=1` while using them.

| Setting | What it does |
|---|---|
| `RECOMP_MENU_LOG=1` | Press **F9** to log the next 1500 draws: what each one is, where it is on screen, its depth and blend state, and which game functions drew it. Used to make the menus widescreen. |
| `RECOMP_GL_DRAWLOG=1` | Only the F9 draw list, without the call chains. `RECOMP_GL_DRAWLOG_N` sets how many draws (default `1500`). |
| `RECOMP_DRAW_WHO=1` | Only the call chains of every new draw (F9 marks a point in the log). |
| `RECOMP_WS_LOG=1` | Log widescreen decisions: which full-width layers are kept full width and which are squeezed. |
| `RECOMP_WS_MENU_DEBUG=1` | Log each menu piece the widescreen layouts move, with its call site. |
