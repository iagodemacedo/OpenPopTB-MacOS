# Findings: game internals and Windows behaviour

Everything learned while bringing the game up, so nobody has to rediscover it.
Addresses refer to the tested GOG v1.01 files (see README for hashes).

## popTB.exe overview
- MSVC 5/6 build, statically linked CRT, C++ (SEH frames via `fs:[0]`, vtables).
- Sections: `.text` 0x401000, `CSEG` 0x564000 (hand-written asm: sprite/bitmap blitters with 2D
  jump tables), `CSEG` 0x97c000 (RWX, more asm), `DSEG`, `IDCT_DAT`/`UVA_DATA` (video decoder tables).
  No self-modifying code was found (tested by write-protecting code pages: `POPTB_PROTECT_CODE=1`).
- Imports: kernel32, user32, gdi32 (1 function), shell32, ole32, ddraw (`DirectDrawCreate` only;
  Direct3D is not used by this exe), winmm, QMIXER, imm32, wsock32, weanetr, dsound (ordinal 1),
  advapi32, dinput. 240 imports in total.
- Entry 0x548490 (CRT) → WinMain path `f_562a00 → f_4bc730 → f_4ba520` (main loop).

## Startup sequence
1. CRT init, `GetVersion`, heap, command line.
2. Registry: `HKLM\SOFTWARE\Bullfrog Productions Ltd\Populous: The Beginning` values `InstallPath`,
   `InstallDrive`, `InstallDirectory`, `Language` (9 = English), `Version`, optional
   `DrawFrameRateLimit` (12..60, default 40). See `regs.cmd` in the GOG folder.
   The runtime answers `C:\Populous`; `fileio.c` maps `C:\Populous\...` to the game folder.
3. A throwaway DirectDraw object creates a primary surface to probe the display (`FUN_0041f500`).
4. Keyboard thread (`0x52c9d0`) and mouse thread (`0x52ce90`) are started; each creates a DirectInput
   device, sets an event with `SetEventNotification` and waits on it. **The main thread busy-waits
   until they are ready** — this is why the GIL must be yieldable in loops.
5. Main window (`_Bullfrog4194304` class), DirectDraw 640x480x8, QMixer init, Bullfrog logo FMV
   (`fmv/blogo.tgq`), copyright screen, front end.

## Frame pacing
`f_4ba520` waits with `GetTickCount` busy loops:
- front end: `1000 / DrawFrameRateLimit` ms (default 40 fps);
- in game (state byte `0x877598 == 2` and speed flags `0x95706c`): 60/24/20/14 fps depending on
  the game-speed setting (`FUN_0041fa00`). The Rolling Demo runs at 14 fps by design.
Rendering takes ~7% of the frame on an M-series Mac; the rest is waiting.

## CPU speed detection
The game measures the CPU with `rdtsc` and picks default detail levels from it. The runtime reports a
1 GHz TSC; with the original 200 MHz value the game chose "LOW" landscape detail.

## Input
- Mouse: DirectInput, **relative deltas only** (buffered `GetDeviceData`, 16-byte records), processed
  by the mouse thread. Button events in a batch use the cursor position *before* the batch's motion
  is applied, so a real mouse works but scripted input must move first, click a few ms later.
  The game draws its own cursor; the runtime uses SDL relative mouse mode and converts window
  points to game pixels.
- Keyboard: DirectInput buffered data with DIK scan codes (= PC set 1); also WM_KEYDOWN/WM_CHAR.

## Video (FMV)
- EA TGQ (TQI) video, 320x352 yuv420p @25 fps, EA ADPCM stereo audio 22.05 kHz. Decoder in
  `0x558000–0x564000`, uses a reader thread (`_beginthread`) and the main thread.
- The player switches the display to **640x480x16 (RGB565)**.
- It paces frames on the **DirectSound play cursor** (`GetCurrentPosition`). Without working audio
  each frame waits ~0.4 s.
- Uses the x87 "magic number" rounding trick → needs 53-bit FPU precision (see ARCHITECTURE).

## Audio (QMixer)
- `QMixer.dll` (QSound Labs) is the mixer; the game wraps it at `0x53b9xx–0x53c3xx`.
- Stdcall argument sizes of every QMixer export were read from their `ret N`
  (e.g. `InitEx` 4, `PlayEx` 0x18, `OpenWaveEx` 0xc, `ConfigureChannel` 0x14, `SetSourcePosition` 0x10).
- `QSWaveMixInitEx` config: 64-byte struct, flags `0x10` (|2), rate 22050.
- Waves are streamed through game callbacks (`OpenWaveEx` flags 8 and 0x11, callbacks
  `0x542d40`/`0x542d10`) that decode `.sdt` data on QMixer's thread.
- QMixer gets DirectSound via `CoCreateInstance(CLSID_DirectSound)` + `Initialize`, creates a primary
  buffer with `DSBCAPS_CTRL3D` and **requires** `IDirectSound3DListener` (else `QSWaveMixActivate`
  fails and the game silently disables sound). It also requires `waveOutGetNumDevs() > 0`.
- Internal debug log: writing a level to `0x1802dfd4` (`POPTB_QMIX_DEBUG=<n>`) makes QMixer log to
  `C:\dump.txt` (= `<game>/dump.txt`).
- MMX code path exists; disabled by reporting no MMX in `cpuid`.

## Networking (weanetr.dll, "MLDPlay")
See [MULTIPLAYER.md](MULTIPLAYER.md). Key facts:
- C++ class `MLDPlay`, thiscall exports. Services: "Winsock TCP/IP Internet Connection" (raw
  Winsock, UDP port 7575 by default), "Head-to-head modem", IPX via DirectPlay (unavailable).
- `AreWeLobbied` returns `0x20` when not launched from a DirectPlay lobby; any other value makes
  the game jump straight to the multiplayer lobby screen.
- Without weanetr, the game expects `EnumerateServices` to report providers; the fallback stub
  fabricates three (`runtime/misc.c`).

## Windows vs macOS behaviour differences we had to emulate
1. Every Windows thread starts with FPU control word 0x27f (53-bit precision).
2. Code page and memory semantics: unaligned access everywhere, `may_alias` types.
3. `sendto(255.255.255.255)` fails on macOS (EHOSTUNREACH); use directed broadcast per interface.
4. A UDP socket bound to a specific address receives broadcasts on Windows, not on BSD.
5. `closesocket()` wakes threads blocked in `recvfrom()` on Windows; on macOS neither `close` nor
   `shutdown` does, and the fd number is immediately reused. The runtime keeps its own never-reused
   socket handles and waits with `poll` in 50 ms slices.
6. `gethostbyname(<own hostname>)` can hang on mDNS on macOS.
7. Messages belong to the thread that owns the window (QMixer and weanetr have their own windows
   and message loops).
8. Cocoa only accepts event pumping on the main thread.
9. macOS pages are 16 KB (matters for `mprotect` diagnostics).

## Bugs found in the recompiler (and the tools that found them)
| Bug | Symptom | Found with |
|---|---|---|
| Fused `cmp+jcc` stored flags after the branch | corrupted 8x8 blocks in FMV | function replay + trace compare |
| x87 run at 64-bit precision on the main thread | off-by-one pixels in FMV colour conversion | trace compare |
| 2D jump tables (`jmp [base+idx*4+tbl]`) not detected | crash in sprite blitter | crash report (`dispatch: unknown target`) |
| GIL handed over on every loop iteration | UW at 3 fps | `sample` profiler |
