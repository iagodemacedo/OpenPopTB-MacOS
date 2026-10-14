#!/bin/sh
# Builds the downloadable release: dist/OpenPopTB-MacOS-<version>.zip with "Populous TB.app" and
# "Populous UW.app". The apps contain only our code (runtime + recompiler) and third-party tools; the
# game code is generated from the player's copy on first launch (runtime/prepare.c). Never pass a
# game folder here: nothing in the release may come from the game.
# Needs: cmake ninja sdl3 innoextract (brew), python3 with pip, Xcode Command Line Tools.
# usage: tools/package_release.sh
set -e
P="$(cd "$(dirname "$0")/.." && pwd)"
VER=$(sed -n 's/^project(openpoptb VERSION \([0-9.]*\).*/\1/p' "$P/CMakeLists.txt")
B="$P/build-pre"
OUT="$P/dist/release"
CAPSTONE=capstone==5.0.7     # generated code was verified identical with this version
PEFILE=pefile==2024.8.26

cmake -S "$P" -B "$B" -G Ninja -DCMAKE_BUILD_TYPE=Release -DPOPTB_PREBUILT=ON >/dev/null
ninja -C "$B" poptb poptbuw
rm -rf "$OUT"; mkdir -p "$OUT"

# Python side of the recompiler, run on the player's Mac by the Command Line Tools' python3.
# capstone/pefile are pure Python (capstone loads its own dylib through ctypes), so any python3 works.
PY="$OUT/recomp"
mkdir -p "$PY/seeds"
cp "$P/recomp/gen.py" "$P/recomp/image.py" "$PY/"
cp "$P/re/export/functions.tsv" "$PY/seeds/popTB.exe.tsv"   # function addresses only, no code
python3 -m venv "$OUT/venv"   # a venv: Homebrew's python3 refuses pip installs (PEP 668)
"$OUT/venv/bin/pip" install -q --no-compile --target "$OUT/pydeps" --platform macosx_11_0_arm64 \
  --only-binary=:all: --python-version 3.9 "$CAPSTONE" "$PEFILE"
cp -R "$OUT/pydeps/capstone" "$OUT/pydeps/pefile.py" "$OUT/pydeps/ordlookup" "$OUT/pydeps/peutils.py" "$PY/"
/usr/bin/python3 -I -c "import sys; sys.path.insert(0, '$PY'); import capstone, pefile; capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)" \
  || { echo "vendored capstone/pefile do not load with /usr/bin/python3" >&2; exit 1; }
rm -rf "$PY/capstone/include"

# third-party notices
NOTICES="$OUT/THIRD_PARTY_NOTICES.txt"
{
  echo "OpenPopTB-MacOS $VER bundles the following third-party software."
  for lic in "$OUT"/pydeps/capstone-*.dist-info/LICENSE* "$OUT"/pydeps/pefile-*.dist-info/LICENSE* \
             "$(brew --prefix innoextract)/LICENSE" "$(brew --prefix sdl3)/LICENSE.txt"; do
    [ -f "$lic" ] || continue
    printf '\n==== %s ====\n' "$(basename "$(dirname "$lic")")/$(basename "$lic")"; cat "$lic"
  done
  printf '\n==== boost, xz, zstd (used by innoextract) ====\n'
  echo "Boost Software License 1.0 (https://www.boost.org/LICENSE_1_0.txt); xz/liblzma: 0BSD/public domain;"
  echo "zstd: BSD license (https://github.com/facebook/zstd/blob/dev/LICENSE)."
} > "$NOTICES"

# install_name_tool, quiet about the signature it invalidates (everything is re-signed at the end)
nametool() { out=$(install_name_tool "$@" 2>&1) || { echo "$out" >&2; exit 1; }; }

# copies every non-system dylib a binary needs into Frameworks/ and points the binary at the copies
bundle_deps() {  # bundle_deps <binary> <Frameworks dir> <prefix for the binary's references>
  for dep in $(otool -L "$1" | awk 'NR>1{print $1}'); do
    case "$dep" in /usr/lib/*|/System/*) continue;; esac
    base=$(basename "$dep")
    [ "$base" = "$(basename "$1")" ] && continue   # the library's own install name
    case "$dep" in
      @*) src=""
          for d in "$(dirname "$1")" "$(brew --prefix)/lib"; do [ -f "$d/$base" ] && src="$d/$base" && break; done
          [ -n "$src" ] || src=$(find "$(brew --prefix)/opt/"*/lib -name "$base" 2>/dev/null | head -1);;
      *) src="$dep";;
    esac
    [ -f "$src" ] || { echo "cannot find $dep (needed by $1)" >&2; exit 1; }
    nametool -change "$dep" "$3/$base" "$1"
    if [ ! -f "$2/$base" ]; then   # (sh has no local variables: recurse last)
      cp "$src" "$2/$base"; chmod u+w "$2/$base"
      nametool -id "@loader_path/$base" "$2/$base"
      bundle_deps "$2/$base" "$2" "@loader_path"
    fi
  done
}

make_app() {  # make_app <cmake target output> <display name>
  APP="$OUT/$2.app"
  cp -R "$B/$1.app" "$APP"
  FW="$APP/Contents/Frameworks"; RES="$APP/Contents/Resources"
  mkdir -p "$FW" "$RES/include"
  bundle_deps "$APP/Contents/MacOS/$1" "$FW" "@executable_path/../Frameworks"
  install_name_tool -add_rpath @executable_path/../Frameworks "$APP/Contents/MacOS/$1" 2>/dev/null || true
  # innoextract: lets the player pick the GOG installer instead of an extracted folder
  cp "$(brew --prefix innoextract)/bin/innoextract" "$APP/Contents/MacOS/innoextract"
  chmod u+w "$APP/Contents/MacOS/innoextract"
  bundle_deps "$APP/Contents/MacOS/innoextract" "$FW" "@executable_path/../Frameworks"
  cp -R "$PY" "$RES/recomp"
  cp "$P/runtime/cpu.h" "$P/runtime/rt_ops.h" "$P/runtime/table.h" "$RES/include/"
  cp "$NOTICES" "$P/LICENSE" "$RES/"
  PL="$APP/Contents/Info.plist"
  /usr/libexec/PlistBuddy -c "Add :NSHighResolutionCapable bool true" "$PL" 2>/dev/null || true
  # the oldest macOS the app runs on is the newest minimum among its binaries (Homebrew bottles are
  # built for the machine's macOS, so a release built on macOS 14 needs macOS 14)
  MINOS=$(for f in "$APP/Contents/MacOS/"* "$FW/"*.dylib; do vtool -show-build "$f" 2>/dev/null | awk '$1=="minos"{print $2}'; done | sort -t. -k1,1n -k2,2n | tail -1)
  /usr/libexec/PlistBuddy -c "Add :LSMinimumSystemVersion string $MINOS" "$PL" 2>/dev/null || \
    /usr/libexec/PlistBuddy -c "Set :LSMinimumSystemVersion $MINOS" "$PL"
  echo "minimum macOS: $MINOS"
  /usr/libexec/PlistBuddy -c "Set :CFBundleVersion $VER" "$PL" 2>/dev/null || true
  codesign --force --deep -s - "$APP"
  echo "done: $APP"
}
# the runtime must export what the generated code calls: link a tiny generated-style file against it
check_loader() {
  T=$(mktemp -d)
  printf '#include "funcs.h"\n#include "table.h"\nvoid f_401000(CPU *R) { TR(0x401000); hook_enter(R, 0); dispatch(R, R->eax); div_trap(R, 0); rep_movs(R, 4, 1); rep_stos(R, 4, 1); rep_scas(R, 1, 1); rep_cmps(R, 1, 1); do_lods(R, 1); fpush(R, frnd(R, rdf80(0))); wrf80(0, (double)fist(R, 1.0, 32)); fxam(R); fnsave(R, 0); frstor(R, 0); rt_cpuid(R); R->eax = (uint32_t)rt_rdtsc(); rt_trap(R, 0, ""); YIELD(); hook_leave(R, 0); }\nconst FnEntry gen_fn_table[] = {{0x401000u, f_401000}};\nconst int gen_fn_table_len = 1;\nconst ImportEntry gen_import_table[] = {{0, "", ""}};\nconst int gen_import_table_len = 1;\n' > "$T/t.c"
  printf '#pragma once\n#include "cpu.h"\n#include "rt_ops.h"\nvoid hook_enter(CPU *R, uint32_t f);\nvoid hook_leave(CPU *R, uint32_t f);\nextern int trace_on;\nvoid trace_rec(CPU *R, uint32_t a);\n#define TR(a) do { if (trace_on) trace_rec(R, a); } while (0)\n' > "$T/funcs.h"
  xcrun clang -c -arch arm64 -std=gnu11 -O1 -w -I"$P/runtime" -I"$T" "$T/t.c" -o "$T/t.o"
  xcrun clang -bundle -arch arm64 -bundle_loader "$1" "$T/t.o" -o "$T/t.bundle"
  rm -rf "$T"
}
check_loader "$B/PopulousTB.app/Contents/MacOS/PopulousTB"

make_app PopulousTB "Populous TB"
make_app PopulousUW "Populous UW"
rm -rf "$OUT/pydeps" "$OUT/venv" "$PY" "$NOTICES"

# guard: no game-derived file may end up in the release
if find "$OUT" \( -iname '*.exe' -o -iname '*.dll' -o -name 'f[0-9][0-9].c' -o -name 'funcs.h' -o -name 'table.c' -o -name '*.bundle' \) | grep .; then
  echo "game-derived files found in the release, aborting" >&2; exit 1
fi

cp "$P/README.md" "$OUT/README.md"
ZIP="$P/dist/OpenPopTB-MacOS-$VER.zip"
rm -f "$ZIP"
(cd "$OUT" && zip -qry "$ZIP" "Populous TB.app" "Populous UW.app" README.md)
echo "release: $ZIP"
