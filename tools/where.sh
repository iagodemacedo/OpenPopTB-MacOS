#!/bin/sh
# tools/where.sh <load addr> <pc>...: maps native PCs (from a crash backtrace) to guest x86 addresses
# using the line tables of gen/*.c. Best with a POPTB_GEN_DEBUG=ON build.
P="$(cd "$(dirname "$0")/.." && pwd)"
APP="${POPTB_APP:-PopulousTB}"
B="${POPTB_BIN:-$P/build}/$APP.app/Contents/MacOS/$APP"
GEN="${POPTB_GENDIR:-$P/gen}"
L=$1; shift
for pc in "$@"; do
  loc=$(atos -o "$B" -l "$L" "$pc" | sed -E 's/.*\(([^)]*)\)$/\1/')
  f=$(echo "$loc" | cut -d: -f1); n=$(echo "$loc" | cut -d: -f2)
  if [ -f "$GEN/$f" ] && [ "$n" -gt 0 ] 2>/dev/null; then
    lbl=$(head -n "$n" "$GEN/$f" | grep -o 'L_[0-9a-f]*:' | tail -1)
    echo "$pc -> $loc -> $lbl :: $(sed -n "${n}p" "$GEN/$f" | cut -c1-200)"
  else echo "$pc -> $loc"; fi
done
