// Direct3D 5 (IDirect3D2 / IDirect3DDevice2 / IDirect3DViewport2 / IDirect3DMaterial2 / IDirect3DTexture2)
// as fake COM objects. The rendering itself is done by the GPU backend (d3d_gpu.c).
#include "ddraw.h"
#include "d3d.h"

// ---- IIDs (first dword) ----
#define IID_HAL_D1 0x84E63dE0u

typedef struct Material { uint32_t obj; float diffuse[4]; } Material;
typedef struct Viewport {
    uint32_t obj;
    int x, y, w, h;
    float min_z, max_z;
    uint32_t bg_material;   // material handle used by Clear
} Viewport;
typedef struct Device {
    uint32_t obj;
    uint32_t target;        // render target surface object
    uint32_t cur_vp;
    uint32_t rs[D3D_NRS];
    int in_scene;
} Device;

#define MAX_HANDLES 4096
static Surf *tex_handles[MAX_HANDLES];
static Material *mat_handles[MAX_HANDLES];
static int ntex = 1, nmat = 1;

static uint32_t vt_d3d, vt_dev, vt_vp, vt_mat, vt_tex;
static void d3d_build_vtables(void);

// The game only draws pre-transformed vertices (DrawPrimitive with D3DTLVERTEX) through immediate mode with a fixed
// viewport and no lights, so the methods below that belong to the transform pipeline, lights, execute buffers and
// the retained-style Begin/Vertex/End calls are reported and ignored.
// TODO: implement them (and untransformed/lit vertex types, fog) if another executable needs the fixed-function pipeline.
static uint32_t unimpl(const char *what) {
    WARN("D3D: %s not implemented", what);
    return 0;
}

// ---------------- IDirect3D2 ----------------
// D3DDEVICEDESC (DX5, 204 bytes; DX6 adds 48 more)
static void fill_prim_caps(uint32_t p) {
    memset(PTR(p), 0, 56);
    wr32(p, 56);
    wr32(p + 4, 0x10 | 0x20 | 0x40);                      // MISCCAPS: CULLNONE|CULLCW|CULLCCW
    wr32(p + 8, 0x1 | 0x10 | 0x80 | 0x100 | 0x4000);      // RASTERCAPS: DITHER|ZTEST|FOGVERTEX|FOGTABLE|ZBIAS
    wr32(p + 12, 0xff);                                   // ZCMPCAPS
    wr32(p + 16, 0x1fff); wr32(p + 20, 0x1fff);           // SRC/DESTBLENDCAPS
    wr32(p + 24, 0xff);                                   // ALPHACMPCAPS
    wr32(p + 28, 0x2 | 0x8 | 0x80 | 0x200 | 0x1000 | 0x4000 | 0x40000 | 0x80000);   // SHADECAPS (incl. ALPHAFLATBLEND)
    wr32(p + 32, 0x1 | 0x2 | 0x4 | 0x8);                  // TEXTURECAPS: PERSPECTIVE|POW2|ALPHA|TRANSPARENCY
    wr32(p + 36, 0x1 | 0x2);                              // TEXTUREFILTERCAPS: NEAREST|LINEAR
    wr32(p + 40, 0xff);                                   // TEXTUREBLENDCAPS
    wr32(p + 44, 0x1 | 0x2 | 0x4 | 0x10);                 // TEXTUREADDRESSCAPS
}
static void fill_device_desc(uint32_t d, int hw) {
    uint32_t size = rd32(d);
    if (size < 172 || size > 300) size = 252;
    memset(PTR(d), 0, size);
    wr32(d, size);
    if (!hw) return;
    wr32(d + 4, 0x7ff);                      // all fields valid (COLORMODEL..MAXVERTEXCOUNT)
    wr32(d + 8, 2);                          // D3DCOLOR_RGB
    wr32(d + 12, 0x1 | 0x20 | 0x80 | 0x200 | 0x400 | 0x800);   // DEVCAPS: FLOATTLVERTEX|EXECUTEVIDEOMEM|TLVERTEXVIDEOMEM|TEXTUREVIDEOMEM|DRAWPRIMTLVERTEX|CANRENDERAFTERFLIP
    wr32(d + 16, 8); wr32(d + 20, 1);        // D3DTRANSFORMCAPS: CLIP
    wr32(d + 24, 1);                         // bClipping
    wr32(d + 28, 16);                        // lighting caps: none
    fill_prim_caps(d + 44);
    fill_prim_caps(d + 100);
    wr32(d + 156, 0x400 | 0x100 | 0x800);    // render bit depth 16 / 32 / 8
    wr32(d + 160, 0x400 | 0x200 | 0x100);    // z-buffer 16 / 24 / 32
    wr32(d + 164, 0); wr32(d + 168, 0);
    if (size >= 204) {
        wr32(d + 172, 1); wr32(d + 176, 1);          // min texture size
        wr32(d + 180, 2048); wr32(d + 184, 2048);    // max texture size
    }
}

static const uint8_t HAL_GUID[16] = {0xE0, 0x3d, 0xE6, 0x84, 0xAA, 0x46, 0xCF, 0x11, 0x81, 0x6F, 0x00, 0x00, 0xC0, 0x20, 0x15, 0x6E};

COMFN(d3d_QueryInterface, 2) {
    uint32_t d1 = rd32(ARG(1));
    if (d1 == IID_IDIRECT3D2) { wr32(ARG(2), THIS); wr32(THIS + 4, rd32(THIS + 4) + 1); return 0; }
    WARN("IDirect3D2::QueryInterface unknown iid %08x", d1);
    wr32(ARG(2), 0);
    return 0x80004002;
}
COMFN(d3d_EnumDevices, 2) {
    uint32_t cb = ARG(1), ctx = ARG(2);
    uint32_t guid = gmalloc(16), name = gstrdup("Direct3D HAL"), desc = gstrdup("OpenPopTB SDL_GPU Direct3D device");
    uint32_t hal = gmalloc(252), hel = gmalloc(252);
    memcpy(PTR(guid), HAL_GUID, 16);
    wr32(hal, 252); wr32(hel, 252);
    fill_device_desc(hal, 1); fill_device_desc(hel, 0);
    guest_call(R, cb, 6, guid, desc, name, hal, hel, ctx);
    gfree(guid); gfree(hal); gfree(hel);
    return 0;
}
COMFN(d3d_CreateLight, 2) { wr32(ARG(1), 0); return unimpl("CreateLight") | 0x80004001; }
COMFN(d3d_CreateMaterial, 2) {
    Material *m = calloc(1, sizeof *m);
    m->obj = com_new("IDirect3DMaterial2", vt_mat, 0, m);
    wr32(ARG(1), m->obj);
    return 0;
}
COMFN(d3d_CreateViewport, 2) {
    Viewport *v = calloc(1, sizeof *v);
    v->obj = com_new("IDirect3DViewport2", vt_vp, 0, v);
    v->max_z = 1;
    wr32(ARG(1), v->obj);
    return 0;
}
COMFN(d3d_FindDevice, 2) {
    uint32_t res = ARG(2);
    uint32_t size = rd32(res);
    if (size < 20 + 2 * 172 || size > 20 + 2 * 300) { WARN("D3D: FindDevice with a result size of %u", size); return 0x80070057; }   // E_INVALIDARG
    LOG("D3D FindDevice (result size %u)", size);
    memset(PTR(res), 0, size); wr32(res, size);
    memcpy(PTR(res + 4), HAL_GUID, 16);
    uint32_t sub = (size - 20) / 2;   // two D3DDEVICEDESC (the game's SDK defines their size)
    wr32(res + 20, sub); fill_device_desc(res + 20, 1);
    wr32(res + 20 + sub, sub); fill_device_desc(res + 20 + sub, 0);
    return 0;
}
COMFN(d3d_CreateDevice, 3) {
    uint32_t guid = ARG(1), target = ARG(2), out = ARG(3);
    uint32_t d1 = rd32(guid);
    Device *d = calloc(1, sizeof *d);
    d->obj = com_new("IDirect3DDevice2", vt_dev, 0, d);
    d->target = target;
    d3d_default_states(d->rs);
    LOG("D3D CreateDevice guid=%08x target=%x -> %x", d1, target, d->obj);
    if (d1 != IID_HAL_D1) WARN("D3D: CreateDevice for a non-HAL device (%08x)", d1);
    if (!d3d_gpu_init()) return 0x80004005;
    wr32(out, d->obj);
    return 0;
}

// ---------------- IDirect3DDevice2 ----------------
static Device *D(uint32_t o) { return com_native(o); }
static Viewport *V(uint32_t o) { return com_native(o); }
static void apply_viewport(uint32_t o);
static uint32_t g_cur_vp;   // the device's current viewport object: only its rectangle clips drawing

COMFN(dev_QueryInterface, 2) { wr32(ARG(2), 0); WARN("IDirect3DDevice2::QueryInterface %08x", rd32(ARG(1))); return 0x80004002; }
COMFN(dev_GetCaps, 2) {
    fill_device_desc(ARG(1), 1);
    fill_device_desc(ARG(2), 0);
    return 0;
}
COMFN(dev_SwapTextureHandles, 2) { return unimpl("SwapTextureHandles"); }
COMFN(dev_GetStats, 1) { memset(PTR(ARG(1)), 0, rd32(ARG(1))); return 0; }
COMFN(dev_AddViewport, 1) { return 0; }
COMFN(dev_DeleteViewport, 1) { return 0; }
COMFN(dev_NextViewport, 3) { wr32(ARG(2), 0); return 0; }
COMFN(dev_EnumTextureFormats, 2) {
    uint32_t cb = ARG(1), ctx = ARG(2);
    struct { uint32_t flags, bits, r, g, b, a; } fmt[] = {
        {0x40 | 0x1, 16, 0x7c00, 0x03e0, 0x001f, 0x8000},    // ARGB1555 (RGB | ALPHAPIXELS)
        {0x40 | 0x1, 16, 0x0f00, 0x00f0, 0x000f, 0xf000},    // ARGB4444
        {0x40, 16, 0xf800, 0x07e0, 0x001f, 0},               // RGB565
        {0x20 | 0x4, 8, 0, 0, 0, 0},                          // 8-bit paletted (PALETTEINDEXED8 | RGB)
    };
    uint32_t d = gmalloc(124);
    for (int i = 0; i < 4; i++) {
        memset(PTR(d), 0, 124);
        wr32(d, 108); wr32(d + 4, 0x1000);
        uint32_t p = d + 72;
        wr32(p, 32); wr32(p + 4, fmt[i].flags); wr32(p + 12, fmt[i].bits);
        wr32(p + 16, fmt[i].r); wr32(p + 20, fmt[i].g); wr32(p + 24, fmt[i].b); wr32(p + 28, fmt[i].a);
        if (i == 3) wr32(p + 4, 0x20 | 0x40);   // DDPF_PALETTEINDEXED8 | DDPF_RGB
        if (!guest_call(R, cb, 2, d, ctx)) break;
    }
    gfree(d);
    return 0;
}
COMFN(dev_BeginScene, 0) { Device *d = D(THIS); d->in_scene = 1; apply_viewport(d->cur_vp); d3d_gpu_begin_scene(dd_surf(d->target)); return 0; }
COMFN(dev_EndScene, 0) { Device *d = D(THIS); d->in_scene = 0; d3d_gpu_end_scene(dd_surf(d->target)); return 0; }
COMFN(dev_GetDirect3D, 1) { wr32(ARG(1), d3d_create_interface()); return 0; }
static void apply_viewport(uint32_t o) {
    Viewport *v = o ? V(o) : NULL;
    if (v) d3d_gpu_set_viewport(v->x, v->y, v->w, v->h);
}
COMFN(dev_SetCurrentViewport, 1) { D(THIS)->cur_vp = g_cur_vp = ARG(1); apply_viewport(ARG(1)); return 0; }
COMFN(dev_GetCurrentViewport, 1) { uint32_t v = D(THIS)->cur_vp; wr32(ARG(1), v); if (v) wr32(v + 4, rd32(v + 4) + 1); return 0; }
COMFN(dev_SetRenderTarget, 2) { D(THIS)->target = ARG(1); LOG("D3D SetRenderTarget %x", ARG(1)); return 0; }
COMFN(dev_GetRenderTarget, 1) { wr32(ARG(1), D(THIS)->target); return 0; }
COMFN(dev_Begin, 3) { return unimpl("Begin"); }
COMFN(dev_BeginIndexed, 5) { return unimpl("BeginIndexed"); }
COMFN(dev_Vertex, 1) { return unimpl("Vertex"); }
COMFN(dev_Index, 1) { return unimpl("Index"); }
COMFN(dev_End, 1) { return unimpl("End"); }
COMFN(dev_GetRenderState, 2) { uint32_t st = ARG(1); wr32(ARG(2), st < D3D_NRS ? D(THIS)->rs[st] : 0); return 0; }
COMFN(dev_SetRenderState, 2) {
    Device *d = D(THIS);
    uint32_t st = ARG(1), v = ARG(2);
    if (st < D3D_NRS) d->rs[st] = v;
    if (st == RS_TEXTUREHANDLE) d3d_gpu_set_texture(v < (uint32_t)ntex ? tex_handles[v] : NULL);
    if (rt_verbose > 1) LOG("D3D SetRenderState %u = %u", st, v);
    d3d_gpu_set_state(st, v);
    return 0;
}
COMFN(dev_GetLightState, 2) { wr32(ARG(2), 0); return 0; }
COMFN(dev_SetLightState, 2) { return 0; }
COMFN(dev_SetTransform, 2) { return unimpl("SetTransform"); }
COMFN(dev_GetTransform, 2) { return unimpl("GetTransform"); }
COMFN(dev_MultiplyTransform, 2) { return unimpl("MultiplyTransform"); }
COMFN(dev_DrawPrimitive, 5) {
    uint32_t prim = ARG(1), vtype = ARG(2), verts = ARG(3), count = ARG(4);
    if (vtype != 3) { WARN("D3D: DrawPrimitive vertex type %u (only TLVERTEX is implemented)", vtype); return 0; }
    d3d_gpu_draw(prim, (const D3DTLVertex *)PTR(verts), (int)count, NULL, 0);
    return 0;
}
COMFN(dev_DrawIndexedPrimitive, 7) {
    uint32_t prim = ARG(1), vtype = ARG(2), verts = ARG(3), count = ARG(4), idx = ARG(5), icount = ARG(6);
    if (vtype != 3) { WARN("D3D: DrawIndexedPrimitive vertex type %u (only TLVERTEX is implemented)", vtype); return 0; }
    d3d_gpu_draw(prim, (const D3DTLVertex *)PTR(verts), (int)count, (const uint16_t *)PTR(idx), (int)icount);
    return 0;
}
COMFN(dev_SetClipStatus, 1) { return 0; }
COMFN(dev_GetClipStatus, 1) { memset(PTR(ARG(1)), 0, 12); return 0; }

// ---------------- IDirect3DViewport2 ----------------

COMFN(vp_Initialize, 1) { return 0; }
COMFN(vp_GetViewport, 1) { return unimpl("Viewport::GetViewport"); }
COMFN(vp_SetViewport, 1) {
    Viewport *v = V(THIS); uint32_t p = ARG(1);
    v->x = (int)rd32(p + 4); v->y = (int)rd32(p + 8); v->w = (int)rd32(p + 12); v->h = (int)rd32(p + 16);
    v->min_z = rdf32(p + 36); v->max_z = rdf32(p + 40);
    LOG("D3D Viewport::SetViewport %d,%d %dx%d z=%g..%g", v->x, v->y, v->w, v->h, v->min_z, v->max_z);
    if (THIS == g_cur_vp) d3d_gpu_set_viewport(v->x, v->y, v->w, v->h);
    return 0;
}
COMFN(vp_SetViewport2, 1) {
    Viewport *v = V(THIS); uint32_t p = ARG(1);
    v->x = (int)rd32(p + 4); v->y = (int)rd32(p + 8); v->w = (int)rd32(p + 12); v->h = (int)rd32(p + 16);
    v->min_z = rdf32(p + 36); v->max_z = rdf32(p + 40);
    LOG("D3D Viewport::SetViewport2 %d,%d %dx%d z=%g..%g", v->x, v->y, v->w, v->h, v->min_z, v->max_z);
    if (THIS == g_cur_vp) d3d_gpu_set_viewport(v->x, v->y, v->w, v->h);
    return 0;
}
COMFN(vp_GetViewport2, 1) { return unimpl("Viewport::GetViewport2"); }
COMFN(vp_TransformVertices, 4) { return unimpl("Viewport::TransformVertices"); }
COMFN(vp_LightElements, 2) { return unimpl("Viewport::LightElements"); }
COMFN(vp_SetBackground, 1) { V(THIS)->bg_material = ARG(1); return 0; }
COMFN(vp_GetBackground, 2) { wr32(ARG(1), V(THIS)->bg_material); wr32(ARG(2), V(THIS)->bg_material != 0); return 0; }
COMFN(vp_SetBackgroundDepth, 1) { return 0; }
COMFN(vp_GetBackgroundDepth, 2) { return unimpl("Viewport::GetBackgroundDepth"); }
COMFN(vp_Clear, 3) {
    Viewport *v = V(THIS);
    uint32_t count = ARG(1), rects = ARG(2), flags = ARG(3);
    float col[4] = {0, 0, 0, 1};
    if (v->bg_material && v->bg_material < (uint32_t)nmat) memcpy(col, mat_handles[v->bg_material]->diffuse, sizeof col);
    int rc[4] = {v->x, v->y, v->x + v->w, v->y + v->h};
    if (count && rects) { rc[0] = (int)rd32(rects); rc[1] = (int)rd32(rects + 4); rc[2] = (int)rd32(rects + 8); rc[3] = (int)rd32(rects + 12); }
    d3d_gpu_clear(flags & 1, flags & 2, col, rc);
    return 0;
}
COMFN(vp_AddLight, 1) { return 0; }
COMFN(vp_DeleteLight, 1) { return 0; }
COMFN(vp_NextLight, 3) { wr32(ARG(2), 0); return 0; }

// ---------------- IDirect3DMaterial2 ----------------
COMFN(mat_SetMaterial, 1) {
    Material *m = com_native(THIS);
    for (int i = 0; i < 4; i++) m->diffuse[i] = rdf32(ARG(1) + 4 + 4 * i);
    return 0;
}
COMFN(mat_GetMaterial, 1) { return unimpl("Material::GetMaterial"); }
COMFN(mat_GetHandle, 2) {
    Material *m = com_native(THIS);
    if (nmat >= MAX_HANDLES) return 0x80004005;
    mat_handles[nmat] = m;
    wr32(ARG(2), (uint32_t)nmat++);
    return 0;
}

// ---------------- IDirect3DTexture2 ----------------
COMFN(tex_QueryInterface, 2) {
    uint32_t d1 = rd32(ARG(1));
    if (d1 == IID_IDIRECT3DTEXTURE2) { wr32(ARG(2), THIS); wr32(THIS + 4, rd32(THIS + 4) + 1); return 0; }
    if (d1 == 0x2CDCD9E0) { wr32(ARG(2), 0); return 0x80004002; }   // IDirect3DTexture (DX3): different method table
    // IDirectDrawSurface and friends: back to the owning surface
    Surf *s = com_native(THIS);
    wr32(ARG(2), s->obj); wr32(s->obj + 4, rd32(s->obj + 4) + 1);
    return 0;
}
COMFN(tex_GetHandle, 2) {
    Surf *s = com_native(THIS);
    if (ntex >= MAX_HANDLES) return 0x80004005;
    for (int i = 1; i < ntex; i++) if (tex_handles[i] == s) { wr32(ARG(2), (uint32_t)i); return 0; }
    tex_handles[ntex] = s;
    wr32(ARG(2), (uint32_t)ntex++);
    return 0;
}
COMFN(tex_PaletteChanged, 2) { g_pal_gen++; return 0; }
COMFN(tex_Load, 1) {
    Surf *d = com_native(THIS), *s = com_native(ARG(1));
    // the source is the IDirect3DTexture2 of another surface
    if (!s || !d) return 0x80004005;
    if (d->w == s->w && d->h == s->h && d->bpp == s->bpp) memcpy(PTR(d->mem), PTR(s->mem), (size_t)s->pitch * s->h);
    else WARN("D3D: Texture::Load between different formats (%dx%d/%d -> %dx%d/%d)", s->w, s->h, s->bpp, d->w, d->h, d->bpp);
    if (s->pal) d->pal = s->pal;
    d->gen++;
    return 0;
}

uint32_t d3d_texture_object(Surf *s) {
    if (!vt_tex) d3d_build_vtables();
    if (!s->d3dtex) s->d3dtex = com_new("IDirect3DTexture2", vt_tex, 0, s);
    else wr32(s->d3dtex + 4, rd32(s->d3dtex + 4) + 1);
    return s->d3dtex;
}

uint32_t d3d_create_interface(void) {
    static uint32_t obj;
    if (!vt_d3d) d3d_build_vtables();
    if (!obj) obj = com_new("IDirect3D2", vt_d3d, 0, NULL);
    else wr32(obj + 4, rd32(obj + 4) + 1);
    return obj;
}

static void d3d_build_vtables(void) {
    static const char *dm[] = {"QueryInterface", "AddRef", "Release", "EnumDevices", "CreateLight", "CreateMaterial",
        "CreateViewport", "FindDevice", "CreateDevice"};
    native_fn df[] = {d3d_QueryInterface, com_addref, com_release, d3d_EnumDevices, d3d_CreateLight, d3d_CreateMaterial,
        d3d_CreateViewport, d3d_FindDevice, d3d_CreateDevice};
    vt_d3d = com_vtbl("IDirect3D2", dm, 9, df);
    static const char *vm[] = {"QueryInterface", "AddRef", "Release", "GetCaps", "SwapTextureHandles", "GetStats",
        "AddViewport", "DeleteViewport", "NextViewport", "EnumTextureFormats", "BeginScene", "EndScene", "GetDirect3D",
        "SetCurrentViewport", "GetCurrentViewport", "SetRenderTarget", "GetRenderTarget", "Begin", "BeginIndexed",
        "Vertex", "Index", "End", "GetRenderState", "SetRenderState", "GetLightState", "SetLightState",
        "SetTransform", "GetTransform", "MultiplyTransform", "DrawPrimitive", "DrawIndexedPrimitive",
        "SetClipStatus", "GetClipStatus"};
    native_fn vf[] = {dev_QueryInterface, com_addref, com_release, dev_GetCaps, dev_SwapTextureHandles, dev_GetStats,
        dev_AddViewport, dev_DeleteViewport, dev_NextViewport, dev_EnumTextureFormats, dev_BeginScene, dev_EndScene,
        dev_GetDirect3D, dev_SetCurrentViewport, dev_GetCurrentViewport, dev_SetRenderTarget, dev_GetRenderTarget,
        dev_Begin, dev_BeginIndexed, dev_Vertex, dev_Index, dev_End, dev_GetRenderState, dev_SetRenderState,
        dev_GetLightState, dev_SetLightState, dev_SetTransform, dev_GetTransform, dev_MultiplyTransform,
        dev_DrawPrimitive, dev_DrawIndexedPrimitive, dev_SetClipStatus, dev_GetClipStatus};
    vt_dev = com_vtbl("IDirect3DDevice2", vm, 33, vf);
    static const char *pm[] = {"QueryInterface", "AddRef", "Release", "Initialize", "GetViewport", "SetViewport",
        "TransformVertices", "LightElements", "SetBackground", "GetBackground", "SetBackgroundDepth",
        "GetBackgroundDepth", "Clear", "AddLight", "DeleteLight", "NextLight", "GetViewport2", "SetViewport2"};
    native_fn pf[] = {dev_QueryInterface, com_addref, com_release, vp_Initialize, vp_GetViewport, vp_SetViewport,
        vp_TransformVertices, vp_LightElements, vp_SetBackground, vp_GetBackground, vp_SetBackgroundDepth,
        vp_GetBackgroundDepth, vp_Clear, vp_AddLight, vp_DeleteLight, vp_NextLight, vp_GetViewport2, vp_SetViewport2};
    vt_vp = com_vtbl("IDirect3DViewport2", pm, 18, pf);
    static const char *mm[] = {"QueryInterface", "AddRef", "Release", "SetMaterial", "GetMaterial", "GetHandle"};
    native_fn mf[] = {dev_QueryInterface, com_addref, com_release, mat_SetMaterial, mat_GetMaterial, mat_GetHandle};
    vt_mat = com_vtbl("IDirect3DMaterial2", mm, 6, mf);
    static const char *tm[] = {"QueryInterface", "AddRef", "Release", "GetHandle", "PaletteChanged", "Load"};
    native_fn tf[] = {tex_QueryInterface, com_addref, com_release, tex_GetHandle, tex_PaletteChanged, tex_Load};
    vt_tex = com_vtbl("IDirect3DTexture2", tm, 6, tf);
}
