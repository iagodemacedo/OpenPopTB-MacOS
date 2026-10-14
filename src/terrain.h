#pragma once
#include <stdint.h>
#include "level.h"

#define TERRAIN_TEX_PER_CELL 8
#define TERRAIN_TEX_W (MAP_W * TERRAIN_TEX_PER_CELL)
#define TERRAIN_TEX_H (MAP_H * TERRAIN_TEX_PER_CELL)

// Per-theme lookup tables used by the software terrain texturer.
typedef struct {
    uint8_t *bigf;    // 0x48000: 1152 rows (height) x 256 cols (brightness)
    uint8_t *cliff;   // 0x2000: remap by slope, row = slope/4, 128 bytes/row
    uint8_t *disp;    // 0x10000: 256x256 signed noise, tiles every 8 cells
    int16_t  scale[0x480]; // noise amplitude per height row, built in FUN_00487230
} TerrainTables;

int  terrain_tables_load(TerrainTables *t, char theme_key);
void terrain_tables_free(TerrainTables *t);

// FUN_00488920 for every cell: writes an 8-bit indexed 1024x1024 image,
// cell (cx,cz) at pixel (cx*8, cz*8).
void terrain_build_texture(const TerrainTables *t, Level *lv, uint8_t *out);
