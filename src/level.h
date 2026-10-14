#pragma once
#include <stdint.h>

#define MAP_W 128
#define MAP_H 128

// Mirrors the original 16-byte cell at 0x88897c (popTB.exe). Only fields
// whose meaning is confirmed in the decompile are named.
typedef struct {
    uint32_t flags;     // +0x0: 0x1 diag split, 0x10 slope done, 0x20 mid-height, 0x1000000 flat water
    int16_t  height;    // +0x4
    uint8_t  _pad6[4];  // +0x6
    uint8_t  slope;     // +0xa: (max-min)>>3 of 3x3 heights, 1..127; 0 if all flat at 0
    uint8_t  _padb[2];  // +0xb
    uint8_t  light;     // +0xd: per-cell brightness 0..255
    uint8_t  dirnib;    // +0xe: lo nibble = light bias, hi nibble = index of lowest neighbour
    uint8_t  _padf;
} Cell;

typedef struct {
    char    name[32];
    int     num_tribes;
    int     theme;        // 0..35, hdr offset 0x60
    int     start_angle;  // dat 0x14042
    Cell    cells[MAP_W * MAP_H];
} Level;

int  level_load(Level *lv, int number);   // loads levels/levl2NNN.{hdr,dat}

static inline Cell *level_cell(Level *lv, int cx, int cz) {
    return &lv->cells[(cz & (MAP_H - 1)) * MAP_W + (cx & (MAP_W - 1))];
}

// FUN_004e8450: slope, lowest-neighbour nibble and triangle split flag.
void level_compute_slopes(Level *lv);
// FUN_00487870 (lighting half): per-cell brightness from height gradient.
void level_compute_light(Level *lv, int l0, int l1, int l2);
