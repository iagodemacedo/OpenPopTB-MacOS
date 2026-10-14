# Changelog

All notable changes are documented here. Format: [Keep a Changelog](https://keepachangelog.com/),
versions follow [Semantic Versioning](https://semver.org/).

## [Unreleased]
### Planned
- v0.2.0: downloadable `.app` that recompiles the player's own copy on first launch (no game code
  in the release). See docs/ROADMAP.md, item 0.

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
