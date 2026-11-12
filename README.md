# JSRF Mod Tool

Edits Jet Set Radio Future's game files for the [JSRF recomp](https://github.com/Miggas/JSRF-Recomp): textures (export, import, paint on them or on the 3D model), models (export to and import from Blender), materials and sounds. Windows and Linux (Steam Deck included).

An edited file is changed in place in the game folder. Before its first change, the original is kept in `mod_backups/` at the game's own path (`mod_backups/Media/Player/Beat.dat`), and **Undo all (file)** puts it back. The game keeps copies of most of its files in its cache (`savegame/Cache` in the game folder) and reads those after the first time: the tool changes that copy too when there is one. Nothing is needed from the recomp.

## Use

1. **Choose...** the game folder: the one with `default.xbe` and `Media`. Put the tool in the JSRF launcher's folder and it uses the launcher's game folder by itself (the one set in Play, else `jsrf/` beside the launcher). The tool looks through `Media` and lists every file with textures, in tabs: Characters (by their names in the game), Graffiti (in sections SS, S, M, L and XL: `grf_0_` to `grf_4_`), Stages, Objects, Cutscenes, Menus & HUD, each in collapsible sections.
2. Pick a file, then one of its textures.
3. **Export PNG** saves it to `texture_edits/` in the game folder (**Open edits folder** opens it).
4. Edit the PNG in any image editor and save it under the same name. Transparency is kept.
5. **Import PNG** puts it back. You can also drop any PNG or TGA on the window to import it into the picked texture.

**Paint** opens the picked texture in the painter: Brush, Eraser (back to the original), Picker (a colour from the texture) and Fill, with size, hardness, opacity and fill tolerance, and the colour chooser. The mouse wheel zooms, a right drag moves the view, Undo / Redo (Ctrl+Z / Ctrl+Y) go back and forth. **Resize** makes the painting 2, 4 or 8 times bigger by integer scaling (each pixel a block, nothing blurred), so you can add finer detail; a size over 4096 is greyed out, and graffiti keep their size. You paint on a full-quality copy: **Save** compresses it into the game file once, the same way an import does.

**3D view**, on the right, shows the picked file's models that wear the picked texture, and follows the painting as you paint. Drag to turn the model, right drag to move it, the wheel zooms. **All** lists every model in the file, Reset puts the camera back. **Wires** shows the model's triangles, on the model and over the texture (in the painter too), so you can see which part of the texture lands where. Menus and HUD files don't say which model wears which texture, so there the view stays empty unless All is ticked.

**Paint** in the 3D view paints straight on the model, with the painter's tool, colour and size (ticking it opens the painter if it isn't open). The brush's circle shows its size on the model where the cursor is. A stroke that crosses onto another part of the texture carries on there, without a line through the texture between. The picker and fill work on the model too. Middle drag turns the model while Paint is on, right drag moves it, the wheel zooms. Undo, redo and Save are the painter's: it is one painting, whichever side you paint on.

**Export** in the 3D view saves the model shown in `model_exports/` in the game folder, in two forms: a Wavefront `.obj` (with its `.mtl` and its texture as a PNG: the painting, while painting it) and a Valve `.smd` with its skeleton and bone weights. Open the `.obj` in Blender with File > Import > Wavefront; the `.smd` needs Blender's Source Tools add-on and comes in rigged, with each vertex's weights.

**Import** puts a model back: it reads whichever of the exported `.obj` or `.smd` was saved last, or drop any `.obj` or `.smd` on the window for the model shown. The model's bones, shadow shapes and materials are kept; its visible mesh is replaced. Keep the material names (`mat0`, `mat1`...) to keep each part's material; other names take the original's materials in turn.
- From an `.smd`, each vertex gets the bones and weights in the file (the bones are the model's parts by number, as in JSRF ModTool's `.smd` files, so models made for it import as they are). Up to the game's limit for that model: Beat and the other characters take two bones a vertex, the heaviest two are kept.
- From an `.obj` (no weights in it), each vertex takes its bones from the nearest original vertex: fine for a character edited in place.
- At most 65535 vertices. For the `.obj`, keep Blender's export defaults (+Y up, -Z forward), with UVs and normals.

**Materials**, under the 3D view: the shown model's materials. Pick one (Highlight dims the rest of the model, to see which parts use it), change its colour (it multiplies the texture: white leaves it as it is), its shading kind (the kinds JSRF ModTool names: Standard, Skin, Lambert, Metal, reflective metal, semi-transparent, self-illuminated, UV-scrolling), and the three other numbers it holds (what they do isn't known). The 3D view previews the colours; **Save materials** writes them to the game file, **Revert** throws them away.

**Bounds**, under the 3D view: the sphere the game draws the model within (its "draw distance"). A model made bigger than its bounds can pop out of view early: **Bigger** doubles them, **Fit** makes them fit the model again (Import does that by itself).

**Raw items** (the checkbox above a file's textures) lists every item in the file by group, with what it looks like (texture, model, sound, material list, empty, data) and its size, and shows its first bytes. **Extract** saves one as it is into `item_exports/`; **Replace** puts it back from there (or drop any file on the window), whatever its new size; **Copy** and **Paste** copy an item over another, in the same file or another one; **Empty** leaves it with nothing. The original is in `mod_backups/` as for every edit.

**Restore all** (top right) puts back the original of every modded file in `mod_backups/`, and the game's cached copies, after asking once more.

**Sounds** (the Sounds tab, and any file with sounds in it): each sound's length and rate, its waveform, **Play**, **Export WAV** (into `sound_edits/` in the game folder), **Export all**, and **Import WAV** (the exported file, or drop any WAV on the window). An imported WAV can be any length, rate or channel count: it's made the game's rate and channels and its format (Xbox ADPCM or PCM), and the file around it is rebuilt to fit. The original is kept in `mod_backups/` as for textures.

**Music** (m14, idea by Miggas): the `.adx` files in `Media/Z_ADX` are in the Sounds tab too: **Music** (BGM), **Ambience** (Env) and **DJ (Professor K)**. They play, export and import like the other sounds. An imported song can be any length: it's encoded as ADX at the original's rate and channels. A song that loops in the game (the green lines on its waveform) loops the whole new song. Encrypted ADX files aren't read (JSRF's aren't).

Sizes: an image the texture's own size goes in as it is. A texture in a `.dat` can be made bigger: import a square image of 512, 1024, 2048 or 4096 pixels (larger than the texture) and the file makes room for it; the game reads the new size from the texture itself. Any other size is resized to the texture's. Graffiti (`.grf`, `.prs`, `.JTX`) always keep their size.

What it reads: characters, objects, stages, HUD sprites, fonts and logos (`.dat`), graffiti (`.grf`, `.prs`, `.JTX`), and music (`.adx`). Textures in DXT1, DXT3 and DXT5 are written back in the same format, with their smaller mip levels made again from the new image.

## Build

- Windows: `build_modtool.bat`
- Linux: `./build_modtool.sh` (needs SDL2, which SteamOS and desktop Linux have)

Both download Zig (the compiler) into `deps/` the first time, the same way the launcher does.

## Credits

The file formats were learned from [JSRF ModTool](https://github.com/neodoso/JSRF_ModTool) by neodoso and his [jsrf-inside](https://jsrf-inside.blogspot.com) blog. No code was taken from it: this tool is written from scratch in C. The ADX music format (CRI's) was written from its public description, no code taken. Idea by Miggas.
