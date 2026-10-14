#include "level.h"
#include "fs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Layout from FUN_0040c330; see docs/formats.md.
#define DAT_SIZE 192137
#define OFF_HEIGHT 0x0
#define OFF_MAPC 0x10000
#define OFF_TRIBES 0x14000
#define OFF_MISC 0x14040

int level_load(Level *lv, int number) {
    memset(lv, 0, sizeof *lv);
    char path[64];
    size_t n = 0;

    snprintf(path, sizeof path, "levels/levl2%03d.hdr", number);
    uint8_t *hdr = fs_load(path, &n);
    if (!hdr || n < 0x61) { free(hdr); return 0; }
    snprintf(lv->name, sizeof lv->name, "%.20s", (const char *)hdr + 0x38);
    lv->num_tribes = hdr[0x58];
    lv->theme = hdr[0x60];
    free(hdr);

    snprintf(path, sizeof path, "levels/levl2%03d.dat", number);
    uint8_t *dat = fs_load(path, &n);
    if (!dat || n != DAT_SIZE) {
        fprintf(stderr, "level: %s has %zu bytes, expected %d\n", path, n, DAT_SIZE);
        free(dat);
        return 0;
    }
    for (int i = 0; i < MAP_W * MAP_H; i++) {
        Cell *c = &lv->cells[i];
        c->height = (int16_t)(dat[OFF_HEIGHT + i * 2] | (dat[OFF_HEIGHT + i * 2 + 1] << 8));
        if (dat[OFF_MAPC + i]) c->flags |= 4;
    }
    lv->start_angle = dat[OFF_MISC + 2];
    free(dat);

    level_compute_slopes(lv);
    level_compute_light(lv, 0x93, 0x93, 0x93);  // FUN_00453760 defaults
    return 1;
}

void level_compute_slopes(Level *lv) {
    // Neighbour order matches the decompiled stack layout (local_58..local_38),
    // so the "lowest neighbour" index stored in dirnib is bit-compatible.
    static const int nb[9][2] = {
        {0, 0}, {0, 1}, {1, 0}, {0, -1}, {-1, 0}, {1, 1}, {1, -1}, {-1, -1}, {-1, 1},
    };
    for (int cz = 0; cz < MAP_H; cz++) {
        for (int cx = 0; cx < MAP_W; cx++) {
            Cell *c = level_cell(lv, cx, cz);
            int h00 = c->height;
            int h01 = level_cell(lv, cx, cz + 1)->height;
            int h10 = level_cell(lv, cx + 1, cz)->height;
            int h11 = level_cell(lv, cx + 1, cz + 1)->height;

            // Split the quad along the diagonal through the corner that deviates most.
            int avg = (h00 + h01 + h10 + h11) >> 2;
            int d00 = abs(avg - h00), d01 = abs(avg - h01), d11 = abs(avg - h11), d10 = abs(avg - h10);
            int m = d00 > 0 ? d00 : 0;
            if (m < d01) m = d01;
            if (m < d10) m = d10;
            if (m < d11) m = d11;
            c->flags = (m == d00 || m == d11) ? (c->flags | 0x11) : ((c->flags & ~1u) | 0x10);

            int hmax = 0, hmin = 0x401, imin = 0;
            for (int k = 0; k < 9; k++) {
                int h = level_cell(lv, cx + nb[k][0], cz + nb[k][1])->height;
                if (hmax < h) hmax = h;
                if (h < hmin) { hmin = h; imin = k; }
            }
            c->dirnib = (uint8_t)((c->dirnib & 0xf) | (imin << 4));

            // flags byte 3 bit 2 (0x04000000) pins the slope; never set by the loader.
            int pinned = (c->flags & 0x04000000) != 0;
            if (hmax == 0) {
                if (!pinned) c->slope = 0;
            } else {
                if (!pinned) {
                    int s = (hmax - hmin) >> 3;
                    c->slope = (uint8_t)(s > 127 ? 127 : s < 1 ? 1 : s);
                }
                if (hmin < 1 || hmax > 0x1ff) c->flags &= ~0x20u;
                else c->flags |= 0x20u;
            }
            // Simplified: the original also clears this when a neighbour has flag 0x200
            // or a 4x3 ring exceeds 7/8 of DAT_005a3160 (not yet identified).
            if (h00 == 0 && h01 == 0 && h10 == 0 && h11 == 0) c->flags |= 0x1000000;
            else c->flags &= ~0x1000000u;
        }
    }
}

void level_compute_light(Level *lv, int l0, int l1, int l2) {
    for (int cz = 0; cz < MAP_H; cz++) {
        for (int cx = 0; cx < MAP_W; cx++) {
            Cell *c = level_cell(lv, cx, cz);
            int h = c->height;
            int hz = level_cell(lv, cx, cz + 1)->height;
            int hx = level_cell(lv, cx + 1, cz)->height;
            int v = (l2 + (hz - h) * l1 - l0 * (h - hx)) / 350 + ((c->dirnib & 0xf) + 8) * 16;
            c->light = (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v);
        }
    }
}
