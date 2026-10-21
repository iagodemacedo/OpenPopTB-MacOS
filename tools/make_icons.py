#!/usr/bin/env python3
# Draws the app icons (an original design: a small world in space, nothing taken from the game) and
# writes assets/icons/<name>.icns. Needs Pillow and macOS iconutil.
# usage: python3 tools/make_icons.py [out dir]
import math, os, random, subprocess, sys, tempfile
from PIL import Image, ImageDraw, ImageFilter, ImageFont

N = 1024
BODY = 824          # macOS icon grid: 824 px body on a 1024 canvas
OUT = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(__file__), '..', 'assets', 'icons')

THEMES = {
    'tb':  dict(sky=((26, 38, 104), (6, 8, 28)), nebula=(120, 60, 160), glow=(255, 196, 92),
                ocean=((10, 44, 110), (34, 116, 190)), land=[(0.00, (222, 196, 120)), (0.05, (92, 148, 60)),
                (0.22, (60, 110, 46)), (0.40, (140, 118, 72)), (0.55, (236, 236, 230))], atmo=(110, 180, 255),
                moon=False, badge=None),
    'uw':  dict(sky=((40, 18, 80), (8, 4, 24)), nebula=(30, 140, 150), glow=(130, 255, 220),
                ocean=((20, 30, 90), (40, 150, 170)), land=[(0.00, (200, 170, 220)), (0.05, (120, 80, 170)),
                (0.22, (84, 54, 140)), (0.40, (170, 110, 160)), (0.55, (240, 225, 250))], atmo=(150, 255, 230),
                moon=True, badge=None),
}
THEMES['d3d'] = dict(THEMES['tb'], badge='3D')


# ---- 3D value noise (so continents wrap the sphere without seams) ----
rng = random.Random(7)
PERM = list(range(256)); rng.shuffle(PERM); PERM += PERM
VALS = [rng.random() for _ in range(256)]
def lattice(x, y, z): return VALS[PERM[PERM[PERM[x & 255] + (y & 255)] + (z & 255)]]
def smooth(t): return t * t * (3 - 2 * t)
def noise(x, y, z):
    xi, yi, zi = math.floor(x), math.floor(y), math.floor(z)
    xf, yf, zf = smooth(x - xi), smooth(y - yi), smooth(z - zi)
    def lerp(a, b, t): return a + (b - a) * t
    c = [[[lattice(xi + i, yi + j, zi + k) for k in (0, 1)] for j in (0, 1)] for i in (0, 1)]
    return lerp(lerp(lerp(c[0][0][0], c[0][0][1], zf), lerp(c[0][1][0], c[0][1][1], zf), yf),
                lerp(lerp(c[1][0][0], c[1][0][1], zf), lerp(c[1][1][0], c[1][1][1], zf), yf), xf)
def fbm(x, y, z):
    s, a, f = 0.0, 0.5, 1.0
    for _ in range(5):
        s += a * noise(x * f, y * f, z * f); a *= 0.5; f *= 2.03
    return s / 0.96875


def mix(a, b, t): return tuple(a[i] + (b[i] - a[i]) * t for i in range(3))
def clamp(v, lo=0.0, hi=1.0): return lo if v < lo else hi if v > hi else v
def ramp(stops, h):
    for (h0, c0), (h1, c1) in zip(stops, stops[1:]):
        if h < h1: return mix(c0, c1, clamp((h - h0) / (h1 - h0)))
    return stops[-1][1]


def squircle_mask():
    # superellipse |x|^5 + |y|^5 = 1, rendered at 4x for smooth edges
    s = 4; m = Image.new('L', (N * s, N * s), 0); px = m.load()
    r = BODY * s / 2; c = N * s / 2
    for y in range(N * s):
        dy = abs(y + 0.5 - c) / r
        if dy >= 1: continue
        w = r * (1 - dy ** 5) ** 0.2
        for x in range(int(c - w), int(c + w) + 1): px[x, y] = 255
    return m.resize((N, N), Image.LANCZOS)


def background(t):
    img = Image.new('RGB', (N, N)); px = img.load()
    c0, c1 = t['sky']; neb = t['nebula']
    for y in range(N):
        for x in range(N):
            d = math.hypot(x - 330, y - 260) / 900
            col = mix(c0, c1, clamp(d))
            n = math.hypot(x - 760, y - 300) / 360
            col = mix(col, neb, 0.28 * clamp(1 - n) ** 2)
            px[x, y] = tuple(int(v) for v in col)
    d = ImageDraw.Draw(img); r = random.Random(3)
    for _ in range(150):
        x, y = r.uniform(100, 924), r.uniform(100, 924); b = r.random() ** 3
        s = 1.2 + 2.6 * b; v = int(150 + 105 * b)
        d.ellipse((x - s, y - s, x + s, y + s), fill=(v, v, min(255, v + 20)))
    return img


def planet(t, cx, cy, R):
    # sphere layer with alpha; light from the upper left
    L = (-0.55, -0.6, 0.58); ln = math.sqrt(sum(v * v for v in L)); L = tuple(v / ln for v in L)
    layer = Image.new('RGBA', (N, N), (0, 0, 0, 0)); px = layer.load()
    rot = 0.6; cr, sr = math.cos(rot), math.sin(rot)
    for y in range(int(cy - R) - 2, int(cy + R) + 3):
        for x in range(int(cx - R) - 2, int(cx + R) + 3):
            dx, dy = (x + 0.5 - cx) / R, (y + 0.5 - cy) / R
            rr = dx * dx + dy * dy
            if rr > 1.0: continue
            dz = math.sqrt(1 - rr)
            # surface point in the planet's frame (tilted)
            sx, sy, sz = dx * cr - dz * sr, dy, dx * sr + dz * cr
            h = fbm(sx * 1.6 + 3, sy * 1.6 + 7, sz * 1.6 + 11) - 0.52
            diff = max(0.0, dx * L[0] + dy * L[1] + dz * L[2])
            if h < 0:
                col = mix(t['ocean'][0], t['ocean'][1], clamp(1 + h * 6))
                hx, hy, hz = L[0], L[1], L[2] + 1
                hn = math.sqrt(hx * hx + hy * hy + hz * hz)
                spec = max(0.0, (dx * hx + dy * hy + dz * hz) / hn) ** 60 * 0.9
            else:
                col = ramp(t['land'], h * 2.2); spec = 0.0
            shade = 0.12 + 0.95 * diff
            col = tuple(clamp((c / 255) * shade + spec) * 255 for c in col)
            # rim light: atmosphere seen edge-on
            fres = (1 - dz) ** 3
            col = mix(col, t['atmo'], 0.55 * fres * (0.35 + diff))
            a = clamp((1 - math.sqrt(rr)) * R * 1.5)   # anti-aliased edge
            px[x, y] = (int(col[0]), int(col[1]), int(col[2]), int(255 * a))
    return layer


def glow(color, cx, cy, R, width, strength):
    g = Image.new('RGBA', (N, N), (0, 0, 0, 0)); d = ImageDraw.Draw(g)
    for i in range(24, 0, -1):
        r = R + width * i / 24
        d.ellipse((cx - r, cy - r, cx + r, cy + r), fill=color + (int(strength * (1 - i / 24) ** 2 * 255),))
    return g.filter(ImageFilter.GaussianBlur(width / 3))


def badge(img, text):
    d = ImageDraw.Draw(img)
    w, h = 300, 168; x1, y1 = 512 + BODY / 2 - 40, 512 + BODY / 2 - 40; x0, y0 = x1 - w, y1 - h
    sh = Image.new('RGBA', (N, N), (0, 0, 0, 0))
    ImageDraw.Draw(sh).rounded_rectangle((x0, y0 + 10, x1, y1 + 10), 50, fill=(0, 0, 0, 140))
    img.alpha_composite(sh.filter(ImageFilter.GaussianBlur(14)))
    grad = Image.new('RGBA', (N, N), (0, 0, 0, 0)); gp = grad.load()
    for y in range(int(y0), int(y1) + 1):
        c = mix((255, 170, 60), (232, 84, 30), (y - y0) / h)
        for x in range(int(x0), int(x1) + 1): gp[x, y] = (int(c[0]), int(c[1]), int(c[2]), 255)
    m = Image.new('L', (N, N), 0); ImageDraw.Draw(m).rounded_rectangle((x0, y0, x1, y1), 50, fill=255)
    img.paste(grad, (0, 0), m)
    d.rounded_rectangle((x0, y0, x1, y1), 50, outline=(255, 230, 190), width=6)
    font = None
    for f in ('/System/Library/Fonts/SFNSRounded.ttf', '/System/Library/Fonts/SFNS.ttf', '/System/Library/Fonts/Helvetica.ttc'):
        try: font = ImageFont.truetype(f, 124); break
        except OSError: pass
    try: font.set_variation_by_name('Heavy')
    except Exception: pass
    d.text(((x0 + x1) / 2, (y0 + y1) / 2 + 4), text, font=font, fill=(255, 255, 255), anchor='mm')


def make(name, t):
    img = background(t).convert('RGBA')
    cx, cy, R = 512, 548, 268
    img.alpha_composite(glow(t['glow'], cx - 150, cy - 160, R * 0.55, 190, 0.85))   # sunrise behind the world
    img.alpha_composite(glow(t['atmo'], cx, cy, R, 46, 0.75))
    img.alpha_composite(planet(t, cx, cy, R))
    if t['moon']:
        img.alpha_composite(glow(t['atmo'], 770, 300, 46, 20, 0.5))
        img.alpha_composite(planet(dict(t, land=[(0, (190, 180, 200)), (1, (120, 110, 140))],
                                        ocean=((90, 80, 110), (150, 140, 170))), 770, 300, 46))
    if t['badge']: badge(img, t['badge'])
    out = Image.new('RGBA', (N, N), (0, 0, 0, 0))
    mask = squircle_mask()
    sh = Image.new('RGBA', (N, N), (0, 0, 0, 0)); sh.putalpha(mask.point(lambda v: v * 0.45))
    out.alpha_composite(sh.transform((N, N), Image.AFFINE, (1, 0, 0, 0, 1, -12)).filter(ImageFilter.GaussianBlur(16)))
    body = img.copy(); body.putalpha(mask)
    out.alpha_composite(body)
    os.makedirs(OUT, exist_ok=True)
    out.save(os.path.join(OUT, name + '.png'))
    with tempfile.TemporaryDirectory() as tmp:
        iset = os.path.join(tmp, 'icon.iconset'); os.mkdir(iset)
        for s in (16, 32, 128, 256, 512):
            out.resize((s, s), Image.LANCZOS).save(os.path.join(iset, f'icon_{s}x{s}.png'))
            out.resize((s * 2, s * 2), Image.LANCZOS).save(os.path.join(iset, f'icon_{s}x{s}@2x.png'))
        subprocess.run(['iconutil', '-c', 'icns', iset, '-o', os.path.join(OUT, name + '.icns')], check=True)
    print('wrote', os.path.join(OUT, name + '.icns'))


for name in (sys.argv[2:] or THEMES):
    make(name, THEMES[name])
