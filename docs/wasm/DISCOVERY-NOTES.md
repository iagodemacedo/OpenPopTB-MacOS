# WASM port — discovery notes (paused 2026-11-18)

Status: technical discovery in progress. Planning doc NOT yet written to Notion (connector returned
404 for the page; it is likely in a workspace the Notion integration can't see).

## What was done
1. Read the upstream code (this repo is already a fork of iagodemacedo/OpenPopTB-MacOS, MIT):
   docs/*, recomp/gen.py, recomp/image.py, runtime/* (cpu, mem, main, k32, user32, plat, dsound, com).
2. Extracted the GOG installer `setup_populous_the_beginning_1.02_d3dfix_(76526).exe` with innoextract
   into `~/Dev/OpenPopTB-data/game` (outside the repo). 493 MB total; Fmv 189 MB, data 77 MB,
   Sound 77 MB, __redist 100 MB (not needed).
3. SHA-256 of popTB.exe, popTBUW.exe, QMixer.dll, WEANETR.dll **match the upstream-tested hashes exactly**.
4. Generated C: `~/Dev/OpenPopTB-data/gen` (symlinked as `gen` in the repo, excluded via .git/info/exclude).
5. Built and ran the native macOS reference (`build/PopulousTB.app`): 10 s build; front end renders
   (screenshots in /tmp/poptb-shots*). Not yet played in-game. **M0 partly done.**
6. Installed emsdk locally at `~/Dev/emsdk` (emcc 7.0.0; needs `EMSDK_PYTHON=/opt/homebrew/bin/python3.13`).
7. Wasm experiments (throwaway, in /tmp/wasmexp; repo untouched).

## Verified findings
- Recompiler output for popTB.exe + QMixer + WEANETR: 6,578 functions, 518k x86 instructions →
  73 C files, 37 MB, ~972k lines. Largest function 17k lines (f_97c000, hand-written asm CSEG).
  4,145 dispatch() sites, 545 jump-table switches, 6,468 YIELD() back-edges. Generation takes ~15 s.
- **All 73 generated files compile to wasm32 with emcc, zero errors** (8 s, 12 jobs, -O1, 14 MB objects).
- Runtime: 15/20 files compile unchanged with emcc. Failures are shallow:
  cpu.c (mach_time), main.c (execinfo, dyld, _NSGetExecutablePath, signals), plat.c (pthread_main_np),
  winsock.c (sin_len, SO_NOSIGPIPE), prepare.c (CommonCrypto/dlopen/xcrun — macOS-only, exclude).
- With trivial patches/stubs, the **whole thing links into a 12.7 MB .wasm with no undefined symbols**
  (-pthread, PROXY_TO_PTHREAD, SDL3 port). V8 (Node 22) validates it; eager TurboFan compile of
  the whole module ≈0.7 s. Code size / engine limits are not a blocker.
- V8 can allocate 4 GB shared wasm memory (lazily committed).
- Native peak RSS in the front end ≈147 MB, so the game touches far less than its 4 GB guest space.
- Native front end still runs with the main-thread stack capped at 512 KB (in-game depth not measured).
- The upstream differential fuzzer (Unicorn vs. generated C) runs natively: 18,498 runs, 56 failures
  in 7 cases (upstream-documented pushfd AF / invalid x87 differences). A wasm/Node run of the same
  fuzzer was started when the session was paused; its result was not collected.

## Main design consequences
- **Guest memory**: runtime mmaps 4 GB and uses `MEM + (uint32_t)addr`. Can't coexist with the host
  heap in wasm32. Proposed: compact the guest layout into a ~1 GB window (images are fixed at
  0x400000 / 0x10000000 / 0x18000000; heap, VirtualAlloc, stacks are runtime-controlled; thunks
  0xFE000000 and MAGIC_RET are dispatch-only values, verify never dereferenced). Codegen unchanged.
  Loses page-0 protection; add an optional bounds-checked debug build. Fallback: Memory64.
- **Threads**: real pthreads + a GIL map directly to Emscripten pthreads (needs COOP/COEP,
  SharedArrayBuffer). Guest code must never run on the browser main thread → PROXY_TO_PTHREAD.
  Native pthread stack is 64 MB per thread; must shrink (lives in linear memory on wasm).
- **Platform**: SDL3 Emscripten backend expects the main thread; game renders/pumps from a worker.
  Open decision: SDL3 + OffscreenCanvas/proxying vs. a thin `plat_web.c` (shared framebuffer +
  input ring buffer + AudioWorklet). Leaning thin web backend; needs a 1-day spike.
- **Files**: ~370 MB of game data. (Corrected: legacy MEMFS keeps file contents in JS memory, outside
  the Wasm linear memory, so loading it all is viable; the costs are load time, a temporary second
  copy, and file I/O relayed through the browser main thread.) Options: WasmFS + OPFS (lazy,
  persistent) or MEMFS populated from OPFS; NODERAWFS for the Node harness.
- **Cheapest proof of concept**: run the wasm build headless under **Node** (NODERAWFS, dummy
  audio, existing scripted input + BMP screenshots) and diff screenshots against native. Validates
  CPU/memory/threads/game logic before any browser work.

## Open risks / questions
- Wasm VM stack depth in workers (~1 MB) vs. deep guest call chains in-game (measure natively in-game).
- Tail `jmp` to functions is emitted as `f_x(R); return;` — no guaranteed tail calls on wasm.
- libm differences (sin/cos/atan2…) native vs. musl; consider `-ffp-contract=off` on both targets.
- Distribution: wasm contains game-derived code → local build only (no public hosting).
- Reference projects: berrry-wine (MIT, hand-written WAT x86 interpreter + Win32, browser; Populous
  not mentioned in README); veritr1x/populous-recomp (native macOS/iPad recompilation, kit in a
  submodule, not inspected).

## Next steps when resuming
1. Collect the wasm fuzzer result (`/tmp/wasmexp-scripts/fuzzwasm.sh`).
2. Write the structured plan to Notion (fix page access first).
3. Finish M0: play the first level natively; measure in-game stack depth and guest heap high-water mark.
