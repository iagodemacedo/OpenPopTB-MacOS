#!/bin/sh
# Builds self-contained dist/Populous TB.app, Populous UW.app and Populous TB (Direct3D).app (SDL3 bundled, ad-hoc signed).
# usage: tools/package.sh [game folder, used for the app icons]
set -e
P="$(cd "$(dirname "$0")/.." && pwd)"
GAME="${1:-${POPTB_GAME:-$(cd "$P/.." && pwd)}}"
cmake -S "$P" -B "$P/build-rel" -G Ninja -DCMAKE_BUILD_TYPE=Release >/dev/null
TARGETS=poptb
ls "$P"/gen_uw/*.c >/dev/null 2>&1 && TARGETS="$TARGETS poptbuw"
ls "$P"/gen_d3d/*.c >/dev/null 2>&1 && TARGETS="$TARGETS poptbd3d"
ninja -C "$P/build-rel" $TARGETS
mkdir -p "$P/dist"
make_app() {  # make_app <.app target> <display name> <.ico icon>
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
ICO="$GAME/$3"
if [ -f "$ICO" ]; then
  T=$(mktemp -d); mkdir "$T/icon.iconset"
  sips -s format png "$ICO" --out "$T/base.png" >/dev/null 2>&1 || true
  if [ -f "$T/base.png" ]; then
    for s in 16 32 128 256 512; do
      sips -z $s $s "$T/base.png" --out "$T/icon.iconset/icon_${s}x${s}.png" >/dev/null
      d=$((s*2)); sips -z $d $d "$T/base.png" --out "$T/icon.iconset/icon_${s}x${s}@2x.png" >/dev/null
    done
    iconutil -c icns "$T/icon.iconset" -o "$APP/Contents/Resources/AppIcon.icns" && \
      /usr/libexec/PlistBuddy -c "Add :CFBundleIconFile string AppIcon" "$APP/Contents/Info.plist" 2>/dev/null || true
  fi
  rm -rf "$T"
fi
/usr/libexec/PlistBuddy -c "Add :NSHighResolutionCapable bool true" "$APP/Contents/Info.plist" 2>/dev/null || true
/usr/libexec/PlistBuddy -c "Add :LSMinimumSystemVersion string 12.0" "$APP/Contents/Info.plist" 2>/dev/null || true
codesign --force --deep -s - "$APP" 2>/dev/null
echo "done: $APP"
}
make_app PopulousTB "Populous TB" goggame-1207659027.ico
[ -d "$P/build-rel/PopulousUW.app" ] && make_app PopulousUW "Populous UW" goggame-1207662583.ico
[ -d "$P/build-rel/PopulousTBD3D.app" ] && make_app PopulousTBD3D "Populous TB (Direct3D)" goggame-1207659027.ico
true
