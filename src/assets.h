#pragma once
#include <stdint.h>

// data/palN-X.dat: 256 entries of {R,G,B,0}. X is the level "theme" letter
// (0-9, a-z) shared with sky/BL320/fade/etc.
typedef struct { uint8_t r, g, b, a; } PalEntry;

typedef struct {
    PalEntry pal[256];
    uint8_t *sky;      // 640x480 8-bit, data/sky0-X.dat
    uint8_t *blocks;   // 256x1024 8-bit texture atlas (64 tiles of 32x32? see notes), data/BL320-X.DAT
    uint8_t *fade;     // 64x256 shade table, data/fade0-X.dat
} Theme;

int  theme_load(Theme *t, char key);
void theme_free(Theme *t);
