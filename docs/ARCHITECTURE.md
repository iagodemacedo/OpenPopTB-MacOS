# Architecture

OpenPopTB-MacOS has two halves: an offline **recompiler** (Python) that turns the x86 code of the
game's PE files into C, and a **runtime** (C) that provides the environment that code expects
(a 32-bit address space, Win32, DirectX) on top of macOS and SDL3.

## 1. Recompiler (`recomp/`)

### Code discovery (`image.py`)
- Loads each PE (`pefile`), maps sections at their preferred base, reads relocations and imports.
- Seeds: entry point, exports, the Ghidra function list (`re/export/functions.tsv`, addresses only,
  only for `popTB.exe`), plus every relocated pointer into code that is a vtable/callback entry
  (relocations located in data sections, or in `push imm` / `mov reg, imm` / `cmp imm`).
- Recursive descent per function with capstone. Calls add new entries; a `jmp` to a known entry
  is a tail call; jumps into other code are followed and the code is duplicated (safe: semantics
  are identical).
- Jump tables: `jmp [reg*4 + table]` and the 2D form `jmp [base + reg*4 + table]` used by the
  hand-written assembly in `CSEG`. Entries are consecutive relocated dwords; leading zero rows of
  2D tables are skipped.
- Iterates to a fixed point. Result for `popTB.exe`: 4,984 functions, ~435k instructions, zero
  undecodable bytes. `QMixer.dll`: 1,077 functions. `WEANETR.dll`: 517. `popTBUW.exe`: 4,888.

### Translation (`gen.py`)
- One C function per x86 function: `void f_<addr>(CPU *R)`. Every instruction gets a label
  `L_<addr>`; control flow is `goto`. Indirect jumps through tables become a `switch`; anything
  unknown goes through `dispatch()`.
- Registers live in a `CPU` struct (`runtime/cpu.h`); memory access is `MEM + (uint32_t)addr`.
- `call` pushes the real return address on the guest stack and calls the C function; `ret n` pops.
  So stack layouts, stdcall/cdecl/thiscall all behave exactly like the original.
- **Lazy flags**: flag-producing instructions store `(op, a, b, result)`; consumers evaluate only
  what they need. A per-function backward **liveness analysis** removes stores whose flags are
  never read. A producer immediately followed by `jcc`/`setcc` is **fused** into a direct C
  comparison (`cmp eax, 5; jl x` → `if ((int32_t)a < (int32_t)b)`).
  *Gotcha (fixed bug):* the lazy store must be emitted **before** the fused branch, because the
  branch target may re-read the same flags (`cmp; jne a; ... a: jle b`).
- **x87** is emulated with a `double` stack. The precision-control bits of the FPU control word
  are honoured (24-bit rounds to float). Windows starts every thread with FCW = `0x27f`
  (53-bit precision), which makes `double` arithmetic bit-exact with the hardware for the
  "magic number" float→int trick used by the video decoder. 80-bit loads/stores are converted.
- String instructions (`rep movs/stos/scas/cmps`) and rotates are runtime helpers.
- No MMX/SSE in the game. QMixer has an MMX path; the runtime's `cpuid` reports no MMX so QMixer
  picks its x86 mixer.
- Loop back-edges emit `YIELD()` (see threads below).
- Debug options (environment variables at generation time): `POPTB_HOOK=<addr,...>` adds
  `hook_enter/hook_leave` calls to functions; `POPTB_TRACE_RANGE=lo-hi` adds a per-instruction
  trace call (`TR`) in that address range.

## 2. Runtime (`runtime/`)

| File | Role |
|---|---|
| `main.c` | Finds the game folder, loads the PE files into guest memory, binds imports, calls `DllMain`s, runs the entry point. Crash handler. |
| `mem.c` | 4 GB `mmap` (page 0 protected), guest heap (`HeapAlloc`, `malloc` of the game's CRT), `VirtualAlloc` region. Skips DLL images that sit inside the heap range (QMixer at 0x18000000, weanetr at 0x10000000). |
| `cpu.c` / `fpu.c` | Indirect-call dispatch (binary search of the function table), native thunks, string ops, x87 helpers, `cpuid`/`rdtsc`, the GIL. |
| `com.c` | Fake COM objects: guest-memory vtables of thunks to native methods. |
| `k32.c`, `fileio.c` | kernel32: threads, TLS, events/semaphores/mutexes, critical sections, heap, files (with `C:\Populous\...` → game folder mapping, case-insensitive), registry values the game expects, locale. |
| `user32.c` | Windows, classes, **per-thread** message queues, `WndProc` dispatch, GDI/IMM/OLE stubs. |
| `plat.c` | SDL3 window, presentation (8-bit palettized and RGB565), SDL events → Win32 messages and DirectInput, scripted input and screenshots for tests. |
| `ddraw.c` | DirectDraw: surfaces in guest memory, primary/back buffer flip, palettes (global display palette, like real 8-bit hardware), Blt/BltFast (8 and 16 bpp). |
| `d3d.c`, `d3d_gpu.c` | Direct3D 5 (`IDirect3D2`, device, viewport, material, texture) for `D3DPopTB.exe`. `d3d.c` is the COM layer; `d3d_gpu.c` records a scene (pre-transformed `D3DTLVERTEX` draws), renders it with SDL_GPU at window resolution (MSAA, mip maps) and copies a 1x version into the guest surface so the game's 2D drawing keeps working; `gpu_present()` then shows the frame and swaps in the hi-res pixels the game did not draw over. |
| `dinput.c` | DirectInput keyboard and mouse: immediate state and buffered data, event notification handles (the game reads input on two threads that wait for these events). |
| `dsound.c` | DirectSound software mixer on an SDL3 audio stream (44.1 kHz float), `IDirectSound3DListener/Buffer` with distance attenuation and panning, and `waveOut`. |
| `misc.c` | timers (`timeSetEvent`), mmio, MCI/MIDI (absent), weanetr stubs used when networking is off. |
| `winsock.c` | Winsock on BSD sockets (multiplayer work in progress). |
| `snap.c` | Debug: memory snapshots and traces for the replay tools. |

### Address space
The game runs in a flat 32-bit space: `MEM[0 .. 4 GB)`. Images at their preferred bases
(`popTB.exe` 0x400000, `QMixer.dll` 0x18000000, `WEANETR.dll` 0x10000000), heap from 0x01000000,
`VirtualAlloc` from 0x70000000, native thunks at 0xFE000000 (calling such an address from guest code
runs a C function), magic return address 0xFDFFFFF0 for native→guest calls.

### Imports
Each import resolves, in order, to: an export of a recompiled DLL loaded in guest memory, a native
shim (`SHIM(Name, nargs)` in the runtime, matched by name), or a trap thunk that reports
"unimplemented Win32 function: Name" when called. Data imports (C++ `@@3` symbols) get a
zeroed block.

### Threads and the GIL
Guest threads are real pthreads, each with its own `CPU`. A global, fair (ticket) lock — the GIL —
lets one guest thread run at a time, which mirrors the single-core machines the game targeted and
keeps the runtime simple. Blocking shims (`Sleep`, waits, `GetMessage`, socket calls) release it.
Loop back-edges call `YIELD()`, which hands the GIL over only if another thread is waiting **and**
the current holder has had it for at least 2 ms (time slicing). Without the quantum, two busy
threads (main renderer + QMixer streaming) ping-ponged on every loop iteration and the expansion
dropped to ~3 fps.

### Audio path
Game → recompiled `QMixer.dll` (QSound software 3D mixer, its own threads) → our `IDirectSound`
(obtained through `CoCreateInstance(CLSID_DirectSound)`), primary buffer with `DSBCAPS_CTRL3D` +
`IDirectSound3DListener`, one streaming secondary buffer → SDL3. The FMV player uses DirectSound
directly (it syncs video to the audio play cursor, so without audio the videos crawl).

### Video path
The game renders 8-bit frames itself (software renderer) into a DirectDraw back buffer and flips.
`ddraw.c` converts with the current display palette and uploads to an SDL texture (nearest-neighbour
scaling, letterboxed). FMVs switch the display to 640x480x16 (RGB565).

## 3. Two ways to build: from source and the prebuilt app
- **From source** (`build.sh`): `gen.py` writes `gen*/*.c` and CMake links them into the app together
  with the runtime. `main.c` points `fn_table`/`import_table` at the generated `gen_fn_table`/
  `gen_import_table`.
- **Prebuilt app** (`-DPOPTB_PREBUILT=ON`, `tools/package_release.sh`, CI on tags): the app holds only
  the runtime, `recomp/gen.py` + `image.py` with capstone 5.0.7 and pefile vendored next to them, the
  Ghidra address list (`seeds/popTB.exe.tsv`) and the headers the generated code includes
  (`cpu.h`, `rt_ops.h`, `table.h`), plus innoextract and its libraries. On first launch
  `runtime/prepare.c`:
  1. finds the game (saved path, `--data`, or asks: game folder, or GOG installer unpacked with the
     bundled innoextract into `~/Library/Application Support/OpenPopTB/Game`);
  2. hashes `<exe>`, `QMixer.dll`, `WEANETR.dll`; the cache key is those hashes plus
     `POPTB_GEN_ABI` (app version + hashes of `gen.py`, `image.py`, `cpu.h`, `rt_ops.h`, `table.h`);
  3. if `cache/<exe>-<key>/game.bundle` is missing: checks for the Command Line Tools (offers
     `xcode-select --install`), copies the recompiler out of the app without the quarantine flag
     (Gatekeeper refuses quarantined libraries even after the app was approved), runs
     `/usr/bin/python3 -I gen.py`, compiles each file with `xcrun clang` (same flags as the CMake
     build, one job per performance core) and links `clang -bundle -bundle_loader <app binary>`, so
     the generated code's references to the runtime (`MEM`, `dispatch`, ...) resolve against the app;
  4. `dlopen`s the bundle (the bundle is only ad-hoc signed, so the hardened-runtime app carries the
     `com.apple.security.cs.disable-library-validation` entitlement) and reads the tables with
     `dlsym`. Bundles for other keys are deleted.

  About a minute the first time (40 s measured on a 15-core M-series Mac), about 4 s afterwards.
  The generated code was verified byte-identical to the from-source output (same capstone version).

## 4. Verification
- `tools/fuzz.sh`: for each distinct instruction form (mnemonic + operand kinds, plus adjacent
  flag-producer/branch pairs), random register/flag/memory states are run in Unicorn and in the
  generated C; registers, defined flags, x87 stack and memory writes are compared. Current result:
  ~18.5k runs, the only differences are `pushfd` (AF bit, not modelled) and invalid x87 operations
  (sqrt/log of negative numbers) where Unicorn itself is quirky.
- `tools/replay.py`: snapshot memory at entry/exit of one call of a function (`POPTB_HOOK` +
  `POPTB_SNAP_FUNC`), re-run it in Unicorn and diff; with `--cmp`, compare per-instruction register
  traces and stop at the first divergence. This found both video-decoder bugs (6.3M instructions
  now match exactly).
