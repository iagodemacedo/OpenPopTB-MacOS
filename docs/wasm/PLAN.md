# OpenPopTB-WASM — Technical Plan

Living technical reference for porting the statically recompiled *Populous: The Beginning*
(OpenPopTB-MacOS) to WebAssembly. Intended to be copied into Notion later.

Last updated: 2026-11-18. Labels used throughout:

- **[V] Verified**: checked in source or by an experiment on this machine (details given).
- **[A] Assumption**: believed true but not yet tested.
- **[Q] Open question**: needs a decision or a measurement.

Raw discovery log: [DISCOVERY-NOTES.md](DISCOVERY-NOTES.md).

---

## 1. Executive summary

The brief's working hypothesis holds up, and the evidence is stronger than expected:

- **[V]** The generated C (6,578 functions, 37 MB, ~972k lines) compiles to wasm32 with Emscripten
  **with zero errors and no changes** to the recompiler.
- **[V]** 15 of the 20 runtime files compile unchanged. The other 5 fail only on macOS headers or
  APIs (timing, backtraces, `pthread_main_np`, BSD socket fields, and the macOS-only
  first-launch compiler `prepare.c`).
- **[V]** With throwaway patches, the whole program links into a 12.7 MB `.wasm` that V8 validates
  and compiles (eager optimizing compile of the entire module takes about 0.7 s).

The work is therefore not "make the recompiler target Wasm". It comes down to four
runtime-level problems:

1. **Guest memory layout.** The runtime reserves a flat 4 GB guest address space with `mmap`.
   wasm32 cannot fit that alongside the host's own memory. The fix is to compact the guest layout
   into a window of about 1 GB, with the code generator unchanged.
2. **Threading model.** Guest threads are real pthreads behind a global lock (GIL). This maps
   directly onto Emscripten pthreads (Web Workers + SharedArrayBuffer), provided no guest code
   ever runs on the browser main thread.
3. **Platform layer.** Window, presentation, input and audio go through SDL3 and assume they
   own the main thread. The game has to run in a worker, so this layer must be adapted or
   replaced with a thin web backend.
4. **File access.** About 370 MB of game data must be reachable through a browser-compatible
   file system, with persistence so the user imports their copy only once.

**Recommended route:** before any browser work, run the Wasm build **headless under Node.js**,
with direct host file access, dummy audio, and the existing scripted input and screenshot tools.
Then diff its screenshots against the native build. This proves the CPU emulation, memory,
threads and game logic in Wasm cheaply. Only then build the browser platform layer.

---

## 2. Objectives and non-goals

**Objectives (MVP)**
- Run the original single-player campaign in desktop Chromium, using the original software
  renderer (8-bit, 640×480 / 800×600), keyboard and mouse.
- Recompile from the user's own GOG files, locally. No game-derived code is committed or hosted.
- Share the generated C and as much runtime code as possible with the native macOS build, and
  keep the native build working at every step as the reference.

**Non-goals (for now)**
- Multiplayer (`winsock.c`, `WEANETR.dll`). Upstream doesn't load WEANETR unless `POPTB_NET=1`.
- The Direct3D build (`D3DPopTB.exe`, `d3d.c`/`gpu.c` on SDL_GPU).
- Undiscovered Worlds (`popTBUW.exe`). Should work later with the same pipeline.
- Mobile, touch, Safari/Firefox, public hosting, in-browser recompilation, mods, HD assets.

---

## 3. Upstream architecture analysis

### 3.1 Pipeline [V]

```text
GOG installer --innoextract--> popTB.exe, QMixer.dll, WEANETR.dll, data/
      |
      | recomp/gen.py + recomp/image.py   (Python 3, capstone, pefile)
      v
gen/f00.c … f72.c, funcs.h, table.c       (C, one function per x86 function)
      +
runtime/*.c                               (C11: CPU helpers, memory, Win32/DirectX shims, SDL3)
      |
      | CMake + Ninja + clang
      v
Populous TB.app (arm64)
```

- `build.sh` runs the whole pipeline. Generating the C takes ~15 s and the native build ~10 s
  on this Mac [V].
- The release app ships only the runtime and recompiler. `runtime/prepare.c` recompiles the
  player's copy on first launch (Python + `xcrun clang`), builds a `.bundle` and `dlopen`s it.
  **This path is macOS-only and irrelevant to the web port.**

### 3.2 Recompiler (`recomp/`) [V]
- `image.py`: maps PE files at their preferred base and discovers code by recursive descent.
  It is seeded by the entry point, exports, relocations, and the Ghidra address list
  `re/export/functions.tsv`. It handles 1D and 2D jump tables.
- `gen.py`: emits `void f_<addr>(CPU *R)` for each function, with a `goto` label per
  instruction. Other details:
  - Indirect jumps become a `switch` if the jump table is known, otherwise
    `dispatch(R, target)`, a binary search over `gen_fn_table`.
  - `call` pushes the real return address onto the guest stack, then calls the C function, so
    calling conventions stay exactly as on x86.
  - A tail `jmp` into another function is emitted as `f_x(R); return;`.
  - Flags are computed lazily, with liveness analysis, and fused into the following `cmp`+`jcc`.
  - x87 is emulated with a stack of `double` that honours precision control. The
    `FCW=0x27f` setting matters for the video decoder.
  - Loop back-edges call `YIELD()`.
- Output size for popTB.exe + QMixer + WEANETR [V]:

  | Metric | Value |
  |---|---|
  | Functions | 4,984 + 1,077 + 517 = 6,578 |
  | x86 instructions | 435k + 48k + 35k |
  | C files / size / lines | 73 files / 37 MB / ~972k lines |
  | Largest function | `f_97c000`, ~17k lines (hand-written asm in `CSEG`) |
  | `dispatch()` call sites | 4,145 |
  | Jump-table switches | 545 |
  | `YIELD()` points | 6,468 |
  | Unimplemented forms | only MMX in QMixer, unused because `cpuid` reports no MMX |

- Pointer-width safety [V]: the generated code uses only `uint32_t` guest values and
  `MEM + (uint32_t)a` accesses through unaligned `may_alias` typedefs (`runtime/cpu.h`). It never
  converts guest addresses to host pointers except through `MEM`. Nothing in it depends on
  64-bit hosts.
- No SEH or C++ exception support: `RtlUnwind` calls `rt_fatal` (`runtime/k32.c:363`). So no
  `setjmp`/`longjmp` or exception machinery is needed in Wasm [V].

### 3.3 Runtime (`runtime/`)

| File | Role | Wasm status [V] |
|---|---|---|
| `cpu.h`, `rt_ops.h`, `table.h` | CPU struct, memory access, flags, x87 inline helpers | compiles |
| `cpu.c` | dispatch, thunks, `rep` string ops, `cpuid`/`rdtsc`, GIL | **fails**: `mach_absolute_time`, `execinfo`, dyld |
| `fpu.c` | x87 80-bit conversion, rounding, `fxam`, `fnsave` | compiles |
| `mem.c` | 4 GB `mmap`, guest heap (bump + size-class free lists), VirtualAlloc | compiles, but **the 4 GB mmap fails at runtime** |
| `main.c` | PE loader, import binding, `DllMain`s, entry point, crash handler, data-dir lookup | **fails**: `execinfo`, dyld, `_NSGetExecutablePath`; signals unusable |
| `k32.c`, `fileio.c` | threads, TLS, sync objects, files with `C:\Populous` mapping (113 + 37 shims) | compiles |
| `user32.c` | windows, per-thread message queues, WndProc (65 shims) | compiles |
| `misc.c` | timers (a pthread per timer), mmio, MCI, weanetr stubs (34 shims) | compiles |
| `com.c` | fake COM objects (guest vtables → native thunks) | compiles |
| `ddraw.c` | DirectDraw surfaces in guest memory, palette, Blt, Flip → `plat_present` | compiles |
| `dinput.c` | DirectInput keyboard/mouse, buffered data, event notification | compiles |
| `dsound.c` | DirectSound software mixer → SDL3 audio stream callback, waveOut | compiles (SDL3 port) |
| `plat.c` | SDL3 window, presentation, events → Win32/DInput, scripted input, screenshots | **fails**: `pthread_main_np` |
| `d3d.c`, `gpu.c` | Direct3D 5 on SDL_GPU | compiles; out of scope |
| `winsock.c` | BSD sockets | **fails**: `sin_len`, `SO_NOSIGPIPE`; out of scope (stub) |
| `snap.c` | memory snapshots and traces for replay tools | compiles |
| `prepare.c` | macOS first-launch recompile (CommonCrypto, `dlopen`, `xcrun`) | **fails**; exclude |

### 3.4 Guest memory [V]
- `mem_init`: `mmap(NULL, 4 GB, MAP_NORESERVE)` and `mprotect` of the first 64 KB to catch null
  pointers (`runtime/mem.c`).
- Layout (`runtime/rt.h`):
  - `popTB.exe` image at 0x00400000
  - `WEANETR.dll` at 0x10000000 (only loaded with `POPTB_NET=1`)
  - `QMixer.dll` at 0x18000000
  - heap from 0x01000000 to 0x70000000, skipping DLL ranges
  - VirtualAlloc region from 0x70000000 to 0xC0000000
  - thunk addresses at 0xFE000000+, `MAGIC_RET` at 0xFDFFFFF0
- Guest stacks are 1 MB each, allocated with `gmalloc`. Each thread's TIB block (`fs:` base) is
  also allocated with `gmalloc` (`k32.c:55–76`).
- Native peak RSS in the front end is about 147 MB, including SDL and the host [V]. The game
  touches only a small fraction of the 4 GB.
- Measured with `POPTB_STATS=1` (`runtime/stats.c`), intro → menu → 3 min of Rolling Demo at
  800×600 [V, 2026-11-19]:

  | Metric | Value |
  |---|---|
  | Guest heap high-water | 93.2 MB (`brk_ptr` = 0x06D325C0; stays below QMixer at 0x18000000) |
  | VirtualAlloc high-water | 8.0 MB |
  | Guest threads | 6: main, keyboard `0x52c9d0`, mouse `0x52ce90`, 2× QMixer `0x1801a4f0`, FMV reader `0x5490d0` |
  | Deepest native stack (arm64, sampled in `dispatch`) | 2.3 KB, main thread |
  | Deepest guest stack | 49.4 KB, main thread (1 MB allocated) |
  | GIL acquisitions | ~322k in 210 s (~1.5k/s) |

  Manual play session (front end → load a save → campaign level, ~7 min including a save, quit from the menu) [V, 2026-11-19]:
  - heap 94.0 MB; it jumps 88 → 93 MB at level load, then creeps ~0.15 MB/min;
  - VirtualAlloc 8 MB, same 6 threads;
  - native stack 2.6 KB, guest stack 65.7 KB;
  - ~1.55k GIL acquisitions/s.

  Not yet measured: Undiscovered Worlds, sessions longer than ~10 min.

### 3.5 Threads and timing [V]
- `CreateThread` starts a real pthread with a **64 MB native stack** (`k32.c:110`). Each guest
  thread has its own `CPU` struct.
- A fair ticket GIL means only one guest thread runs at a time (`cpu.c`). `YIELD()` hands the
  GIL over only when another thread is waiting and the 2 ms quantum has expired.
- Blocking shims (`Sleep`, `WaitFor*`, `GetMessage`, `EnterCriticalSection`) release the GIL and
  use `pthread_cond_timedwait`, `usleep` or `sched_yield`.
- Threads seen in the game:
  - the main/render thread
  - DirectInput keyboard and mouse threads (the main thread busy-waits for them at start-up)
  - QMixer streaming threads
  - the FMV reader thread (`_beginthread`)
  - `timeSetEvent` timers (one pthread each)
  - the waveOut notify thread
  - the SDL audio callback thread, which never calls guest code
- Frame pacing uses `GetTickCount` busy-wait loops (40 fps front end, 14–60 fps in game).
- `plat_pump` only pumps SDL events on the process main thread (Cocoa requirement). On native,
  that is also the guest main thread.

### 3.6 Existing test and debug infrastructure [V]
- `tools/fuzz.sh`: differential instruction fuzzer, Unicorn vs generated C, one case per
  instruction form.
  - Native run here: 18,498 runs, 56 failures in 7 of 1,574 cases. These are the documented
    `pushfd` AF bit and invalid-x87 cases.
  - **`tools/fuzz_main.c` is plain C and should retarget to emcc + Node with no changes.**
    A Wasm run was started but its result not collected **[Q]**.
- `tools/replay.py`: snapshots memory at function entry/exit and re-runs the function in
  Unicorn, optionally diffing per-instruction register traces.
- `tools/run.sh`, `--script`, `--shot-dir`/`--shot-every`, `POPTB_WAV`: fully scripted runs
  with screenshots and audio capture. **This is the basis for native-vs-Wasm comparison.**
- Generation-time `POPTB_HOOK` and `POPTB_TRACE_RANGE` options; `POPTB_TRACE_ALL`;
  `tools/calltree.py`; `tools/where.sh` for crash symbolication.

---

## 4. Proposed target architecture

```text
                     shared, unchanged                          per target
               ┌───────────────────────────────┐   ┌─────────────────────────────────┐
 recomp/*.py → │ gen/*.c  (identical C)        │   │ native: plat.c (SDL3), mem mmap │
               │ runtime/{cpu.h,fpu,k32,user32,│ + │ web:    plat_web.c + JS host,   │
               │  ddraw,dinput,dsound-mixer,   │   │         compact guest window,   │
               │  com,fileio,misc,mem-heap}    │   │         WasmFS/OPFS or MEMFS    │
               └───────────────────────────────┘   └─────────────────────────────────┘

Browser
 ┌──────────── main thread (JS) ─────────────┐     ┌──────── Web Workers (pthreads) ─────────┐
 │ canvas: reads shared framebuffer on rAF   │ SAB │ main() via PROXY_TO_PTHREAD = guest main│
 │ DOM key/mouse → input ring buffer         │<===>│ guest threads (input, QMixer, FMV, timer│
 │ AudioWorklet ← audio ring buffer          │     │ GIL, Atomics-based waits                │
 │ file import UI → OPFS                     │     │ dsound mixer thread → audio ring        │
 └───────────────────────────────────────────┘     └─────────────────────────────────────────┘
```

### 4.1 Guest memory: compact window (recommended)
- Allocate one aligned block of about 1 GB (`GUEST_WINDOW`) from the Emscripten heap at start-up
  and set `MEM` to it. **The code generator is unchanged.**
- Move the runtime-controlled regions inside the window. Measured use is a 93 MB heap and 8 MB
  of VirtualAlloc (§3.4), so a **512 MB window** leaves ample margin:
  - heap: 0x01000000 to 0x10000000 (240 MB, about 2.5× observed)
  - VirtualAlloc: 0x19000000 to 0x20000000 (112 MB, about 14× observed)
  - images stay at their fixed bases (QMixer at 0x18000000; WEANETR at 0x10000000 only with
    networking, which is out of scope)
- Thunk addresses and `MAGIC_RET` stay where they are. They are only compared in `dispatch`,
  never dereferenced **[A, verify by grepping and with a bounds-check build]**.
- Total linear memory is about 512 MB guest + host, staying well under 2 GB. That avoids Emscripten's
  >2 GB pointer pitfalls. Guard the layout constants with `#ifdef __EMSCRIPTEN__` in `rt.h` and
  keep native unchanged.
- What is lost:
  - **Page-0 protection.** Add `-DGUEST_BOUNDS_CHECK`, which turns `rd*`/`wr*` into checked
    variants, as a debug build.
  - **Wrapped addresses.** A wild guest address above the window, or one that wraps (e.g.
    `[eax-4]` with `eax=0`), lands in host memory instead of reading zeros.
- Alternatives considered:
  - **Full 4 GB wasm32 memory with the guest offset.** Feasible: V8 allocated 4 GB of shared
    memory [V]. But it aliases the top of the guest space onto host memory and needs >2 GB
    pointer handling. Rejected for the MVP.
  - **Memory64.** Keeps the 4 GB mmap semantics, but costs performance and Emscripten's mmap
    would zero-commit the memory. Fallback only.

### 4.2 Threads
- Emscripten pthreads (`-pthread`, `-sPROXY_TO_PTHREAD`, `-sPTHREAD_POOL_SIZE≈16`).
  The page must be served with COOP/COEP headers for SharedArrayBuffer.
- The browser main thread never runs guest code, so all GIL waits use `Atomics.wait` in workers.
- Replace `mach_absolute_time` with `clock_gettime(CLOCK_MONOTONIC)` (portable on both targets).
- Shrink native pthread stacks for Wasm. They live in linear memory: 64 MB × ~10 threads is too
  much. Start with 1–2 MB and measure.
- `plat_pump`'s "only on the main thread" check becomes "drain the input ring buffer", which
  can run on any thread.

### 4.3 Platform layer: decide with a spike [Q]

**Option A: SDL3 Emscripten port.** Keeps `plat.c` and `dsound.c` as they are. But SDL3 video
and events expect the browser main thread, while the guest runs in a worker. It would need
OffscreenCanvas or main-thread proxying, which is fragile.

**Option B: thin web backend (recommended unless the spike proves A easy).**
- `plat_web.c` with the same `plat_*` API:
  - `plat_present` converts 8-bit + palette (or RGB565) to RGBA in a shared buffer and bumps a
    frame counter.
  - JS draws it to a canvas with `putImageData` or WebGL on `requestAnimationFrame`.
  - DOM events are written to a SharedArrayBuffer ring as (scancode, mouse delta, button). The
    existing SDL-to-Win32/DInput translation logic is reused by factoring it out of `plat.c` into
    shared code keyed on SDL scancodes.
  - The browser's Pointer Lock API gives relative mouse mode.
- Audio: factor `audio_cb`'s mixing out of `dsound.c`. Run it from a worker pthread on a timer,
  writing into a SharedArrayBuffer ring. An AudioWorklet reads the ring.

Either way, keep a headless backend (`plat_headless.c`: no window, BMP screenshots, scripted
input, dummy audio) for Node and CI.

### 4.4 Files and persistence
- **Node harness:** `-sNODERAWFS=1` gives direct access to the host game folder. Zero work.
- **Browser, MVP:** the user picks the game folder or files, which are copied into OPFS once.
  Two ways to mount them:
  - **(a) WasmFS + OPFS backend.** Works with pthreads and is lazy, so files are not held in
    memory.
  - **(b) Legacy MEMFS populated at start-up.** File contents live in JS memory, *not* in the
    Wasm linear memory, so 370 MB is affordable on desktop. The costs are load time, a temporary
    second copy while loading, and file I/O relayed through the browser main thread.

  Start with whichever works first. **[Q]** (a) is the cleaner long-term choice. `Fmv/`
  (189 MB) can be left out of the first import and fetched lazily later.
- `save/` and settings are written by the game in its own folder, so they persist automatically
  under OPFS or an IDBFS mount.
- Importing straight from the GOG installer (innoextract compiled to Wasm) comes later.

### 4.5 Build
- A new CMake toolchain path: `emcmake cmake -S . -B build-web -DPOPTB_WEB=ON`. This adds:
  - the same `gen/*.c`
  - the runtime minus `prepare.c`, `winsock.c`, `gpu.c`, `d3d.c`, `plat.c`
  - `plat_web.c` or `plat_headless.c`, and `winsock` stubs
- Output: `poptb.js`, `poptb.wasm`, a worker script, and `index.html`. Serve locally with
  COOP/COEP headers (a tiny Node or Vite dev server).
- Flags as native (`-fno-strict-aliasing -fwrapv`, generated code at `-O1`), plus:
  `-pthread -sPROXY_TO_PTHREAD -sALLOW_MEMORY_GROWTH -sMAXIMUM_MEMORY=2GB`
  `-sSTACK_SIZE=1MB -sDEFAULT_PTHREAD_STACK_SIZE=2MB`
- Consider `-ffp-contract=off` on both targets for parity **[Q]**.

---

## 5. Technical feasibility assessment

| Area | Verdict | Evidence |
|---|---|---|
| Generated C → wasm32 | **Feasible, verified** | 73/73 files compile with emcc 7.0.0, zero errors, 8 s |
| Module size and engine limits | **Fine** | 12.7 MB wasm at `-O1`; V8 eager TurboFan ≈0.7 s; lazy compile by default |
| Pointer-width / ABI | **Fine** | guest values are all `uint32_t`; host pointers never stored in guest memory except via `com.c`'s index table |
| Guest memory | **Needs work, bounded** | layout constants plus one allocation; codegen unchanged |
| Threads/GIL | **Feasible, needs testing** | pthread/cond/Atomics supported in workers; behaviour under browser scheduling unmeasured |
| Rendering | **Low risk** | 8-bit framebuffer → RGBA → canvas |
| Input | **Low–medium** | Pointer Lock + scancode mapping; DInput relative deltas already virtualised in `plat.c` |
| Audio | **Medium** | AudioWorklet + ring buffer; latency and autoplay-unlock gesture; FMV pacing depends on the play cursor |
| Files | **Low–medium** | NODERAWFS trivial; browser needs import UI + OPFS or MEMFS |
| Performance | **Probably fine [A]** | native rendering is ~7% of a frame, the rest is waiting. Wasm at 1.5–3× native still leaves headroom |
| Wasm call-stack depth | **Probably fine** | native depth 2.3 KB and guest stack 49 KB in the Rolling Demo; to confirm under V8 in M1 |

**Simpler alternatives considered**
- **Interpreting the x86 in the browser** (berrry-wine-style, MIT, hand-written WAT). Needs no
  recompile, but it is an interpreter ("1997-class machine"), and a different project.
  Kept as a reference for Win32 behaviour, not a dependency.
- **Generating Wasm directly from x86**, skipping C. More work for no gain; the C route already
  compiles.
- **veritr1x/populous-recomp.** Another native recompilation (macOS/iPad, Metal, mod API, kit in
  a submodule). Worth reading for its host abstraction, but switching upstream brings no
  advantage now.
- **Node-first headless port** (adopted as M1). It is the simplification that matters most:
  it removes the browser from the first, riskiest milestone.

---

## 6. Platform compatibility audit (change list)

| # | Location | Problem | Fix | Shared with native? |
|---|---|---|---|---|
| 1 | `cpu.c` `rt_rdtsc`, GIL quantum | `mach_absolute_time` | `clock_gettime(CLOCK_MONOTONIC)` | yes, portable |
| 2 | `cpu.c` `rt_fatal`, `main.c` `on_fault` | `execinfo`, dyld | `#ifdef`; Wasm: `emscripten_log(EM_LOG_C_STACK)` | ifdef |
| 3 | `main.c` signals, `POPTB_PROTECT_CODE` mprotect | no signals/mprotect in Wasm | skip under `__EMSCRIPTEN__`; Wasm traps surface as JS exceptions | ifdef |
| 4 | `main.c` `find_data_dir` | `_NSGetExecutablePath`, `~/Library/...` config | Wasm: data dir from argv/env (`/game`) | ifdef |
| 5 | `mem.c` `mem_init` + `rt.h` layout | 4 GB `mmap` | compact window (§4.1) | ifdef |
| 6 | `k32.c` `CreateThread` | 64 MB pthread stacks | configurable; 1–2 MB on Wasm | ifdef |
| 7 | `plat.c` | SDL3 on main thread, `pthread_main_np` | `plat_headless.c`, then `plat_web.c` (§4.3) | factor shared input translation |
| 8 | `dsound.c` | SDL audio stream callback | mixer function + backend hook (SDL / ring buffer / dummy) | refactor |
| 9 | `winsock.c` | BSD-specific fields | exclude; stub file returning errors | web-only file |
| 10 | `prepare.c` | macOS-only | exclude from Wasm build | — |
| 11 | `misc.c` timers | one pthread per `timeSetEvent` | fine in principle; counts against the pthread pool | — |
| 12 | Tail `jmp` → `f_x(R); return;` | no guaranteed tail calls | [Q] measure; option: `-mtail-call` + `musttail` emitted by `gen.py` | gen change, both |

---

## 7. Technical risks and open questions

| ID | Risk / question | Impact | How to resolve |
|---|---|---|---|
| R1 | Wasm call stack overflow in deep guest call chains (V8 worker stack ~1 MB; big Liftoff frames for huge functions) | crash in game | **Downgraded:** native arm64 depth is only 2.3 KB and the guest stack 49 KB in the Rolling Demo (§3.4), so the call depth is shallow. Confirm in Node (M1) |
| R2 | GIL + `Atomics.wait` fairness and latency in browser workers (busy-wait start-up handshake, 2 ms quantum) | stalls, low fps | Node harness first, then browser; profile |
| R3 | Guest wild accesses outside the 1 GB window silently corrupt host memory | heisenbugs | bounds-check build; run scripted scenarios under it |
| R4 | Thunk/`MAGIC_RET` or other high addresses actually dereferenced | crash | grep plus bounds-check build |
| R5 | libm / FP differences (musl vs Apple libm, FMA contraction) cause native-vs-Wasm divergence | screenshot diffs, maybe desync | `-ffp-contract=off`; compare traces with `replay.py` |
| R6 | FMV pacing on audio play cursor: audio must start (autoplay policy) or videos crawl | slow intros | user gesture before start; cursor advances by wall clock if the audio context is suspended |
| R7 | Memory: pthread pool, shadow stacks, guest window under 2 GB | OOM | budget table in M1 |
| R8 | SDL3 Emscripten port off the main thread | architecture choice | spike (B-12) |
| R9 | Distribution: the Wasm build is game-derived | legal | local build only; document; no hosting |
| Q1 | Which file-system backend first (OPFS-WasmFS vs MEMFS)? | — | try WasmFS+OPFS in M3; MEMFS fallback |
| Q2 | Does the Wasm fuzzer match the native result exactly? | CPU confidence | B-3 |
| Q3 | Heap and VirtualAlloc high-water marks in a long campaign | window size | Rolling Demo 93 MB / 8 MB; campaign play 94 MB / 8 MB, growing ~0.15 MB/min after load (§3.4). Save and load covered. Still to check: multi-hour sessions |

---

## 8. Required tooling and development environment

All on Apple Silicon macOS; no Windows needed. Status on this machine as of 2026-11-18:

| Tool | Use | Status |
|---|---|---|
| Xcode Command Line Tools (clang) | native build | installed |
| Homebrew `cmake`, `ninja`, `sdl3` (3.4.20), `innoextract` (1.9) | native build, extraction | installed |
| Python ≥3.9 + `capstone` 5.0.x, `pefile` (repo `.venv`) | recompiler | installed (capstone 5.0.9; upstream's release vendors 5.0.7) |
| `unicorn` (in `.venv`), Pillow (optional) | fuzzer, replay, contact sheets | unicorn installed |
| emsdk (`~/Dev/emsdk`, emcc 7.0.0) | Wasm build | installed; **needs `EMSDK_PYTHON=/opt/homebrew/bin/python3.13`** (system Python 3.9 too old) |
| Node.js 22 | headless Wasm harness, fuzzer | installed |
| Chrome + DevTools | browser target, Wasm debugging (C/C++ DevTools Support extension for DWARF) | — |
| Ghidra | only if discovery needs new seeds | optional |

Local data layout (outside version control):
- `~/Dev/OpenPopTB-data/game`: extracted GOG files
- `~/Dev/OpenPopTB-data/gen`: generated C, symlinked as `gen` in the repo and listed in
  `.git/info/exclude`

---

## 9. Milestones and acceptance criteria

**M0: Native reference build.** Mostly done.
Acceptance:
- The native app builds from our installer.
- The first campaign level is playable.
- The fuzzer baseline is recorded.
- Stack and heap high-water marks are measured.

**M1: Headless Wasm in Node** (replaces the brief's "M1 compile" and front-loads M3's CPU/memory work).
Acceptance:
- `build-node` runs under Node with NODERAWFS.
- It reaches the main menu with a scripted run.
- Its screenshots match the native build pixel for pixel at the same frame numbers (allowing
  for timing jitter).
- The Wasm fuzzer result equals the native result.

**M2: First browser output.**
Acceptance:
- Chromium shows the intro FMV (video, audio optional) and the main menu from user-supplied
  files, through a local dev server with COOP/COEP.

**M3: First playable level in the browser.**
Acceptance:
- Keyboard and mouse (Pointer Lock) work.
- The tutorial / first campaign level can be completed.
- No crashes in 30 minutes.

**M4: Functional single-player.**
Acceptance:
- Sound and music, FMVs with audio, and save/load persisting across reloads.
- Campaign progression works.
- An automated scripted regression run in Node passes in CI.

**M5: Minimal web app.**
Acceptance:
- Import flow (folder pick, later the installer), loading and error states, fullscreen,
  save management, and user documentation.

---

## 10. Prioritized implementation backlog

Each item is sized for one agent session. "Dep" lists prerequisites.

| ID | Task | Dep | Done when |
|---|---|---|---|
| B-1 | Finish M0: play the first level natively; write `docs/wasm/M0.md` results | — | level completed; notes committed |
| B-2 | Measure native in-game: max C stack depth per thread, `brk_ptr`/`virt_ptr` high-water, thread count | B-1 | **done** (`POPTB_STATS=1`, §3.4): Rolling Demo + ~7 min campaign play including load and save |
| B-3 | Run `tools/fuzz_main.c` under emcc + Node (NODERAWFS) and compare with native | — | identical failing-case set |
| B-4 | Portable timing: `clock_gettime` in `cpu.c` (native unchanged in behaviour) | — | native passes the CONTRIBUTING smoke test |
| B-5 | `#ifdef __EMSCRIPTEN__` guards in `main.c` / `cpu.c` (backtrace, signals, dyld, exe path) | — | runtime compiles with emcc |
| B-6 | Compact guest layout behind a compile-time option + `GUEST_BOUNDS_CHECK` debug mode; first test it *natively* (window via `mmap` of 1 GB) | B-2 | native game runs with compact layout and bounds checks |
| B-7 | Configurable thread stack size | — | native unchanged; Wasm uses 2 MB |
| B-8 | `plat_headless.c` (screenshots, scripted input, no SDL) + dsound dummy backend; usable natively too | — | native headless run produces the same screenshots as `plat.c` |
| B-9 | `winsock` stub file for Wasm | — | links |
| B-10 | CMake `POPTB_WEB`/`POPTB_NODE` target via `emcmake` | B-4, B-5, B-9 | `build-node/poptb.js` links with no undefined symbols |
| B-11 | Node harness: run to the main menu; diff screenshots vs native (`tools/compare_shots.py`) | B-6, B-7, B-8, B-10 | **M1 accepted** |
| B-12 | Spike: SDL3 Emscripten port from a PROXY_TO_PTHREAD worker vs thin web backend (1 day, throwaway) | B-10 | decision recorded in §4.3 |
| B-13 | Factor the SDL-event → Win32/DInput translation out of `plat.c` into shared code | B-12 | native unchanged |
| B-14 | `plat_web.c` + JS host: shared framebuffer → canvas, input ring, Pointer Lock | B-13 | menu visible and clickable |
| B-15 | File import: directory picker → OPFS; mount via WasmFS OPFS (or MEMFS fallback) | B-10 | game files readable in the browser |
| B-16 | Dev server with COOP/COEP + `index.html` | B-10 | page cross-origin isolated |
| B-17 | Audio: mixer backend → SAB ring → AudioWorklet; start on user gesture | B-8 | FMV plays at speed with sound; **M2 accepted** |
| B-18 | Play the tutorial / first level in the browser; fix issues | B-14, B-17 | **M3 accepted** |
| B-19 | Saves persist across reload; settings | B-15 | **M4 (partial)** |
| B-20 | CI: Linux runner builds the runtime + headless Wasm; game files only on a local runner | B-11 | CI green |
| B-21 | Web frontend (Vite/TS, minimal), loading/error states, fullscreen | B-18 | **M5** |

Dependency graph (critical path in bold):

```text
B-1 → B-2 → **B-6** ─┐
B-4, B-5, B-9 → **B-10** ─┼→ **B-11 (M1)** → **B-12** → B-13 → **B-14** ─┐
B-7, B-8 ────────────────┘                                              ├→ **B-18 (M3)** → B-19, B-21
B-3 (independent)            B-15, B-16, B-17 ─────────────────────────┘
```

---

## 11. Testing and debugging strategy

1. **Instruction level.** Run `tools/fuzz.sh` natively and in Wasm (B-3) whenever `recomp/` or
   `cpu.h` changes. Both must show the same failing-case set.
2. **Function level.** `tools/replay.py` works on snapshots, so it should work unchanged on
   snapshots written by the Wasm build (`snap.c` compiles) **[A]**. Use it to bisect
   divergences down to an instruction.
3. **Scenario level.**
   - The same `--script` and `--shot-every N` on native and Wasm-in-Node.
   - Compare screenshots by frame number.
   - Scripts to maintain: intro skip → menu; Rolling Demo for 3 minutes; tutorial start; load a
     save.
   - Timing differs between runs, so compare frames reached by deterministic input
     (keys/clicks), not by wall time.
4. **Memory safety.** Add a `GUEST_BOUNDS_CHECK` build in CI that traps on any access outside
   the window or into the null page.
5. **Crashes in Wasm.**
   - Build with `-g`/`-gsource-map`. Generated function names stay `f_<x86 addr>`, so a JS
     stack trace already names the x86 function.
   - `tools/where.sh` equivalent: map a Wasm function name to `gen/fNN.c` with `grep`.
6. **Performance.** Chrome DevTools Performance panel; compare against native `sample` profiles.
   Watch GIL handoff frequency.
7. **Always keep the native build green.** Every runtime change is checked with the
   CONTRIBUTING smoke test before testing in Wasm.

---

## 12. Concrete instructions for M0

```sh
# 0. tools
xcode-select --install
brew install cmake ninja sdl3 innoextract python@3.13

# 1. extract the GOG installer OUTSIDE the repo
mkdir -p ~/Dev/OpenPopTB-data
innoextract --gog -p 0 -d ~/Dev/OpenPopTB-data/game \
  ~/Downloads/'setup_populous_the_beginning_1.02_d3dfix_(76526).exe'
shasum -a 256 ~/Dev/OpenPopTB-data/game/{popTB.exe,QMixer.dll,WEANETR.dll}
#   expect a46e7c6f…109e, 460292fe…8698, 44f5ea80…6418 (README "Tested game version")

# 2. recompiler venv + generate C (out of tree, symlinked)
cd ~/Dev/OpenPopTB-WASM
python3 -m venv .venv && .venv/bin/pip install capstone pefile unicorn pillow
G=~/Dev/OpenPopTB-data/game
.venv/bin/python -I recomp/gen.py ~/Dev/OpenPopTB-data/gen \
  "$G/popTB.exe:re/export/functions.tsv" "$G/QMixer.dll" "$G/WEANETR.dll"
ln -sfn ~/Dev/OpenPopTB-data/gen gen && grep -qx gen .git/info/exclude || echo gen >> .git/info/exclude

# 3. native build + run
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
ninja -C build poptb
open build/PopulousTB.app --args --data "$G"       # play the tutorial / first level by hand

# 4. automated smoke run + fuzzer baseline
SDL_AUDIO_DRIVER=dummy POPTB_GAME="$G" tools/run.sh 60 --shot-dir /tmp/shots --shot-every 300 \
  --script "2000:key:Escape;3000:key:Escape;4000:key:Escape"
VENV_PY=.venv/bin/python POPTB_GAME="$G" tools/fuzz.sh 3 12   # record: runs / failures / failing cases
```

Status:
- Steps 1–3 done (build OK, front end renders).
- Step 4 done: 18,498 runs, 56 failures, 7 failing cases.
- Remaining: play the first level by hand (B-1) and the measurements (B-2).

Notes:
- Pressing Escape too many times opens the Quit dialog.
- Menu coordinates are in `docs/DEBUGGING.md`.
- `build.sh --installer …` does steps 1–3 in one go, but it extracts into `game/` *inside* the
  repo (gitignored).

---

## 13. References

**Repository (this fork of OpenPopTB-MacOS, MIT)**
- `docs/ARCHITECTURE.md`, `docs/FINDINGS.md`, `docs/DEBUGGING.md`, `docs/ROADMAP.md`,
  `CONTRIBUTING.md`
- Recompiler: `recomp/gen.py` (codegen; `gen_func`, `_gen`, `main` writes `gen/`),
  `recomp/image.py` (discovery), `recomp/fuzz.py`
- Runtime core:
  - `runtime/cpu.h` (memory access, flags, `YIELD`)
  - `runtime/rt.h` (layout constants, `SHIM`)
  - `runtime/mem.c` (mmap, heap)
  - `runtime/cpu.c` (dispatch, GIL)
  - `runtime/main.c` (PE loader, imports)
  - `runtime/k32.c:70–125` (threads)
  - `runtime/plat.c` (SDL3)
  - `runtime/dsound.c:149–201` (audio callback)
- Tools: `tools/fuzz.sh`, `tools/fuzz_main.c`, `tools/replay.py`, `tools/run.sh`,
  `tools/where.sh`

**Upstream and related projects**
- OpenPopTB-MacOS: https://github.com/iagodemacedo/OpenPopTB-MacOS
- populous-recomp (native macOS/iPad recompilation): https://github.com/veritr1x/populous-recomp
- berrry-wine (browser x86 interpreter + Win32 in WAT, MIT): https://github.com/vgrichina/berrry-wine
- OpenPopulous: https://github.com/OpenPop/OpenPopulous
- pop3-rev: https://github.com/hrttf111/pop3-rev
- OpenPop notes: https://github.com/OpenPop/notes
- PopResourceEditor: https://github.com/Toksisitee/PopResourceEditor

**Emscripten docs to read before M1–M3**
- Pthreads and `PROXY_TO_PTHREAD`: https://emscripten.org/docs/porting/pthreads.html
- File systems (MEMFS, NODERAWFS, WasmFS/OPFS): https://emscripten.org/docs/api_reference/Filesystem-API.html
- Memory settings (`MAXIMUM_MEMORY`, growth with pthreads): https://emscripten.org/docs/tools_reference/settings_reference.html
- Debugging Wasm in Chrome DevTools: https://developer.chrome.com/docs/devtools/wasm

**Legal**
- Repository code is MIT.
- Game executables, data and **everything generated from them** (`gen/`, Wasm builds) must not
  be committed or published (README "Legal").
