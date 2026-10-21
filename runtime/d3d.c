// Direct3D 5 (IDirect3D2 / IDirect3DDevice2) for the D3DPopTB.exe build.
#include "ddraw.h"
#include "gpu.h"

static uint32_t vt_d3d, vt_dev, vt_vp, vt_tex, vt_mat;
static uint32_t d3d_obj;
static void build_vtables(void);

COMFN(d3d_QueryInterface, 2) { wr32(ARG(2), THIS); wr32(THIS + 4, rd32(THIS + 4) + 1); return 0; }

// D3DDEVICEDESC (DirectX 6 size 0xfc) of a typical 1998 HAL card: TL vertices, z-buffer, all blend modes
static void fill_hal_desc(uint32_t d, uint32_t size) {
    memset(PTR(d), 0, size);
    wr32(d, size);
    wr32(d + 0x04, 0x7ff);                 // D3DDD_* all fields valid
    wr32(d + 0x08, 2);                     // D3DCOLOR_RGB
    wr32(d + 0x0c, 0x80ef1);               // FLOATTLVERTEX | *SYSTEMMEMORY | TEXTUREVIDEOMEMORY | DRAWPRIMTLVERTEX | CANRENDERAFTERFLIP | HWRASTERIZATION
    wr32(d + 0x10, 8); wr32(d + 0x14, 1);  // transform caps: CLIP
    wr32(d + 0x18, 1);
    wr32(d + 0x1c, 16);
    for (uint32_t pc = d + 0x2c; pc <= d + 0x64; pc += 0x38) {
        wr32(pc + 0x00, 0x38);
        wr32(pc + 0x04, 0x72);             // MASKZ | CULLNONE | CULLCW | CULLCCW
        wr32(pc + 0x08, 0x1b1);            // DITHER | ZTEST | SUBPIXEL | FOGVERTEX | FOGTABLE
        wr32(pc + 0x0c, 0xff);             // all z compare functions
        wr32(pc + 0x10, 0x1fff); wr32(pc + 0x14, 0x1fff);
        wr32(pc + 0x18, 0xff);
        wr32(pc + 0x1c, 0xfffff);          // flat/gouraud colour, specular, alpha, fog
        wr32(pc + 0x20, 0x8f);             // PERSPECTIVE | POW2 | ALPHA | TRANSPARENCY | ALPHAPALETTE
        wr32(pc + 0x24, 0x3f);             // NEAREST..LINEARMIPLINEAR
        wr32(pc + 0x28, 0xff);             // DECAL..ADD
        wr32(pc + 0x2c, 0x7);              // WRAP | MIRROR | CLAMP
    }
    wr32(d + 0x9c, 0x500);                 // render: DDBD_16 | DDBD_32
    wr32(d + 0xa0, 0x700);                 // z: DDBD_16 | DDBD_24 | DDBD_32
    wr32(d + 0xa8, 65535);
    if (size < 0xcc) return;
    wr32(d + 0xac, 1); wr32(d + 0xb0, 1); wr32(d + 0xb4, 2048); wr32(d + 0xb8, 2048);
}
static const uint8_t HAL_GUID[16] = {0xe0, 0x3d, 0xe6, 0x84, 0xaa, 0x46, 0xcf, 0x11, 0x81, 0x6f, 0x00, 0x00, 0xc0, 0x20, 0x15, 0x6e};

COMFN(d3d_FindDevice, 2) {
    uint32_t res = ARG(2);
    uint32_t size = rd32(res);
    uint32_t dsz = (size - 0x14) / 2;
    memset(PTR(res + 4), 0, size - 4);
    memcpy(PTR(res + 4), HAL_GUID, 16);
    fill_hal_desc(res + 0x14, dsz);
    wr32(res + 0x14 + dsz, dsz);           // software description left empty: hardware only
    LOG("IDirect3D2::FindDevice flags=%x -> HAL", rd32(ARG(1) + 4));
    return 0;
}

typedef struct Device { uint32_t obj, target, viewport; uint32_t rs[256]; int in_scene; } Device;
// D3D5 render-state defaults for the states the draw path reads
static const uint8_t rs_defaults[][2] = {{3, 1}, {9, 2}, {14, 1}, {17, 1}, {18, 1}, {19, 2}, {20, 1}, {21, 2},
    {22, 3}, {23, 4}, {25, 8}, {29, 1}};
typedef struct Viewport { uint32_t obj; int x, y, w, h; float minz, maxz; uint32_t background; } Viewport;
typedef struct Material { uint32_t obj; float diffuse[4]; } Material;
typedef struct Texture { uint32_t obj, surf; } Texture;
static Device *dev;

COMFN(d3d_CreateDevice, 3) {
    Device *d = calloc(1, sizeof *d);
    d->obj = com_new("IDirect3DDevice2", vt_dev, 0, d);
    d->target = ARG(2);
    for (size_t i = 0; i < sizeof rs_defaults / sizeof rs_defaults[0]; i++) d->rs[rs_defaults[i][0]] = rs_defaults[i][1];
    dev = d;
    LOG("IDirect3D2::CreateDevice target=%x -> %x", d->target, d->obj);
    wr32(ARG(3), d->obj);
    return 0;
}
COMFN(d3d_CreateViewport, 2) {
    Viewport *v = calloc(1, sizeof *v);
    v->obj = com_new("IDirect3DViewport2", vt_vp, 0, v);
    wr32(ARG(1), v->obj);
    return 0;
}
COMFN(d3d_CreateMaterial, 2) {
    Material *m = calloc(1, sizeof *m);
    m->obj = com_new("IDirect3DMaterial2", vt_mat, 0, m);
    wr32(ARG(1), m->obj);
    return 0;
}

COMFN(dev_GetCaps, 2) { fill_hal_desc(ARG(1), rd32(ARG(1))); if (ARG(2)) { uint32_t n = rd32(ARG(2)); memset(PTR(ARG(2)), 0, n); wr32(ARG(2), n); } return 0; }
COMFN(dev_AddViewport, 1) { return 0; }
COMFN(dev_DeleteViewport, 1) { return 0; }
COMFN(dev_SetCurrentViewport, 1) { dev->viewport = ARG(1); return 0; }
COMFN(dev_GetCurrentViewport, 1) { wr32(ARG(1), dev->viewport); return dev->viewport ? 0 : 0x88760000 + 0x3e8; }
COMFN(dev_GetDirect3D, 1) { wr32(ARG(1), d3d_create()); return 0; }
COMFN(dev_SetRenderTarget, 2) { dev->target = ARG(1); return 0; }
COMFN(dev_GetRenderTarget, 1) { wr32(ARG(1), dev->target); return 0; }
// The CPU draws 2D over the 3D image after EndScene, so every scene is read back into the guest render target.
// shadow keeps what was written there: at Flip, pixels that still match it show the high-resolution image.
static uint16_t *shadow;
static uint32_t shadow_mem;
static int shadow_w, shadow_h;

COMFN(dev_BeginScene, 0) {
    Surf *t = com_native(dev->target);
    dev->in_scene = 1;
    gpu_begin_scene(t->w, t->h, (const uint16_t *)PTR(t->mem), t->pitch);
    return 0;
}
COMFN(dev_EndScene, 0) {
    Surf *t = com_native(dev->target);
    dev->in_scene = 0;
    gpu_end_scene((uint16_t *)PTR(t->mem), t->pitch);
    if (shadow_w != t->w || shadow_h != t->h) {
        free(shadow);
        shadow = malloc((size_t)t->w * t->h * 2);
        shadow_w = t->w; shadow_h = t->h;
    }
    for (int y = 0; y < t->h; y++) memcpy(shadow + (size_t)y * t->w, PTR(t->mem + y * t->pitch), (size_t)t->w * 2);
    shadow_mem = t->mem;
    t->gen++;
    return 0;
}

int d3d_present(const Surf *s) {
    if (!shadow || s->mem != shadow_mem || s->w != shadow_w || s->h != shadow_h) return 0;
    static uint8_t *mask;
    static size_t mask_n;
    size_t n = (size_t)s->w * s->h;
    if (mask_n < n) { free(mask); mask = malloc(n); mask_n = n; }
    for (int y = 0; y < s->h; y++) {
        const uint16_t *g = (const uint16_t *)PTR(s->mem + y * s->pitch), *r = shadow + (size_t)y * s->w;
        for (int x = 0; x < s->w; x++) mask[(size_t)y * s->w + x] = g[x] != r[x];
    }
    plat_present_hybrid((const uint16_t *)PTR(s->mem), s->w, s->h, s->pitch, mask);
    return 1;
}
COMFN(dev_GetRenderState, 2) { wr32(ARG(2), dev->rs[ARG(1) & 255]); return 0; }
COMFN(dev_SetRenderState, 2) {
    uint32_t st = ARG(1), v = ARG(2);
    if (rt_verbose > 1 && dev->rs[st & 255] != v) LOG("SetRenderState %u = %x", st, v);
    dev->rs[st & 255] = v;
    return 0;
}
COMFN(dev_GetLightState, 2) { wr32(ARG(2), 0); return 0; }
COMFN(dev_SetLightState, 2) { return 0; }
// Converts a texture surface to RGBA8 (R in the low byte) from its pixel format masks or palette.
static uint32_t channel(uint32_t v, uint32_t mask) {
    if (!mask) return 255;
    int sh = __builtin_ctz(mask);
    uint32_t max = mask >> sh;
    return (((v & mask) >> sh) * 255 + max / 2) / max;
}
static void upload_texture(Surf *s) {
    uint32_t *rgba = malloc((size_t)s->w * s->h * 4);
    Pal *p = s->bpp == 8 && s->pal ? com_native(s->pal) : NULL;
    for (int y = 0; y < s->h; y++) {
        uint32_t row = s->mem + y * s->pitch, *d = rgba + (size_t)y * s->w;
        for (int x = 0; x < s->w; x++) {
            if (s->bpp == 8) {
                uint32_t c = p ? p->rgb[rd8(row + x)] : rd8(row + x) * 0x010101u;
                d[x] = 0xff000000u | (c & 0xff) << 16 | (c & 0xff00) | (c >> 16 & 0xff);
                continue;
            }
            uint32_t v = s->bpp == 16 ? rd16(row + 2 * x) : rd32(row + 4 * x);
            d[x] = channel(v, s->mask[0]) | channel(v, s->mask[1]) << 8 | channel(v, s->mask[2]) << 16 | channel(v, s->mask[3]) << 24;
        }
    }
    const uint32_t *levels[1] = {rgba};
    gpu_texture_upload(s->obj, s->w, s->h, 1, levels);
    free(rgba);
    s->tex_gen = s->gen;
}

static int prim_of(uint32_t type) { return type == 1 ? GPU_POINTS : type == 4 ? GPU_TRIANGLES : -1; }
static void draw(uint32_t type, uint32_t vtype, uint32_t verts, uint32_t nv, uint32_t idx, uint32_t ni) {
    int prim = prim_of(type);
    if (prim < 0 || vtype != 3) {
        static int warned;
        if (!warned++) WARN("Direct3D: unsupported primitive %u / vertex type %u", type, vtype);
        return;
    }
    const uint32_t *rs = dev->rs;
    Surf *tex = rs[1] ? com_native(rs[1]) : NULL;
    if (tex && tex->tex_gen != tex->gen) upload_texture(tex);
    GpuState st = {
        .tex = tex ? tex->obj : 0, .prim = (uint8_t)prim,
        .zenable = rs[7] != 0, .zwrite = rs[14] != 0, .zfunc = rs[23],
        .blend = rs[27] != 0, .srcblend = rs[19], .dstblend = rs[20],
        .alphatest = rs[15] != 0, .alphafunc = rs[25], .alpharef = rs[24] & 255,
        .mapblend = rs[21], .specular = rs[29] != 0, .flat = rs[9] == 1,
        .mag_linear = rs[17] == 2, .min_linear = rs[18] == 2 || rs[18] >= 5,
        .clamp = rs[3] == 3, .cull = rs[22],
    };
    gpu_draw(&st, (const GpuVertex *)PTR(verts), (int)nv, idx ? (const uint16_t *)PTR(idx) : NULL, (int)ni);
}
COMFN(dev_DrawPrimitive, 5) { draw(ARG(1), ARG(2), ARG(3), ARG(4), 0, 0); return 0; }
COMFN(dev_DrawIndexedPrimitive, 7) { draw(ARG(1), ARG(2), ARG(3), ARG(4), ARG(5), ARG(6)); return 0; }

// DDSURFACEDESC.ddpfPixelFormat for each texture format offered to the game
typedef struct TexFormat { uint32_t flags, bits, r, g, b, a; } TexFormat;
static const TexFormat tex_formats[] = {
    {0x40, 16, 0xf800, 0x07e0, 0x001f, 0},          // RGB565
    {0x41, 16, 0x7c00, 0x03e0, 0x001f, 0x8000},     // ARGB1555
    {0x41, 16, 0x0f00, 0x00f0, 0x000f, 0xf000},     // ARGB4444
    {0x60, 8, 0, 0, 0, 0},                          // 8-bit palettized
};
COMFN(dev_EnumTextureFormats, 2) {
    uint32_t d = gmalloc(108);
    for (size_t i = 0; i < sizeof tex_formats / sizeof tex_formats[0]; i++) {
        const TexFormat *f = &tex_formats[i];
        memset(PTR(d), 0, 108);
        wr32(d, 108); wr32(d + 4, 0x1000);
        uint32_t pf = d + 72;
        wr32(pf, 32); wr32(pf + 4, f->flags); wr32(pf + 12, f->bits);
        wr32(pf + 16, f->r); wr32(pf + 20, f->g); wr32(pf + 24, f->b); wr32(pf + 28, f->a);
        if (!guest_call(R, ARG(1), 2, d, ARG(2))) break;
    }
    gfree(d);
    return 0;
}

COMFN(vp_SetViewport2, 1) {
    Viewport *v = com_native(THIS); uint32_t d = ARG(1);
    v->x = (int)rd32(d + 4); v->y = (int)rd32(d + 8); v->w = (int)rd32(d + 12); v->h = (int)rd32(d + 16);
    v->minz = rdf32(d + 36); v->maxz = rdf32(d + 40);
    LOG("Viewport %d,%d %dx%d z %g..%g", v->x, v->y, v->w, v->h, v->minz, v->maxz);
    return 0;
}
COMFN(vp_SetViewport, 1) {
    Viewport *v = com_native(THIS); uint32_t d = ARG(1);
    v->x = (int)rd32(d + 4); v->y = (int)rd32(d + 8); v->w = (int)rd32(d + 12); v->h = (int)rd32(d + 16);
    v->minz = rdf32(d + 40); v->maxz = rdf32(d + 44);
    LOG("Viewport1 %d,%d %dx%d", v->x, v->y, v->w, v->h);
    return 0;
}
COMFN(vp_SetBackground, 1) { ((Viewport *)com_native(THIS))->background = ARG(1); return 0; }
static uint8_t unorm8(float f) { return f <= 0 ? 0 : f >= 1 ? 255 : (uint8_t)(f * 255 + 0.5f); }
COMFN(vp_Clear, 3) {
    Viewport *v = com_native(THIS);
    Material *m = v->background ? com_native(v->background) : NULL;
    uint32_t argb = m ? (uint32_t)unorm8(m->diffuse[3]) << 24 | unorm8(m->diffuse[0]) << 16 | unorm8(m->diffuse[1]) << 8 | unorm8(m->diffuse[2]) : 0;
    for (uint32_t i = 0, r = ARG(2); i < ARG(1); i++, r += 16) {
        int x1 = (int)rd32(r), y1 = (int)rd32(r + 4), x2 = (int)rd32(r + 8), y2 = (int)rd32(r + 12);
        gpu_clear(x1, y1, x2 - x1, y2 - y1, ARG(3), argb, 1.0f);
    }
    return 0;
}
COMFN(vp_Initialize, 1) { return 0; }

COMFN(mat_SetMaterial, 1) { Material *m = com_native(THIS); memcpy(m->diffuse, PTR(ARG(1) + 4), 16); return 0; }
COMFN(mat_GetHandle, 2) { wr32(ARG(2), THIS); return 0; }

COMFN(tex_GetHandle, 2) { Texture *t = com_native(THIS); wr32(ARG(2), t->surf); return 0; }
COMFN(tex_PaletteChanged, 2) { return 0; }
COMFN(tex_Load, 1) {
    Texture *t = com_native(THIS), *src = com_native(ARG(1));
    Surf *d = com_native(t->surf), *s = com_native(src->surf);
    for (int y = 0; y < d->h && y < s->h; y++) memcpy(PTR(d->mem + y * d->pitch), PTR(s->mem + y * s->pitch), d->pitch < s->pitch ? d->pitch : s->pitch);
    if (s->pal) d->pal = s->pal;
    d->gen++;
    return 0;
}

// IDirectDrawSurface::QueryInterface(IID_IDirect3DTexture2): the texture interface of a surface
uint32_t d3d_texture_of(uint32_t surf) {
    if (!vt_d3d) build_vtables();
    Texture *t = calloc(1, sizeof *t);
    t->surf = surf;
    t->obj = com_new("IDirect3DTexture2", vt_tex, 0, t);
    return t->obj;
}

static void build_vtables(void) {
    static const char *d3dm[] = {"QueryInterface", "AddRef", "Release", "EnumDevices", "CreateLight",
        "CreateMaterial", "CreateViewport", "FindDevice", "CreateDevice"};
    native_fn d3df[] = {d3d_QueryInterface, com_addref, com_release, NULL, NULL, d3d_CreateMaterial, d3d_CreateViewport, d3d_FindDevice, d3d_CreateDevice};
    vt_d3d = com_vtbl("IDirect3D2", d3dm, 9, d3df);
    static const char *devm[] = {"QueryInterface", "AddRef", "Release", "GetCaps", "SwapTextureHandles",
        "GetStats", "AddViewport", "DeleteViewport", "NextViewport", "EnumTextureFormats", "BeginScene",
        "EndScene", "GetDirect3D", "SetCurrentViewport", "GetCurrentViewport", "SetRenderTarget",
        "GetRenderTarget", "Begin", "BeginIndexed", "Vertex", "Index", "End", "GetRenderState",
        "SetRenderState", "GetLightState", "SetLightState", "SetTransform", "GetTransform",
        "MultiplyTransform", "DrawPrimitive", "DrawIndexedPrimitive", "SetClipStatus", "GetClipStatus"};
    native_fn devf[33] = {d3d_QueryInterface, com_addref, com_release, dev_GetCaps, NULL, NULL, dev_AddViewport,
        dev_DeleteViewport, NULL, dev_EnumTextureFormats, dev_BeginScene, dev_EndScene, dev_GetDirect3D,
        dev_SetCurrentViewport, dev_GetCurrentViewport, dev_SetRenderTarget, dev_GetRenderTarget, NULL, NULL, NULL,
        NULL, NULL, dev_GetRenderState, dev_SetRenderState, dev_GetLightState, dev_SetLightState, NULL, NULL, NULL,
        dev_DrawPrimitive, dev_DrawIndexedPrimitive};
    vt_dev = com_vtbl("IDirect3DDevice2", devm, 33, devf);
    static const char *vpm[] = {"QueryInterface", "AddRef", "Release", "Initialize", "GetViewport",
        "SetViewport", "TransformVertices", "LightElements", "SetBackground", "GetBackground",
        "SetBackgroundDepth", "GetBackgroundDepth", "Clear", "AddLight", "DeleteLight", "NextLight",
        "GetViewport2", "SetViewport2"};
    native_fn vpf[18] = {d3d_QueryInterface, com_addref, com_release, vp_Initialize, NULL, vp_SetViewport, NULL, NULL,
        vp_SetBackground, NULL, NULL, NULL, vp_Clear, NULL, NULL, NULL, NULL, vp_SetViewport2};
    vt_vp = com_vtbl("IDirect3DViewport2", vpm, 18, vpf);
    static const char *texm[] = {"QueryInterface", "AddRef", "Release", "GetHandle", "PaletteChanged", "Load"};
    native_fn texf[6] = {d3d_QueryInterface, com_addref, com_release, tex_GetHandle, tex_PaletteChanged, tex_Load};
    vt_tex = com_vtbl("IDirect3DTexture2", texm, 6, texf);
    static const char *matm[] = {"QueryInterface", "AddRef", "Release", "SetMaterial", "GetMaterial", "GetHandle"};
    native_fn matf[6] = {d3d_QueryInterface, com_addref, com_release, mat_SetMaterial, NULL, mat_GetHandle};
    vt_mat = com_vtbl("IDirect3DMaterial2", matm, 6, matf);
}

uint32_t d3d_create(void) {
    if (!vt_d3d) build_vtables();
    if (!d3d_obj) d3d_obj = com_new("IDirect3D2", vt_d3d, 0, NULL);
    wr32(d3d_obj + 4, rd32(d3d_obj + 4) + 1);
    return d3d_obj;
}
