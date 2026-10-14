#!/bin/sh
# Runs the port with a time limit: tools/run.sh <seconds> [args...]
# env: POPTB_GAME (game folder), POPTB_BIN (build dir, default build/), POPTB_APP (PopulousTB | PopulousUW)
P="$(cd "$(dirname "$0")/.." && pwd)"
GAME="${POPTB_GAME:-$(cd "$P/.." && pwd)}"
APP="${POPTB_APP:-PopulousTB}"
t=$1; shift
"${POPTB_BIN:-$P/build}/$APP.app/Contents/MacOS/$APP" --data "$GAME" "$@" &
pid=$!
( sleep "$t"; kill -9 $pid 2>/dev/null ) &
w=$!
wait $pid
rc=$?
kill $w 2>/dev/null
exit $rc
