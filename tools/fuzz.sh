#!/bin/sh
# Differential fuzzer (Unicorn vs generated C). usage: tools/fuzz.sh [samples_per_form] [states_per_sample]
# env: POPTB_GAME (game folder), VENV_PY (python with capstone/pefile/unicorn), FUZZ_OUT (work dir)
#      FUZZ_WASM=1: also build the same tests with emcc and run them under Node.js (needs emcc in PATH
#      or EMSDK=/path/to/emsdk); FUZZ_REUSE=1: keep the cases already generated in FUZZ_OUT
set -e
P="$(cd "$(dirname "$0")/.." && pwd)"
GAME="${POPTB_GAME:-$(cd "$P/.." && pwd)}"
V=${VENV_PY:-$P/.venv/bin/python}
O=${FUZZ_OUT:-/tmp/poptb-fuzz}
mkdir -p "$O"
[ -n "$FUZZ_REUSE" ] && [ -f "$O/fz_cases.bin" ] || \
  "$V" -I "$P/recomp/fuzz.py" "$GAME/popTB.exe" "$P/re/export/functions.tsv" "$O" ${1:-3} ${2:-12}
clang -std=gnu11 -O1 -fno-strict-aliasing -fwrapv -w -I"$P/runtime" -o "$O/fuzz" "$P/tools/fuzz_main.c" "$P/runtime/fpu.c" "$O/fz_tests.c"
echo "== native"
"$O/fuzz" "$GAME/popTB.exe" "$O/fz_cases.bin" > "$O/native.txt" || true
tail -1 "$O/native.txt"
if [ -n "$FUZZ_WASM" ]; then
  if ! command -v emcc >/dev/null; then . "$EMSDK/emsdk_env.sh" >/dev/null 2>&1; fi
  emcc -std=gnu11 -O1 -fno-strict-aliasing -fwrapv -w -I"$P/runtime" -sNODERAWFS -sALLOW_MEMORY_GROWTH \
       -sMAXIMUM_MEMORY=4GB -o "$O/fuzz.js" "$P/tools/fuzz_main.c" "$P/runtime/fpu.c" "$O/fz_tests.c"
  echo "== wasm32 (Node.js)"
  node "$O/fuzz.js" "$GAME/popTB.exe" "$O/fz_cases.bin" > "$O/wasm.txt" || true
  tail -1 "$O/wasm.txt"
  # same instruction forms failing on both targets?
  grep '^FAIL' "$O/native.txt" | sort -u > "$O/native.fail"
  grep '^FAIL' "$O/wasm.txt" | sort -u > "$O/wasm.fail"
  if cmp -s "$O/native.fail" "$O/wasm.fail"; then echo "failing forms: identical on native and wasm32"
  else echo "failing forms differ (native < > wasm):"; diff "$O/native.fail" "$O/wasm.fail" || true; fi
fi
