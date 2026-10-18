// DirectDraw surface model shared by ddraw.c and the Direct3D layer (d3d.c).
#pragma once
#include "rt.h"

// first dwords of the Direct3D IIDs the game may ask for. The DX3 interfaces (IDirect3D 3BBA0080, IDirect3DTexture 2CDCD9E0)
// have different method tables and are refused, so a client falls back to the DX5 ones.
#define IID_IDIRECT3D2 0x6AAE1EC1u
#define IID_IDIRECT3DTEXTURE2 0x93281502u

typedef struct Pal { uint32_t obj; uint32_t rgb[256]; uint8_t raw[1024]; } Pal;
typedef struct Surf {
    uint32_t obj;
    int w, h, pitch, bpp;
    uint32_t mem, caps;
    int primary;
    uint32_t back;      // attached back buffer surface
    uint32_t zbuf;      // attached z-buffer surface
    uint32_t pal;
    uint32_t ck_lo, ck_hi; int has_ck;
    int locked;
    // explicit pixel format (textures): DDPF_* flags and channel masks
    uint32_t pf_flags, rmask, gmask, bmask, amask;
    uint32_t d3dtex;    // IDirect3DTexture2 object wrapping this surface
    uint32_t gen;       // bumped on every CPU-side write (and colour key change), so the GPU copy knows to refresh
    int gpu_slot;       // 1 + index of this texture's entry in the GPU backend's cache (0: none)
} Surf;

extern uint32_t g_dd_obj;
extern uint32_t g_pal_gen;   // bumped whenever any palette changes
static inline Surf *dd_surf(uint32_t obj) { return com_native(obj); }

// d3d.c
uint32_t d3d_create_interface(void);          // IDirect3D2 for QueryInterface on IDirectDraw
uint32_t d3d_texture_object(Surf *s);         // IDirect3DTexture2 for QueryInterface on a surface
