# JSRF Mod Tool

Shows the textures in Jet Set Radio Future's game files, exports them to PNG and puts edited PNGs back, for the [JSRF recomp](https://github.com/Miggas/JSRF-Recomp). Windows and Linux (Steam Deck included).

An edited file is changed in place in the game folder. Before its first change, the original is kept in `mod_backups/` at the game's own path (`mod_backups/Media/Player/Beat.dat`), and **Undo all (file)** puts it back. The game keeps copies of most of its files in its cache (`savegame/Cache` in the game folder) and reads those after the first time: the tool changes that copy too when there is one. Nothing is needed from the recomp.

## Use

1. **Choose...** the game folder: the one with `default.xbe` and `Media`. The tool looks through `Media` and lists every file with textures, in tabs: Characters (by their names in the game), Graffiti (in sections SS, S, M, L and XL: `grf_0_` to `grf_4_`), Stages, Objects, Cutscenes, Menus & HUD, Missions and Other, each in collapsible sections.
2. Pick a file, then one of its textures.
3. **Export PNG** saves it to `texture_edits/` in the game folder (**Open edits folder** opens it).
4. Edit the PNG in any image editor and save it under the same name. Transparency is kept.
5. **Import PNG** puts it back. You can also drop any PNG or TGA on the window to import it into the picked texture.

Sizes: an image the texture's own size goes in as it is. A texture in a `.dat` can be made bigger: import a square image of 512, 1024, 2048 or 4096 pixels (larger than the texture) and the file makes room for it; the game reads the new size from the texture itself. Any other size is resized to the texture's. Graffiti (`.grf`, `.prs`, `.JTX`) always keep their size.

What it reads: characters, objects, stages, HUD sprites, fonts and logos (`.dat`), and graffiti (`.grf`, `.prs`, `.JTX`). Textures in DXT1, DXT3 and DXT5 are written back in the same format, with their smaller mip levels made again from the new image.

## Build

- Windows: `build_modtool.bat`
- Linux: `./build_modtool.sh` (needs SDL2, which SteamOS and desktop Linux have)

Both download Zig (the compiler) into `deps/` the first time, the same way the launcher does.

## Credits

The file formats were learned from [JSRF ModTool](https://github.com/neodoso/JSRF_ModTool) by neodoso and his [jsrf-inside](https://jsrf-inside.blogspot.com) blog. No code was taken from it: this tool is written from scratch in C. Idea by Miggas.
