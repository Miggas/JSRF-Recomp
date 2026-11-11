# JSRF Mod Tool

Shows the textures in Jet Set Radio Future's game files, exports them to PNG and puts edited PNGs back, for the [JSRF recomp](https://github.com/Miggas/JSRF-Recomp). Windows and Linux (Steam Deck included).

The game's files are never changed. An edited file is saved in `mods/` in the game folder, at the game's own path (`mods/Media/Player/Beat.dat`), and the recomp (v302 and later) opens it instead of the original. To undo a mod, delete its file, or use **Undo all (file)** in the tool.

## Use

1. **Choose...** the game folder: the one with `default.xbe` and `Media`. The tool looks through `Media` and lists every file with textures.
2. Pick a file, then one of its textures.
3. **Export PNG** saves it to `texture_edits/` in the game folder (**Open edits folder** opens it).
4. Edit the PNG in any image editor and save it under the same name. Transparency is kept.
5. **Import PNG** puts it back. You can also drop any PNG or TGA on the window to import it into the picked texture. An image of another size is resized to the texture's.

What it reads: characters, objects, stages, HUD sprites, fonts and logos (`.dat`), and graffiti (`.grf`, `.prs`, `.JTX`). Textures in DXT1, DXT3 and DXT5 are written back in the same format, with their smaller mip levels made again from the new image.

## Build

- Windows: `build_modtool.bat`
- Linux: `./build_modtool.sh` (needs SDL2, which SteamOS and desktop Linux have)

Both download Zig (the compiler) into `deps/` the first time, the same way the launcher does.

## Credits

The file formats were learned from [JSRF ModTool](https://github.com/neodoso/JSRF_ModTool) by neodoso and his [jsrf-inside](https://jsrf-inside.blogspot.com) blog. No code was taken from it: this tool is written from scratch in C. Idea by Miggas.
