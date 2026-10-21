// Hardware renderer for the Direct3D build: D3D5 TL-vertex scenes on SDL_GPU, read back to the guest each frame.
#pragma once
#include <stdint.h>
// D3DTLVERTEX, 32 bytes, as the game writes it
typedef struct GpuVertex { float x, y, z, rhw; uint32_t diffuse, specular; float u, v; } GpuVertex;
enum { GPU_POINTS, GPU_TRIANGLES };
typedef struct GpuState {          // raw D3D5 render-state values
    uint32_t tex;                  // texture key from gpu_texture_upload, 0 = untextured
    uint8_t prim;                  // GPU_POINTS / GPU_TRIANGLES
    uint8_t zenable, zwrite, zfunc;            // D3DCMPFUNC 1..8
    uint8_t blend, srcblend, dstblend;         // D3DBLEND 1..13
    uint8_t alphatest, alphafunc, alpharef;    // alpharef 0..255
    uint8_t mapblend;              // D3DTBLEND: 1 DECAL, 2 MODULATE, 3 DECALALPHA, 4 MODULATEALPHA, 7 COPY, 8 ADD
    uint8_t specular, flat;        // SPECULARENABLE, SHADEMODE==FLAT
    uint8_t mag_linear, min_linear, mip;       // mip: 0 none, 1 nearest, 2 linear (D3DFILTER_MIP*/LINEARMIP*)
    uint8_t clamp;                 // TEXTUREADDRESS: 0 wrap, 1 clamp (mirror -> wrap is fine)
    uint8_t cull;                  // D3DCULL: 1 none, 2 CW, 3 CCW (screen-space winding)
} GpuState;

// Starts a scene on a w x h (game pixels) target. bg: the guest render target (RGB565, pitch bytes) whose current
// contents become the starting image (nearest-upscaled), because pixels outside the 3D viewport come from the CPU.
void gpu_begin_scene(int w, int h, const uint16_t *bg, int pitch);
// Viewport Clear: rect in game pixels; flags D3DCLEAR_TARGET 1 / D3DCLEAR_ZBUFFER 2; color is ARGB.
void gpu_clear(int x, int y, int w, int h, uint32_t flags, uint32_t argb, float z);
void gpu_draw(const GpuState *st, const GpuVertex *v, int nv, const uint16_t *idx, int ni);   // idx NULL: non-indexed
// Renders everything queued since gpu_begin_scene, then writes the image downsampled to game resolution into out
// (RGB565, pitch bytes). Blocks until the readback is done.
void gpu_end_scene(uint16_t *out, int pitch);
// Creates or replaces texture `key` (any nonzero id chosen by the caller) with `levels` mip levels, level i being
// (w>>i) x (h>>i) RGBA8 pixels (R in the lowest byte, i.e. SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM) at rgba[i].
void gpu_texture_upload(uint32_t key, int w, int h, int levels, const uint32_t *const *rgba);
void gpu_texture_free(uint32_t key);

// plat.c <-> gpu.c glue. plat_gpu_device: the renderer's device (NULL without a GPU renderer) and the window height
// in pixels. gpu_image: the last scene's full-resolution colour target (RGBA8), NULL before the first scene.
struct SDL_GPUDevice *plat_gpu_device(int *window_pixel_h);
struct SDL_GPUTexture *gpu_image(int *w, int *h);
