#!/bin/sh
# Builds the WebAssembly version (headless, runs under Node.js): build-wasm/PopulousTB.js + .wasm
# Needs gen/ (run build.sh or recomp/gen.py first) and the Emscripten SDK:
#   emcc in PATH, or EMSDK=/path/to/emsdk (its emsdk_env.sh is sourced).
#   emsdk needs Python >= 3.10; on macOS: EMSDK_PYTHON=/opt/homebrew/bin/python3.13
# usage: tools/build_wasm.sh [Debug|RelWithDebInfo|Release]   (default RelWithDebInfo)
set -e
P="$(cd "$(dirname "$0")/.." && pwd)"
if ! command -v emcc >/dev/null; then
  [ -n "$EMSDK" ] && [ -f "$EMSDK/emsdk_env.sh" ] || { echo "emcc not found: set EMSDK=/path/to/emsdk" >&2; exit 1; }
  . "$EMSDK/emsdk_env.sh" >/dev/null 2>&1
fi
ls "$P"/gen/*.c >/dev/null 2>&1 || { echo "gen/ is empty: generate the C first (build.sh)" >&2; exit 1; }
emcmake cmake -S "$P" -B "$P/build-wasm" -G Ninja -DCMAKE_BUILD_TYPE="${1:-RelWithDebInfo}" >/dev/null
ninja -C "$P/build-wasm" poptb
