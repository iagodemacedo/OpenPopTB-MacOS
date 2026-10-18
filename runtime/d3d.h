// Interface between the Direct3D COM layer (d3d.c) and the GPU backend (d3d_gpu.c).
#pragma once
#include "ddraw.h"

#define D3D_NRS 64   // render states tracked (D3DRENDERSTATETYPE values below 64)

// ---- render state numbers (D3DRENDERSTATETYPE, DX5) ----
enum {
    RS_TEXTUREHANDLE = 1, RS_ANTIALIAS = 2, RS_TEXTUREADDRESS = 3, RS_TEXTUREPERSPECTIVE = 4, RS_ZENABLE = 7,
    RS_FILLMODE = 8, RS_SHADEMODE = 9, RS_ZWRITEENABLE = 14, RS_ALPHATESTENABLE = 15, RS_TEXTUREMAG = 17,
    RS_TEXTUREMIN = 18, RS_SRCBLEND = 19, RS_DESTBLEND = 20, RS_TEXTUREMAPBLEND = 21, RS_CULLMODE = 22,
    RS_ZFUNC = 23, RS_ALPHAREF = 24, RS_ALPHAFUNC = 25, RS_DITHERENABLE = 26, RS_ALPHABLENDENABLE = 27,
    RS_FOGENABLE = 28, RS_SPECULARENABLE = 29, RS_COLORKEYENABLE = 41, RS_TEXTUREADDRESSU = 44,
    RS_TEXTUREADDRESSV = 45, RS_ZBIAS = 47,
};

// D3DTLVERTEX: pre-transformed vertex in screen space
typedef struct D3DTLVertex {
    float sx, sy, sz, rhw;
    uint32_t color, specular;
    float tu, tv;
} D3DTLVertex;

void d3d_default_states(uint32_t *rs);
bool d3d_gpu_init(void);
void d3d_gpu_begin_scene(Surf *target);
void d3d_gpu_end_scene(Surf *target);
void d3d_gpu_set_viewport(int x, int y, int w, int h);
void d3d_gpu_set_texture(Surf *tex);                 // NULL: untextured
void d3d_gpu_set_state(uint32_t state, uint32_t value);
void d3d_gpu_clear(int color, int depth, const float rgba[4], const int rect[4]);
// prim: D3DPRIMITIVETYPE; idx may be NULL
void d3d_gpu_draw(uint32_t prim, const D3DTLVertex *v, int nverts, const uint16_t *idx, int nidx);

// presentation through the shared SDL_GPU device (plat.c)
struct SDL_Window;
bool gpu_window_init(struct SDL_Window *w);
void gpu_present(const uint32_t *rgba, int w, int h, const uint8_t *pix565, int pitch);
