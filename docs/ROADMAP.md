# Roadmap

Current state: single player of The Beginning and Undiscovered Worlds fully playable, software
renderer (640x480 / 800x600), audio and FMV working. Effort estimates are rough.

## 0. Prebuilt app releases (done in v0.1.1)
Players download `OpenPopTB-MacOS-x.y.z.zip`; the app recompiles their own copy on first launch.
How it works: [ARCHITECTURE.md](ARCHITECTURE.md), section 3. Decisions and what is left:
- The generator stayed in Python instead of being ported to C: it runs on the Command Line Tools'
  `/usr/bin/python3`, which is there anyway because compiling needs the Command Line Tools. Output
  verified identical to the from-source build. A C port remains an option if Python ever becomes
  a problem (for example Apple removing it from the Command Line Tools).
- Minimum macOS of the download = the newest minimum among the bundled binaries (Homebrew bottles
  target the build machine; CI uses `macos-14`, so the download needs macOS 14). Building SDL3,
  innoextract and Boost from source with `MACOSX_DEPLOYMENT_TARGET=12.0` would lower it to 12.
- Signed with a Developer ID and notarized since v0.1.2 (hardened runtime, plus
  `disable-library-validation` so the app can load the game bundle compiled on the player's Mac).
- Not done: Intel build, and an x86 interpreter fallback for players without the Command Line
  Tools.

## 1. Hardware rendering (the Direct3D build)
Why: higher resolutions, texture filtering, smoother look than the software renderer.
- Find out which Direct3D version `D3DPopTB.exe` uses (it gets the interface through
  `QueryInterface` on DirectDraw; it imports only `DirectDrawCreate`/`DirectDrawEnumerateA`).
  ~10 minutes; this sizes the rest.
- Recompile `D3DPopTB.exe` (automatic with `recomp/gen.py`).
- Implement Direct3D (textures, render states, z-buffer, blending, primitives) on Metal or SDL_GPU.
- Validate scene by scene against the software renderer.
Effort: days. Risk: medium (many state details that become visual glitches).

Status: Direct3D 5 (`IDirect3D2`, TL vertices). The build recompiles and plays; SDL_GPU renders the
3D scene at the window's resolution and the CPU's 2D draws are overlaid at game resolution
(map of the game's D3D usage: `re/d3d_usage.md`). Validated in the tutorial's globe and close-up
views. Point-sampled draws (HUD, text, sprites) take the texel of the whole game pixel, so they look
exactly like 640x480 without atlas bleeding. Known differences:
- No blue ring around the globe: D3DPopTB.exe never draws it (`FUN_0042ea10` has no ring; the
  software build paints it with the CPU in `FUN_004d4420`).
- Terrain block edges show as faint lines at high scales: each 32x32 block is its own clamped
  texture, so bilinear magnification can't blend across blocks. Fixing it needs the blocks' world
  positions to sample neighbours.

## 2. Multiplayer (TCP/IP)
Paused. Progress and next steps in [MULTIPLAYER.md](MULTIPLAYER.md).
Remaining: finish the join handshake, play a full match between two instances, then Mac vs the
original Windows game. Effort: 1–3 days. Risk: medium.

## 3. Image and texture improvements
- Final-image scaling: today nearest-neighbour. Options: linear, integer scaling with borders, or an
  upscaling shader (xBR/ScaleFX) on the converted frame. Cheap: hours.
- High-resolution textures (replacing `bl320-*` atlases, HSPR sprites) only make sense with hardware
  rendering (item 1). Needs a new texture pack. High effort.
- Higher software resolutions: the menu offers only 640x480/800x600; check whether the renderer
  accepts 1024x768+ when forced (the limit may be just the menu). Hours to test.

## 4. Mac experience
- Remember window/fullscreen state; optional fullscreen by default.
- Key to release the mouse (today it is captured while the window has focus).
- Verify trackpad tap-to-click in the main menu on a real trackpad. A tap delivers press and release
  in the same instant and the menus ignored it; since v0.2.0 the release is held until the press is
  50 ms old (`MIN_CLICK_MS` in `runtime/plat.c`). Confirmed only with scripted 0 ms clicks
  (`click:x,y,0`). Check New Game, Options and in-game clicks, and fast double clicks. 5 minutes.
- Gamepad support through SDL (optional).
- Universal binary (x86_64) for Intel Macs.

## 5. Robustness
- More fuzzing samples; full x87 coverage.
- Long runs: whole campaign, save/load, every UW level.
- Run Ghidra headless on `popTBUW.exe` to seed code discovery (today UW relies only on
  entry/exports/relocations; no missing function has been hit in tests).
- Bit-exact x87 (80-bit) for lockstep multiplayer with Windows players.

## 6. SoundFont music (verify)
The game tries to load `SFMAN32.DLL` (AWE32 SoundFont manager) and the GOG folder ships
`POPFIGHT.SF2`. Check whether there is MIDI music that does not play today; if so, render it with a
small synthesizer (e.g. TinySoundFont). Hours to verify, about a day to implement.

## Suggested order
1. Check the Direct3D version (10 min) and decide on item 1.
2. Scaling filter (quick visible win).
3. Verify SoundFont music.
4. Multiplayer.
5. Hardware rendering and high-resolution textures.
