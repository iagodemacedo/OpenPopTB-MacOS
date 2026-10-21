// DirectDraw surfaces and palettes shared with the Direct3D implementation.
#pragma once
#include "rt.h"

#define COMFN(name, nargs) \
    static uint32_t name##_i(CPU *R); \
    static void name(CPU *R) { R->eax = name##_i(R); R->esp += 4 + 4 * ((nargs) + 1); } \
    static uint32_t name##_i(CPU *R)
#define THIS ARG(0)

typedef struct Pal { uint32_t obj; uint32_t rgb[256]; uint8_t raw[1024]; } Pal;
typedef struct Surf {
    uint32_t obj;
    int w, h, pitch, bpp;
    uint32_t mem, caps;
    uint32_t pf_flags, mask[4];   // DDPIXELFORMAT flags and R, G, B, A masks
    uint32_t gen;                 // bumped whenever the pixels or palette may have changed
    uint32_t tex_gen;             // gen of the copy uploaded to the GPU
    int primary;
    uint32_t back;      // attached back buffer surface
    uint32_t pal;
    uint32_t ck_lo, ck_hi; int has_ck;
    int locked;
} Surf;

void com_addref(CPU *R);
void com_release(CPU *R);
uint32_t d3d_create(void);
uint32_t d3d_texture_of(uint32_t surf);
int d3d_present(const Surf *s);   // shows a Direct3D frame; 0 if s is not one
