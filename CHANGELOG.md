# Changelog

All notable changes are documented here. Format: [Keep a Changelog](https://keepachangelog.com/),
versions follow [Semantic Versioning](https://semver.org/).

## [Unreleased]
### Added
- Hi-res Direct3D build: `D3DPopTB.exe` (shipped with the GOG game) is recompiled too and runs on a
  Direct3D 5 (`IDirect3D2`) implementation on top of SDL_GPU (`runtime/d3d.c`, `runtime/d3d_gpu.c`).
  The scene is rendered at the window's resolution with 4x MSAA, mip maps and anisotropic
  filtering; the 2D interface and cursor stay crisp on top. `build.sh` produces
  `dist/Populous TB D3D.app` and `dist/Populous UW D3D.app` when `D3DPopTB.exe` / `D3DPopTBUW.exe` are present. Tunables: `POPTB_D3D_SCALE`,
  `POPTB_D3D_MSAA`, `POPTB_D3D_FILTER=game`. Only the Metal backend has shaders so far.
- Software build: the final image is scaled with SDL's pixel-art filter (`POPTB_FILTER=nearest|linear|pixelart`).
- Debugging: `-vvv` logs every native call, `POPTB_TRACE_LAST=1` prints the last executed guest
  addresses on a crash (needs `POPTB_TRACE_RANGE` at generation time), `POPTB_D3D_DUMP=<dir>` saves
  window pictures.

- The graphics *Quick Defaults* preset is set to High at start (all four builds) (`runtime/presets.c`; the
  game does not remember it). `POPTB_PRESET=low|medium|off` overrides it.
- App icons: the built apps get the game's own icon (`gfw_high.ico` / `gfw_high__addon.ico` from the
  game folder, then the older `goggame-*.ico`, then the one embedded in the exe via `tools/exe_icon.py`).

### Fixed
- `CFBundleIconFile` was never set (CMake leaves an empty key), so the apps showed no icon even when
  an `.ico` was found.
- Direct3D build: the picture and the cursor are clipped to the viewport of the scene, but the game
  switches between a 3D viewport and a full-screen one within a frame; the read-back now covers
  everything drawn (the cursor over the left panel used to be invisible), and only the *current*
  viewport sets the clipping rectangle.
- Direct3D build: the GIL is released while waiting for the GPU or the display (vsync), so the input
  and sound threads no longer starve (clicks and the cursor reacted late or not at all).
- Game versions whose second `CSEG` section is not flagged executable (the 2018 GOG installer):
  code discovery treats a section as code when a section of the same name is executable.
- Direct3D build: only the rectangles a scene drew into are copied back (not their bounding box),
  GPU buffers are reused between scenes, texture lookups no longer scan a list, and a colour key
  change refreshes the texture. Display refresh is no longer waited for after every frame.
- The DX3 `IDirect3D`/`IDirect3DTexture` interfaces are refused instead of being served with the
  DX5 method tables.
- `MessageBoxW` text with non-ASCII characters is printed correctly.
- `IDirectDrawSurface::QueryInterface` fails for interfaces it does not implement (colour/gamma
  control) instead of returning the surface itself.

## [0.1.2] - 2026-10-14
### Changed
- The download is signed with a Developer ID and notarized by Apple: macOS opens the apps without
  the "Open Anyway" step. `tools/package_release.sh` signs with `SIGN_ID` and notarizes with
  `NOTARY_PROFILE` (or `NOTARY_APPLE_ID`/`NOTARY_PASSWORD`/`NOTARY_TEAM_ID`); CI does both when the
  signing secrets exist, and builds an ad-hoc signed app otherwise.
- Apps use the hardened runtime with the `disable-library-validation` entitlement
  (`tools/entitlements.plist`), needed to load the game bundle compiled on the player's Mac.

## [0.1.1] - 2026-10-14
### Added
- Prebuilt download (`OpenPopTB-MacOS-0.1.1.zip`): the apps contain no game code; on first launch
  they recompile the player's own copy (about a minute; 40 s measured on a 15-core M-series Mac)
  and cache the result. Needs Apple's Command Line Tools, which the app offers to install. Built
  and attached to GitHub releases by CI.
- The app accepts the GOG offline installer directly (unpacked with a bundled innoextract), besides
  an already extracted or installed game folder. Also `--installer <setup.exe>` on the command
  line and in `build.sh`.
- `tools/package_release.sh` and `.github/workflows/release.yml`.

### Changed
- Generated tables are named `gen_fn_table`/`gen_import_table`; the runtime reaches them through
  pointers so the same code works linked in or loaded from a bundle. Regenerate `gen*/` (build.sh
  always does).

## [0.1.0] - 2026-10-14
First public version.

### Added
- Static recompiler for 32-bit x86 PE files (`recomp/`): recursive code discovery seeded by
  relocations, exports and a Ghidra function list; 1D and 2D jump tables; lazy flags with liveness
  analysis and compare/branch fusion; x87 with precision control.
- Native runtime on SDL3 (`runtime/`): 4 GB guest address space, guest heap, threads with a
  time-sliced global lock, kernel32/user32/gdi32/advapi32/winmm shims, DirectDraw (8 and 16 bpp),
  DirectInput (keyboard, mouse), DirectSound with 3D listener/buffers, waveOut.
- Populous: The Beginning (`popTB.exe`) playable: front end, tutorial, campaign, Rolling Demo,
  options, 640x480 and 800x600.
- Populous: Undiscovered Worlds (`popTBUW.exe`) playable.
- Sound effects and music through the recompiled `QMixer.dll`.
- FMV playback (EA TGQ) with audio.
- `build.sh` one-shot build and `.app` packaging with SDL3 bundled.
- Verification tools: differential instruction fuzzer and function replay against Unicorn,
  scripted input, screenshots and audio capture for automated runs.

### Known limitations
- Multiplayer disabled (work in progress, `POPTB_NET=1`).
- Direct3D build (`D3DPopTB.exe`) not supported; the original software renderer is used.
- Tested only with the GOG v1.01 files.
