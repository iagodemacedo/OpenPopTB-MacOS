#!/bin/sh
# Plays the same scripted path into the first tutorial with the software and Direct3D builds and
# writes a side-by-side image: tools/d3d_compare.sh <out dir> [seconds]
# The D3D shot is the window (full output resolution); out/cmp.png puts both at 640x480,
# out/zoom_sw.png and out/zoom_d3d.png crop the same region at 4x.
P="$(cd "$(dirname "$0")/.." && pwd)"
O="$1"; t="${2:-34}"
[ -n "$O" ] || { echo "usage: $0 <out dir> [seconds]"; exit 2; }
cd "$P" || exit 1
export SDL_AUDIO_DRIVER=dummy
ninja -C build PopulousTB PopulousTBD3D 2>&1 | grep -E "error|warning" && exit 1
SCRIPT="3000:key:Escape;5000:key:Escape;7000:key:Escape;9000:key:Escape;12000:click:320,152;16000:click:320,240;20000:key:Return;24000:click:320,240;27000:click:340,330"
rm -rf "$O/sw" "$O/d3d"; mkdir -p "$O/sw" "$O/d3d"
POPTB_GAME="$P/game" tools/run.sh "$t" --shot-dir "$O/sw" --shot-every 300 --script "$SCRIPT" > "$O/sw.log" 2>&1 < /dev/null
POPTB_SHOT_WINDOW=1 POPTB_GAME="$P/game" POPTB_APP=PopulousTBD3D tools/run.sh "$t" --shot-dir "$O/d3d" --shot-every 300 --script "$SCRIPT" > "$O/d3d.log" 2>&1 < /dev/null
a="$O/sw/$(ls "$O/sw" | tail -1)"; b="$O/d3d/$(ls "$O/d3d" | tail -1)"
echo "sw:  $a"; echo "d3d: $b"
"$P/.venv/bin/python" -I - "$a" "$b" "$O" <<'EOF'
import sys
from PIL import Image
a = Image.open(sys.argv[1]).convert('RGB'); b = Image.open(sys.argv[2]).convert('RGB'); o = sys.argv[3]
print('sizes', a.size, b.size)
c = Image.new('RGB', (1280, 480))
c.paste(a.resize((640, 480)), (0, 0)); c.paste(b.resize((640, 480), Image.LANCZOS), (640, 0))
c.save(o + '/cmp.png')
k = b.size[0] / a.size[0]
box = (345, 250, 395, 288)
a.crop(box).resize((800, 608), Image.NEAREST).save(o + '/zoom_sw.png')
b.crop(tuple(int(v * k) for v in box)).resize((800, 608), Image.NEAREST).save(o + '/zoom_d3d.png')
EOF
