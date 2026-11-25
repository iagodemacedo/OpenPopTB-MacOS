# OpenPopTB-WASM

An attempt to run **Populous: The Beginning** (1998) **in the browser** via WebAssembly.

This is a fork of [OpenPopTB-MacOS](https://github.com/iagodemacedo/OpenPopTB-MacOS), a native
Apple Silicon port that **statically recompiles** the original Windows executables into C and runs
them on a small reimplementation of the Win32/DirectX APIs on top of [SDL3](https://libsdl.org).
The plan is to compile that same generated C and runtime with [Emscripten](https://emscripten.org)
and present the original software renderer in an HTML canvas, while keeping the native macOS build
working as the reference implementation.

> **Status: early.** The game runs **in Chrome** with keyboard and mouse, using the original software
> renderer, and sound. Saves are lost when the tab is closed (for now). It also runs headless under
> Node.js. Plan and findings: [docs/wasm/PLAN.md](docs/wasm/PLAN.md),
> [docs/wasm/DISCOVERY-NOTES.md](docs/wasm/DISCOVERY-NOTES.md).
>
> ```sh
> # after ./build.sh has generated gen/; needs the Emscripten SDK
> EMSDK=/path/to/emsdk tools/build_wasm.sh --web
> python3 tools/serve_web.py          # open http://localhost:8123/ in Chrome, choose your game folder
> ```

> **You must own the game.** This repository contains **no game code and no game data**. The build
> reads *your* copy of the game (the GOG release) and recompiles it locally. The output is derived
> from your copy and must not be redistributed or hosted publicly. See [Legal](#legal).

| | Native macOS build (inherited) | Browser build |
|---|---|---|
| Single player | Fully playable (campaign, tutorial, Rolling Demo, Undiscovered Worlds) | Not yet |
| Video | Original software renderer, 640x480 and 800x600 | Planned: same renderer in a canvas |
| Audio | Sound effects, music and FMV audio | Planned |
| Multiplayer | Work in progress upstream ([docs/MULTIPLAYER.md](docs/MULTIPLAYER.md)) | Out of scope |

## Quick start (native reference build)
The browser build does not exist yet. The native build is what this fork uses as the reference for
debugging and comparison. This fork publishes no downloads; for prebuilt Mac apps use
[upstream's releases](https://github.com/iagodemacedo/OpenPopTB-MacOS/releases).

### Build from source
1. Get the game files from your GOG copy: the offline installer
   (`setup_populous_the_beginning_*.exe`) or a folder copied from a Windows, GOG Galaxy or CrossOver
   install. The folder must contain `popTB.exe`, `QMixer.dll`, `WEANETR.dll` and the `data/`,
   `levels/`, `sound/`, `fmv/` folders.
2. Install the build tools:
   ```sh
   xcode-select --install
   brew install cmake ninja sdl3 python innoextract
   ```
3. Build:
   ```sh
   git clone https://github.com/vrepinsky/OpenPopTB-WASM.git
   cd OpenPopTB-WASM
   ./build.sh "/path/to/Populous The Beginning"
   # or straight from the installer (extracted into game/):
   ./build.sh --installer "/path/to/setup_populous_the_beginning_1.02_d3dfix_(76526).exe"
   ```
   This takes about a minute. It creates `dist/Populous TB.app` and, if `popTBUW.exe` is present,
   `dist/Populous UW.app`.
4. Run `open "dist/Populous TB.app"`. On first launch the app asks for the game folder and
   remembers it (`~/Library/Application Support/OpenPopTB/data_path.txt`).

### Tested game version
The GOG release, version 1.01. `build.sh` and the app check these SHA-256 hashes and warn if yours differ
(other versions may work but are untested; the runtime has a few address-specific tweaks).

| File | SHA-256 |
|---|---|
| `popTB.exe` | `a46e7c6fa04e13f6cd5accebe32bb5f543f1cd0808bb96b0040f59763251109e` |
| `popTBUW.exe` | `d994142ce0ffb0e5a12ad432cd5def2020d3ca9fb6027a1469d9f7459071e760` |
| `QMixer.dll` | `460292fe61a75996e8035f8a65a765733738293d0d391336be292aa74c128698` |
| `WEANETR.dll` | `44f5ea804323b607c68739d90df3e9516390f4045c2779350372566c2a806418` |

## Playing
- **Cmd+Enter** toggles full screen, **Cmd+Q** quits.
- The mouse is captured while the window has focus (the game draws its own cursor, like the
  original fullscreen mode). Cmd+Tab releases it.
- Game options (resolution, landscape detail, sound) are in the game's own *Options* menu.
- Command-line flags: `--data <folder>` (game folder), `--installer <setup.exe>` (unpack the GOG
  installer and use it), `-v` / `-vv` (logging).
- Saves and settings are written by the game into the `save/` folder of your game directory,
  exactly like on Windows.

## How it works
```
popTB.exe, QMixer.dll, WEANETR.dll  (your copy)
        │  recomp/gen.py: capstone disassembly, recursive code discovery,
        │  x86 → C translation (one C function per x86 function)
        ▼
gen/*.c  (generated locally, never committed)
        │  + runtime/*.c: 4 GB guest address space, Win32 (kernel32, user32, ...),
        │    DirectDraw, DirectInput, DirectSound 3D, waveOut, Winsock, all on SDL3/POSIX
        ▼
Populous TB.app  (native arm64)
```
- The original `popTB.exe` and the `QMixer.dll` sound mixer are **both** recompiled; the game calls
  QMixer, QMixer calls our DirectSound, our DirectSound plays through SDL3.
- At run time the original PE files are loaded into a 4 GB guest address space so that all the
  game's data, tables and pointers stay at their original addresses.
- Correctness is checked with a **differential fuzzer** (every x86 instruction form used by the game
  is run in the [Unicorn](https://www.unicorn-engine.org/) emulator and in the generated C) and a
  **function-level replay** tool that compares whole functions instruction by instruction.

Details: [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md). Everything we learned about the game
internals so far: [docs/FINDINGS.md](docs/FINDINGS.md).

## Repository layout
| Path | What |
|---|---|
| `build.sh` | One-shot build from your game folder or GOG installer |
| `recomp/` | The static recompiler (Python): `image.py` code discovery, `gen.py` x86→C, `fuzz.py` differential fuzzer |
| `runtime/` | The C runtime: CPU helpers, memory, threads, Win32/DirectX reimplementation on SDL3; `prepare.c` is the prebuilt app's first-launch recompilation |
| `tools/` | Debugging tools: run with scripted input and screenshots, crash symbolication, replay, disassembly |
| `re/` | Reverse-engineering inputs: function list exported from Ghidra (addresses only), export script |
| `docs/` | Architecture, findings, file formats, debugging guide, upstream roadmap |
| `docs/wasm/` | WebAssembly port: plan, discovery notes |

## Contributing
Work on the browser port follows [docs/wasm/PLAN.md](docs/wasm/PLAN.md) (backlog in section 10).
General conventions: [CONTRIBUTING.md](CONTRIBUTING.md). Improvements to the recompiler or the
native runtime that are not web-specific are best sent upstream to
[OpenPopTB-MacOS](https://github.com/iagodemacedo/OpenPopTB-MacOS).

## Legal
- *Populous: The Beginning* is © 1998 Electronic Arts / Bullfrog Productions. This project is not
  affiliated with or endorsed by Electronic Arts, Bullfrog or GOG.
- This repository contains only original code (the recompiler, the runtime and tools) under the
  MIT license, plus a list of function addresses used to seed code discovery. It contains **no**
  game code, game data or assets.
- The build output (`gen/`, `build*/`, `dist/`, and any WebAssembly build) is derived from your copy
  of the game. **Do not redistribute or host it.**

## License
[MIT](LICENSE) for the contents of this repository. The license does not cover Populous: The Beginning,
its executables, libraries or data files, nor any code generated from them by this software.
