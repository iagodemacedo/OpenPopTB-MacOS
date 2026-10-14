# Game file formats

Target: `popTB.exe` (GOG v1.01). Addresses are Ghidra virtual addresses (image base 0x400000).
"Observed" = read in the disassembly or verified against the files. "Inferred" = hypothesis.

These notes come from the first, hand-written reimplementation attempt (`src/`). The recompiled
port does not need them (the game parses its own files), but they are useful for tools, mods and a
future hardware renderer.

## levels/levl2NNN.dat (192137 bytes, version >= 11)
Loader: `FUN_0040c330(level)` (observed).

| Offset | Size | Content | Destination in the exe |
|---|---|---|---|
| 0x00000 | 0x8000 | heightmap `int16[128][128]` LE | `cell[i].height` (+4), cells at `0x88897c`, 16 bytes/cell |
| 0x08000 | 0x4000 | byte map A, read and discarded | — |
| 0x0C000 | 0x4000 | byte map B, read and discarded | — |
| 0x10000 | 0x4000 | byte map C, `!= 0` sets `cell.flags \|= 4` | `cell[i].flags` (+0) |
| 0x14000 | 0x40 | 4 tribes x 16 bytes, 2 shorts per tribe | `0x886001`, stride 0xC65 |
| 0x14040 | 3 | byte[2] = initial camera angle | `DAT_00920048` (index into sin/cos tables 0x5acea0/0x5ac6a0) |
| 0x14043 | 2000 x 55 | objects: +1/+2 model/type, +3 owner, +4/+6 x/z, +7 int32 angle>>9 | instantiated per type |
| 0x2EBB3 | 0x96 | 150 extra bytes (only if version > 10), discarded | — |

Sum: 32768 + 49152 + 64 + 3 + 110000 + 150 = 192137 (matches the file size).
The version comes from `FUN_0041d290(levlNNN.ver)`; version < 10 has only the heightmap.
The world is a 128x128 torus: the heightmap wraps at the edges.

## levels/levl2NNN.hdr (616 bytes = 0x9a dwords)
Loaded at `0x883cd9` by `FUN_0040cf80` (observed).

| Offset | Content |
|---|---|
| 0x38 | level name (ASCII, NUL) |
| 0x58 | number of tribes (including the player) |
| 0x59.. | AI script id (`cpatrNNN.dat` / `cpscrNNN.dat`) per AI tribe |
| 0x60 | theme index 0..35 → `FUN_00450730` → `FUN_004503f0` |

## Themes (data/*0-X.dat)
`FUN_004503f0(idx)`: X = '0'+idx if idx < 10, else 'a'+idx-10 (observed).
Files swapped together: sky, pal, fade, ghost, bl320, anibl, baclr, bsclr, bigf, cliff, disp, al.
If `pal0-X.dat` does not exist, theme 0 is kept.

| File | Size | Format |
|---|---|---|
| pal0-X.dat | 1024 | 256 x {R,G,B,0} (observed) |
| sky0-X.dat | 307200 | 640x480 indexed (observed; renders for theme 0, other themes differ) |
| bl320-X.dat | 262144 | atlas 256 wide x 1024, 32x32 indexed tiles (observed) |
| fade0-X.dat | 16384 | shading table, 64 levels x 256 colors (inferred) |
| disp0-X.dat | 65536 | 256x256 (inferred: terrain noise/displacement) |

## Other formats seen at run time
| File | Notes |
|---|---|
| `fmv/*.tgq` | EA TGQ/TQI video (yuv420p, 25 fps) + EA ADPCM audio; ffmpeg decodes them. Played by the game's own decoder at 0x558000-0x564000, opened with `mmioOpenA`. |
| `sound/*.sdt`, `*.sf2` | Sound banks; music tracks (`PopDrum*`, `PopDrones*`) are streamed through QMixer callbacks. `POPFIGHT.SF2` is a SoundFont for the optional `SFMAN32.DLL` (AWE32) path. |
| `data/hspr0-0.dat`, `hfx0-0.dat` | Sprite banks (HSPR). |
| `objects/shapes.dat`, `objs0-*.dat` | 3D object shapes. |
| `data/fenew/fe*` | Front-end (menu) backgrounds, palettes, fades. |
| `save/CONFIG00.*`, `SAVGAM*.HDR` | Options and save headers, written by the game. |
