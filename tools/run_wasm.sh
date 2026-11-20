#!/bin/sh
# Runs the WebAssembly build under Node.js with a time limit, like tools/run.sh:
#   tools/run_wasm.sh <seconds> [args...]      e.g. --script "31000:click:320,302" --shot-dir /tmp/shots --shot-every 600
# env: POPTB_GAME (game folder), POPTB_BIN (build dir, default build-wasm/), POPTB_STATS=1 etc.
# Node.js 22+ (WebAssembly threads). The game folder is read directly from disk (NODERAWFS).
P="$(cd "$(dirname "$0")/.." && pwd)"
GAME="${POPTB_GAME:-$(cd "$P/.." && pwd)}"
t=$1; shift
node "${POPTB_BIN:-$P/build-wasm}/PopulousTB.js" --data "$GAME" "$@" &
pid=$!
( sleep "$t"; kill -9 $pid 2>/dev/null ) &
w=$!
wait $pid
rc=$?
kill $w 2>/dev/null
exit $rc
