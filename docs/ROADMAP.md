# Roadmap

Current state: single player of The Beginning and Undiscovered Worlds fully playable, software
renderer (640x480 / 800x600), audio and FMV working. Effort estimates are rough.

## 0. Prebuilt app releases (target: v0.2.0)
Goal: players download a ready-made `.app` instead of building, **without distributing any game code**.
The release ships only our code (recompiler + runtime); the game code is generated and compiled on
the player's Mac from their own copy, the first time the app opens.
- Port the generator (`recomp/image.py` + `recomp/gen.py`, today Python + capstone) to C, linked
  against the capstone C library, so it runs inside the app.
- First-launch flow: pick the game folder, verify hashes, generate C into
  `~/Library/Application Support/OpenPopTB/<hash>/`, compile it with the system `clang` (Xcode
  Command Line Tools; macOS offers to install them) into a dylib, then `dlopen` it. Progress screen
  ("Preparing the game, about 1 minute"). Rebuild automatically when the runtime version changes.
- Accept the GOG installer directly: the player points to `setup_populous_the_beginning_*.exe`
  (plus any `.bin` parts next to it) and the app extracts it, instead of asking for an extracted folder.
  Bundle [innoextract](https://constexpr.org/innoextract/) (zlib license; needs liblzma, boost
  libraries linked statically) and run the equivalent of `innoextract --gog -d <app data> <setup.exe>`,
  then continue with the hash check. Keep accepting an already extracted/installed folder
  (GOG Galaxy, CrossOver, Windows copy). Also add `--installer <setup.exe>` to `build.sh`.
- Split the runtime so the app binary contains no `gen/` code: `fn_table`/`import_table` and all
  `f_*` come from the dylib.
- CI (GitHub Actions, macOS runner) builds, signs ad hoc and attaches `OpenPopTB-MacOS-x.y.z.zip`
  to the release. CI never sees game files.
- Optional later: notarization (needs an Apple developer account) and an x86 interpreter fallback
  for players without the Command Line Tools.
Effort: 1–2 days. Legal note: this follows the common "user supplies the game, transformation happens
on their machine" model; it is not legal advice.

## 1. Hardware rendering (the Direct3D build)
Why: higher resolutions, texture filtering, smoother look than the software renderer.
- Find out which Direct3D version `D3DPopTB.exe` uses (it gets the interface through
  `QueryInterface` on DirectDraw; it imports only `DirectDrawCreate`/`DirectDrawEnumerateA`).
  ~10 minutes; this sizes the rest.
- Recompile `D3DPopTB.exe` (automatic with `recomp/gen.py`).
- Implement Direct3D (textures, render states, z-buffer, blending, primitives) on Metal or SDL_GPU.
- Validate scene by scene against the software renderer.
Effort: days. Risk: medium (many state details that become visual glitches).

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
0. Prebuilt app releases (v0.2.0).
1. Check the Direct3D version (10 min) and decide on item 1.
2. Scaling filter (quick visible win).
3. Verify SoundFont music.
4. Multiplayer.
5. Hardware rendering and high-resolution textures.
