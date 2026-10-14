#include "terrain.h"
#include "fs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int terrain_tables_load(TerrainTables *t, char key) {
    memset(t, 0, sizeof *t);
    char name[64];
    size_t n;
    snprintf(name, sizeof name, "data/bigf0-%c.dat", key);
    t->bigf = fs_load(name, &n);
    if (!t->bigf || n < 0x48000) goto fail;
    snprintf(name, sizeof name, "data/cliff0-%c.dat", key);
    t->cliff = fs_load(name, &n);
    if (!t->cliff || n < 0x2000) goto fail;
    snprintf(name, sizeof name, "data/disp0-%c.dat", key);
    t->disp = fs_load(name, &n);
    if (!t->disp || n < 0x10000) goto fail;

    // FUN_00487230: walks byte offsets 0x8fe..0 with a value starting at 0xd3d, -3 per entry.
    int16_t v = 0xd3d;
    for (int off = 0x8fe; off >= 0; off -= 2) {
        int16_t s = off < 0x100 ? 0x140 : off < 0x2d5 ? v : 0x400;
        t->scale[off / 2] = s;
        v -= 3;
    }
    return 1;
fail:
    terrain_tables_free(t);
    return 0;
}

void terrain_tables_free(TerrainTables *t) {
    free(t->bigf);
    free(t->cliff);
    free(t->disp);
    memset(t, 0, sizeof *t);
}

typedef struct { int light, h, slope; } Corner;

static Corner corner(Level *lv, int cx, int cz) {
    const Cell *c = level_cell(lv, cx, cz);
    Corner k = { c->light, c->height + 0x4b, c->slope };
    if (c->slope) {
        k.h = c->height + 0x96;
        if (k.h > 0x3fe) k.h = 0x3fe;
    }
    return k;
}

// Fixed-point bilinear walk identical to FUN_00488920 (16.16, steps of 1/8).
static void texel_cell(const TerrainTables *t, Level *lv, int cx, int cz, uint8_t *dst, int pitch) {
    Corner c0 = corner(lv, cx, cz);          // top-left
    Corner c1 = corner(lv, cx + 1, cz);      // top-right
    Corner c2 = corner(lv, cx + 1, cz + 1);  // bottom-right
    Corner c3 = corner(lv, cx, cz + 1);      // bottom-left

    int dl = (c1.light - c0.light) * 0x2000, dh = (c1.h - c0.h) * 0x2000, ds = (c1.slope - c0.slope) * 0x2000;
    int l_row = c0.light << 16, h_row = c0.h << 16, s_row = c0.slope << 16;
    const int8_t *noise = (const int8_t *)t->disp + (cz & 7) * 0x2000 + (cx & 7) * 0x20;

    for (int y = 0; y < 8; y++) {
        int l = l_row, h = h_row, s = s_row;
        const int8_t *np = noise + y * 0x400;
        for (int x = 0; x < 8; x++) {
            int n = np[x * 4];
            int hi = h >> 16;
            int col = (n >> 2) + (l >> 16);
            if (col < 0) col = 0; else if (col > 255) col = 255;
            // (scale*n & 0xfffffc03) >> 2 == floor(scale*n / 1024) rows of 256.
            int row_off = ((t->scale[hi] * n) >> 10) * 256;
            int idx = row_off + hi * 256 + col;
            if (idx < 0) idx = 0; else if (idx >= 0x48000) idx = 0x47fff;
            int ci = t->bigf[idx] + (s >> 18) * 0x80;
            dst[y * pitch + x] = t->cliff[ci & 0x1fff];
            l += dl; h += dh; s += ds;
        }
        l_row += (c3.light - c0.light) * 0x2000;
        h_row += (c3.h - c0.h) * 0x2000;
        s_row += (c3.slope - c0.slope) * 0x2000;
        dl += ((c2.light - c3.light) * 0x2000 - (c1.light - c0.light) * 0x2000) >> 3;
        dh += ((c2.h - c3.h) * 0x2000 - (c1.h - c0.h) * 0x2000) >> 3;
        ds += ((c2.slope - c3.slope) * 0x2000 - (c1.slope - c0.slope) * 0x2000) >> 3;
    }
}

void terrain_build_texture(const TerrainTables *t, Level *lv, uint8_t *out) {
    for (int cz = 0; cz < MAP_H; cz++)
        for (int cx = 0; cx < MAP_W; cx++)
            texel_cell(t, lv, cx, cz, out + cz * 8 * TERRAIN_TEX_W + cx * 8, TERRAIN_TEX_W);
}
