# Debugging guide

## Logs
- `-v` logs Win32/DirectX activity (files opened, registry, DirectDraw modes, sockets...).
- `-vv` also logs per-frame activity (surface locks, flips, audio buffer positions); `-vvv` logs every native (COM/Win32) call.
- A call to an unimplemented Win32 function aborts with
  `unimplemented Win32 function: <Name> (ret=<return address>)`. Add a `SHIM(Name, nargs)` in the
  runtime (nargs = stdcall argument count; `-1` for cdecl).
- `dispatch: unknown target 0x...` means an indirect call/jump to code the recompiler did not
  discover. Find out who jumps there (`tools/dis.py`) and teach `recomp/image.py` about the pattern.

## Crashes
The crash handler prints the native backtrace and the load address. Map it to x86 addresses:
```sh
cmake -S . -B build-dbg -G Ninja -DCMAKE_BUILD_TYPE=Debug -DPOPTB_GEN_DEBUG=ON && ninja -C build-dbg poptb
POPTB_BIN=$PWD/build-dbg tools/run.sh 60          # reproduce
tools/where.sh <load addr> <pc> <pc> ...          # -> gen/fNN.c line -> L_<x86 address>
```
Native function names in backtraces are `f_<x86 address>`.

To find the guest instruction that crashed: generate with `POPTB_TRACE_RANGE=<lo>-<hi>` around the
function from the backtrace, then run with `POPTB_TRACE_LAST=1`; the crash report lists the last
executed addresses with registers (`[last] ...`).

## Automated runs (no hands needed)
```sh
tools/run.sh <seconds> [--script "<ms>:click:x,y;<ms>:key:Return;<ms>:move:x,y;<ms>:rclick:x,y"] \
             [--shot-dir DIR --shot-every N] [--shot out.bmp --shot-frame N]
python3 tools/sheet.py DIR sheet.png [step]       # contact sheet of screenshots (needs Pillow)
POPTB_WAV=out.raw tools/run.sh 40                 # record the mixer output (float32 stereo 44.1 kHz)
```
Coordinates are game pixels (640x480). Intro movies make timing vary between runs; press Escape a
few times early in the script to skip them.

Useful menu coordinates (640x480, TB): New Game (320,152), Multiplayer (320,242),
Options (320,272), Rolling Demo (320,302). UW: Single Level (320,152), Start (320,438).

## Disassembly and decompilation
- `tools/dis.py <addr> [n]` (popTB.exe), `tools/disdll.py <dll> <addr> [n]` (annotates imports).
- The Ghidra export (`re/export/decomp.c`) is not in the repo. To regenerate it, run Ghidra headless
  with `re/scripts/ExportAll.java` on your `popTB.exe`; then `tools/fn.sh FUN_xxxxxxxx`.

## Differential tools
```sh
python3 -m venv .venv && .venv/bin/pip install capstone pefile unicorn pillow
VENV_PY=.venv/bin/python tools/fuzz.sh 3 12       # instruction-level fuzzer vs Unicorn
```
Function replay (example: the FMV frame decoder):
```sh
POPTB_HOOK=55b920 POPTB_TRACE_RANGE=558000-564000 .venv/bin/python recomp/gen.py gen \
    <game>/popTB.exe:re/export/functions.tsv <game>/QMixer.dll <game>/WEANETR.dll
ninja -C build poptb
mkdir -p /tmp/snap && echo 55b920 > /tmp/snap/func
POPTB_SNAP_FUNC=55b920 POPTB_SNAP_SKIP=30 POPTB_SNAP_DIR=/tmp/snap POPTB_SNAP_TRACE=1 tools/run.sh 8
.venv/bin/python tools/replay.py /tmp/snap                     # memory diff vs Unicorn
.venv/bin/python tools/replay.py /tmp/snap --cmp 558000-564000 # first divergent instruction
```
`POPTB_TRACE_ALL=1` records every thread into `tNNN.trace`; `tools/calltree.py` turns a trace into a
call tree with return values. `POPTB_HOOK_LOG=1` prints arguments/returns of hooked functions.
Remember to regenerate `gen/` without `POPTB_HOOK`/`POPTB_TRACE_RANGE` afterwards.

## Profiling
`sample <pid> 5 -file out.txt` works well; guest functions appear as `f_<address>`.
