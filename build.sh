#!/bin/sh
# One-shot build: recompiles YOUR copy of the game and produces dist/Populous TB.app (+ UW if present).
# usage: ./build.sh /path/to/Populous-The-Beginning   (folder that contains popTB.exe)
#        ./build.sh --installer setup_populous_the_beginning_*.exe   (GOG offline installer)
set -e
P="$(cd "$(dirname "$0")" && pwd)"
if [ "$1" = "--installer" ]; then
  [ -f "$2" ] || { echo "usage: ./build.sh --installer <setup_populous_the_beginning_*.exe>" >&2; exit 1; }
  command -v innoextract >/dev/null || { echo "missing 'innoextract' (brew install innoextract)" >&2; exit 1; }
  echo "== extracting the installer into game/"
  rm -rf "$P/game"
  innoextract --gog -p 0 -d "$P/game" "$2" >/dev/null
  set -- "$(dirname "$(find "$P/game" -maxdepth 3 -name popTB.exe | head -1)")"
fi
GAME="${1:-${POPTB_GAME:-}}"
if [ -z "$GAME" ] || [ ! -f "$GAME/popTB.exe" ]; then
  echo "usage: ./build.sh <game folder containing popTB.exe>" >&2
  echo "       ./build.sh --installer <setup_populous_the_beginning_*.exe>" >&2; exit 1
fi
GAME="$(cd "$GAME" && pwd)"

for t in cmake ninja python3 clang; do
  command -v $t >/dev/null || { echo "missing '$t' (brew install cmake ninja python sdl3; xcode-select --install)" >&2; exit 1; }
done
[ -d "$(brew --prefix sdl3 2>/dev/null)" ] || { echo "missing SDL3: brew install sdl3" >&2; exit 1; }

# known-good files (GOG release, v1.01). Other versions are untested: addresses in the runtime may not match.
check() {
  f="$GAME/$1"; [ -f "$f" ] || return 0
  h=$(shasum -a 256 "$f" | cut -d' ' -f1)
  [ "$h" = "$2" ] || echo "warning: $1 sha256 $h differs from the tested GOG build ($2)" >&2
}
check popTB.exe   a46e7c6fa04e13f6cd5accebe32bb5f543f1cd0808bb96b0040f59763251109e
check popTBUW.exe d994142ce0ffb0e5a12ad432cd5def2020d3ca9fb6027a1469d9f7459071e760
check QMixer.dll  460292fe61a75996e8035f8a65a765733738293d0d391336be292aa74c128698
check WEANETR.dll 44f5ea804323b607c68739d90df3e9516390f4045c2779350372566c2a806418

if [ ! -x "$P/.venv/bin/python" ]; then
  python3 -m venv "$P/.venv"
  "$P/.venv/bin/pip" install -q capstone pefile
fi
PY="$P/.venv/bin/python"

echo "== recompiling popTB.exe (+ QMixer.dll, WEANETR.dll)"
"$PY" -I "$P/recomp/gen.py" "$P/gen" "$GAME/popTB.exe:$P/re/export/functions.tsv" "$GAME/QMixer.dll" "$GAME/WEANETR.dll"
if [ -f "$GAME/popTBUW.exe" ]; then
  echo "== recompiling popTBUW.exe (Undiscovered Worlds)"
  "$PY" -I "$P/recomp/gen.py" "$P/gen_uw" "$GAME/popTBUW.exe" "$GAME/QMixer.dll" "$GAME/WEANETR.dll"
fi
if [ -f "$GAME/D3DPopTB.exe" ]; then
  echo "== recompiling D3DPopTB.exe (hardware-rendered build)"
  "$PY" -I "$P/recomp/gen.py" "$P/gen_d3d" "$GAME/D3DPopTB.exe" "$GAME/QMixer.dll" "$GAME/WEANETR.dll"
fi
if [ -f "$GAME/D3DPopTBUW.exe" ]; then
  echo "== recompiling D3DPopTBUW.exe (hardware-rendered Undiscovered Worlds)"
  "$PY" -I "$P/recomp/gen.py" "$P/gen_d3duw" "$GAME/D3DPopTBUW.exe" "$GAME/QMixer.dll" "$GAME/WEANETR.dll"
fi
echo "== building and packaging"
"$P/tools/package.sh" "$GAME"
echo
echo "Done. Open: open \"$P/dist/Populous TB.app\"  (first launch asks for the game folder)"
if [ -d "$P/dist/Populous TB D3D.app" ]; then
  echo "      Hi-res Direct3D build: open \"$P/dist/Populous TB D3D.app\""
fi
if [ -d "$P/dist/Populous UW D3D.app" ]; then
  echo "      Hi-res Direct3D Undiscovered Worlds: open \"$P/dist/Populous UW D3D.app\""
fi
