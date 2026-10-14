#!/bin/sh
# Differential fuzzer (Unicorn vs generated C). usage: tools/fuzz.sh [samples_per_form] [states_per_sample]
# env: POPTB_GAME (game folder), VENV_PY (python with capstone/pefile/unicorn), FUZZ_OUT (work dir)
set -e
P="$(cd "$(dirname "$0")/.." && pwd)"
GAME="${POPTB_GAME:-$(cd "$P/.." && pwd)}"
V=${VENV_PY:-$P/.venv/bin/python}
O=${FUZZ_OUT:-/tmp/poptb-fuzz}
mkdir -p "$O"
"$V" -I "$P/recomp/fuzz.py" "$GAME/popTB.exe" "$P/re/export/functions.tsv" "$O" ${1:-3} ${2:-12}
clang -std=gnu11 -O1 -fno-strict-aliasing -fwrapv -w -I"$P/runtime" -o "$O/fuzz" "$P/tools/fuzz_main.c" "$P/runtime/fpu.c" "$O/fz_tests.c"
"$O/fuzz" "$GAME/popTB.exe" "$O/fz_cases.bin"
