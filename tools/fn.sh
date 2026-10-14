#!/bin/sh
# usage: tools/fn.sh FUN_0040cc10 [...] -> prints decompiled bodies from the local Ghidra export
# (re/export/decomp.c is generated locally by re/scripts/ExportAll.java and is not versioned)
P="$(cd "$(dirname "$0")/.." && pwd)"
for f in "$@"; do
  awk -v n="$f" '/^\/\/ ==== /{p=($3==n)} p' "$P/re/export/decomp.c"
done
