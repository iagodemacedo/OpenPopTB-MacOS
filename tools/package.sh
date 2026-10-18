#!/bin/sh
# Builds self-contained dist/Populous TB.app, Populous UW.app and Populous TB D3D.app, Populous UW D3D.app (SDL3 bundled, ad-hoc signed).
# usage: tools/package.sh [game folder, used for the app icons]
set -e
P="$(cd "$(dirname "$0")/.." && pwd)"
GAME="${1:-${POPTB_GAME:-$(cd "$P/.." && pwd)}}"
cmake -S "$P" -B "$P/build-rel" -G Ninja -DCMAKE_BUILD_TYPE=Release >/dev/null
TARGETS=poptb
ls "$P"/gen_uw/*.c >/dev/null 2>&1 && TARGETS="$TARGETS poptbuw"
ls "$P"/gen_d3d/*.c >/dev/null 2>&1 && TARGETS="$TARGETS poptbd3d"
ls "$P"/gen_d3duw/*.c >/dev/null 2>&1 && TARGETS="$TARGETS poptbd3duw"
ninja -C "$P/build-rel" $TARGETS
mkdir -p "$P/dist"
make_app() {  # make_app <.app target> <display name> <game exe> <.ico candidates, relative to the game folder; the first that exists wins>
APP="$P/dist/$2.app"
rm -rf "$APP"
cp -R "$P/build-rel/$1.app" "$APP"
mkdir -p "$APP/Contents/Frameworks" "$APP/Contents/Resources"
SDL=$(otool -L "$APP/Contents/MacOS/$1" | awk '/libSDL3/{print $1}')
if [ "${SDL#@rpath}" = "$SDL" ]; then
  cp "$SDL" "$APP/Contents/Frameworks/libSDL3.0.dylib"
  chmod u+w "$APP/Contents/Frameworks/libSDL3.0.dylib"
  install_name_tool -id @rpath/libSDL3.0.dylib "$APP/Contents/Frameworks/libSDL3.0.dylib" 2>/dev/null
  install_name_tool -change "$SDL" @rpath/libSDL3.0.dylib "$APP/Contents/MacOS/$1" 2>/dev/null
  install_name_tool -add_rpath @executable_path/../Frameworks "$APP/Contents/MacOS/$1" 2>/dev/null || true
fi
T=$(mktemp -d)
ICO=""
for c in "$4" "$5"; do [ -f "$GAME/$c" ] && { ICO="$GAME/$c"; break; }; done
# no icon file in the game folder: take the one embedded in the exe (48 px at most)
if [ -z "$ICO" ] && [ -x "$P/.venv/bin/python" ] && "$P/.venv/bin/python" -I "$P/tools/exe_icon.py" "$GAME/$3" "$T/exe.ico" 2>/dev/null; then
  ICO="$T/exe.ico"
fi
if [ -n "$ICO" ]; then
  mkdir "$T/icon.iconset"
  sips -s format png "$ICO" --out "$T/base.png" >/dev/null 2>&1 || true
  if [ -f "$T/base.png" ]; then
    for s in 16 32 128 256 512; do
      sips -z $s $s "$T/base.png" --out "$T/icon.iconset/icon_${s}x${s}.png" >/dev/null
      d=$((s*2)); sips -z $d $d "$T/base.png" --out "$T/icon.iconset/icon_${s}x${s}@2x.png" >/dev/null
    done
    iconutil -c icns "$T/icon.iconset" -o "$APP/Contents/Resources/AppIcon.icns" && \
      { /usr/libexec/PlistBuddy -c "Set :CFBundleIconFile AppIcon" "$APP/Contents/Info.plist" 2>/dev/null ||
        /usr/libexec/PlistBuddy -c "Add :CFBundleIconFile string AppIcon" "$APP/Contents/Info.plist"; }
  fi
fi
rm -rf "$T"
/usr/libexec/PlistBuddy -c "Add :NSHighResolutionCapable bool true" "$APP/Contents/Info.plist" 2>/dev/null || true
/usr/libexec/PlistBuddy -c "Add :LSMinimumSystemVersion string 12.0" "$APP/Contents/Info.plist" 2>/dev/null || true
codesign --force --deep -s - "$APP" 2>/dev/null
echo "done: $APP"
}
# gfw_high*.ico: the game's own 256 px icons (2018 GOG installer); goggame-*.ico: older GOG installers
TB_ICON="gfw_high.ico goggame-1207659027.ico"
UW_ICON="gfw_high__addon.ico goggame-1207662583.ico"
make_app PopulousTB "Populous TB" popTB.exe $TB_ICON
[ -d "$P/build-rel/PopulousUW.app" ] && make_app PopulousUW "Populous UW" popTBUW.exe $UW_ICON
[ -d "$P/build-rel/PopulousD3D.app" ] && make_app PopulousD3D "Populous TB D3D" D3DPopTB.exe $TB_ICON
[ -d "$P/build-rel/PopulousD3DUW.app" ] && make_app PopulousD3DUW "Populous UW D3D" D3DPopTBUW.exe $UW_ICON
true
