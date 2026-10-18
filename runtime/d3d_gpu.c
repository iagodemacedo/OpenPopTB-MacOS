// GPU backend of the Direct3D layer (SDL_GPU).
//
// The game draws a scene with pre-transformed vertices (D3DTLVERTEX) between BeginScene and EndScene.
// Every draw is recorded; EndScene uploads vertices and changed textures, renders the whole scene into an
// off-screen target and copies the viewport rectangle back into the guest render-target surface, so the
// game's own 2D drawing (DirectDraw blits, flips) keeps working on top of it.
//
// The scene is rendered at the window's resolution (multisampled, trilinear/anisotropic textures with mip maps).
// The guest surface gets a 1x downsampled copy; at present time the pixels of that copy that the game did not
// touch afterwards (2D blits, cursor) are replaced by the hi-res image, see gpu_present().
//
// Shaders are written in MSL (Metal) for now.
// TODO: add SPIR-V (Vulkan) and DXIL (Direct3D 12) shaders (HLSL + SDL_shadercross) for other platforms.
#include "d3d.h"
#include <SDL3/SDL.h>
#include <stddef.h>

void d3d_default_states(uint32_t *rs) {
    memset(rs, 0, D3D_NRS * sizeof *rs);
    rs[RS_ZENABLE] = 1; rs[RS_ZWRITEENABLE] = 1; rs[RS_ZFUNC] = 4;   // LEQUAL
    rs[RS_CULLMODE] = 3;                                              // CCW
    rs[RS_SRCBLEND] = 2; rs[RS_DESTBLEND] = 1;                        // ONE, ZERO
    rs[RS_TEXTUREMAPBLEND] = 2;                                       // MODULATE
    rs[RS_TEXTUREMAG] = 1; rs[RS_TEXTUREMIN] = 1;                     // NEAREST
    rs[RS_ALPHAFUNC] = 8;                                             // ALWAYS
    rs[RS_TEXTUREADDRESS] = 1;                                        // WRAP
    rs[RS_TEXTUREADDRESSU] = 1; rs[RS_TEXTUREADDRESSV] = 1;
}

// ---------------------------------------------------------------------------------------------
static const char *MSL =
    "#include <metal_stdlib>\n"
    "using namespace metal;\n"
    "struct VIn { float4 pos [[attribute(0)]]; float4 col [[attribute(1)]]; float4 spec [[attribute(2)]]; float2 uv [[attribute(3)]]; };\n"
    "struct VOut { float4 pos [[position]]; float4 col; float4 spec; float2 uv; float ps [[point_size]]; };\n"
    "struct VU { float point_size; };\n"
    "vertex VOut vs_main(VIn i [[stage_in]], constant VU& u [[buffer(0)]]) {\n"
    "  VOut o; o.pos = i.pos; o.col = i.col.zyxw; o.spec = i.spec.zyxw; o.uv = i.uv; o.ps = u.point_size; return o;\n"
    "}\n"
    "struct FU { float alpharef; int alphafunc; int texblend; int use_tex; int use_spec; int alphatest; int tex_alpha; int pad; };\n"
    "fragment float4 fs_main(VOut i [[stage_in]], constant FU& u [[buffer(0)]], texture2d<float> t [[texture(0)]], sampler s [[sampler(0)]]) {\n"
    "  float4 c = i.col;\n"
    "  if (u.use_tex != 0) {\n"
    "    float4 x = t.sample(s, i.uv);\n"
    "    switch (u.texblend) {\n"
    "      case 1: c = x; break;\n"                                                         // DECAL
    "      case 3: c = float4(mix(c.rgb, x.rgb, x.a), c.a); break;\n"                        // DECALALPHA
    "      case 4: c = x * c; break;\n"                                                      // MODULATEALPHA
    "      case 7: c = x; break;\n"                                                          // COPY
    "      case 8: c = float4(x.rgb + c.rgb, c.a); break;\n"                                 // ADD
    "      default: c = float4(x.rgb * c.rgb, u.tex_alpha != 0 ? x.a : c.a); break;\n"       // MODULATE
    "    }\n"
    "  }\n"
    "  if (u.use_spec != 0) c.rgb += i.spec.rgb;\n"
    "  if (u.alphatest != 0) {\n"
    "    bool ok = true;\n"
    "    switch (u.alphafunc) {\n"
    "      case 1: ok = false; break; case 2: ok = c.a < u.alpharef; break; case 3: ok = c.a == u.alpharef; break;\n"
    "      case 4: ok = c.a <= u.alpharef; break; case 5: ok = c.a > u.alpharef; break; case 6: ok = c.a != u.alpharef; break;\n"
    "      case 7: ok = c.a >= u.alpharef; break; default: break;\n"
    "    }\n"
    "    if (!ok) discard_fragment();\n"
    "  }\n"
    "  return c;\n"
    "}\n";

static const char *MSL_PRESENT =
    "#include <metal_stdlib>\n"
    "using namespace metal;\n"
    "struct QV { float4 pos [[position]]; float2 uv; };\n"
    "vertex QV q_vs(uint vid [[vertex_id]]) {\n"
    "  float2 p = float2((vid & 1) ? 1.0 : -1.0, (vid & 2) ? 1.0 : -1.0);\n"
    "  QV o; o.pos = float4(p, 0.0, 1.0); o.uv = float2(p.x * 0.5 + 0.5, 0.5 - p.y * 0.5); return o;\n"
    "}\n"
    "struct QU { float2 size; int use_hi; int sharp; };\n"
    "fragment float4 q_fs(QV i [[stage_in]], constant QU& u [[buffer(0)]],\n"
    "    texture2d<float> ui [[texture(0)]], texture2d<float> hi [[texture(1)]], texture2d<float> mask [[texture(2)]],\n"
    "    sampler s_lin [[sampler(0)]], sampler s_hi [[sampler(1)]], sampler s_near [[sampler(2)]]) {\n"
    "  float2 uv = i.uv;\n"
    "  if (u.sharp != 0) {\n"                                                       // sharp bilinear for the 2D picture
    "    float2 px = uv * u.size; float2 seam = floor(px + 0.5); float2 d = fwidth(px);\n"
    "    uv = (seam + clamp((px - seam) / d, -0.5, 0.5)) / u.size;\n"
    "  }\n"
    "  float4 c = ui.sample(s_lin, uv);\n"
    "  if (u.use_hi != 0 && mask.sample(s_near, i.uv).r > 0.5) c = hi.sample(s_hi, i.uv);\n"
    "  return float4(c.rgb, 1.0);\n"
    "}\n";

typedef struct GVertex {      // 32 bytes
    float x, y, z, w;         // clip space
    uint32_t color, spec;     // D3DCOLOR (ARGB, little endian = B G R A in memory)
    float u, v;
} GVertex;

typedef struct FragU {
    float alpharef; int32_t alphafunc, texblend, use_tex, use_spec, alphatest, tex_alpha, pad;
} FragU;

typedef struct PipeKey {
    uint8_t blend, src, dst, ztest, zwrite, zfunc, cull, topo;
} PipeKey;

typedef struct TexVer {       // one uploaded version of a surface
    SDL_GPUTexture *tex;
    int w, h, has_alpha;
    uint32_t stage_off;       // offset of the pixels in the upload staging area, until uploaded
    int pending;
} TexVer;

typedef struct TexEntry {
    TexVer *ver;
    uint32_t gen, pal_gen; int ckey;
} TexEntry;

typedef enum { CMD_DRAW, CMD_CLEAR } CmdKind;
typedef struct Cmd {
    CmdKind kind;
    PipeKey key;
    FragU u;
    TexVer *tex;
    int filter, wrap;
    uint32_t first, count;                // vertices
    int clear_color, clear_depth; float rgba[4];
    int scissor[4];
} Cmd;

static SDL_GPUDevice *dev;
static SDL_Window *gwin;                 // window claimed for presentation (NULL: off-screen only)
static SDL_GPUShader *vs, *fs, *qvs, *qfs;
static SDL_GPUSampler *samplers[3][2];   // [0 nearest, 1 bilinear, 2 trilinear + anisotropic][wrap, clamp]
static SDL_GPUSampler *s_near, *s_lin;   // for the presenter
static SDL_GPUTexture *tex_white;        // bound when untextured
static SDL_GPUGraphicsPipeline *present_pipe;
static int msaa = 1;                     // samples per pixel of the 3D target
static int force_scale;                  // POPTB_D3D_SCALE override
static int smooth = 1;                   // textures: 1 = always trilinear/anisotropic, 0 = as the game asks

#define MAX_PIPES 1024
static struct { PipeKey k; SDL_GPUGraphicsPipeline *p; } pipes[MAX_PIPES];
static int npipes;

// 3D target: rt_msaa (when multisampled) resolves into rt_color (hi-res); rt_1x is the downsampled copy
static SDL_GPUTexture *rt_msaa, *rt_color, *rt_depth, *rt_1x;
static int rt_w, rt_h;                   // target size in game pixels (the surface)
static int rt_scale = 1;
static uint32_t *snap;                   // 1x copy handed to the game: 565 value | 0x10000 where a scene wrote it
static int hires_valid;

// per-scene recording
static GVertex *verts; static uint32_t nverts, capverts;
static Cmd *cmds; static int ncmds, capcmds;
static TexVer **scene_texs; static int nscene_texs, capscene_texs;   // versions created this scene
static uint8_t *stage; static uint32_t stage_used, stage_cap;
static TexVer **garbage; static int ngarbage, capgarbage;
static TexEntry *tex_entries; static int ntex_entries, captex_entries;

static uint32_t rs_cur[D3D_NRS];
static Surf *cur_tex;
static int vp_rect[4];     // x, y, w, h of the current viewport
static int target_w, target_h;
static int first_pass = 1;
static int dbg_scenes;
static SDL_Mutex *gpu_mu;
// The GPU lock is taken with the GIL held and the GIL is released while waiting on the GPU, so never block on it with the GIL held
static void gpu_lock(void) {
    while (!SDL_TryLockMutex(gpu_mu)) { gil_unlock(); SDL_Delay(1); gil_lock(); }
}

#define GROW(arr, n, cap, T) do { if ((n) >= (cap)) { (cap) = (cap) ? (cap) * 2 : 256; (arr) = realloc((arr), (size_t)(cap) * sizeof(T)); } } while (0)

// ---------------------------------------------------------------------------------------------
static SDL_GPUShader *make_shader(const char *src, const char *entry, SDL_GPUShaderStage stage_, int nsamp, int nubo) {
    SDL_GPUShaderCreateInfo si = {.code = (const Uint8 *)src, .code_size = strlen(src), .entrypoint = entry,
        .format = SDL_GPU_SHADERFORMAT_MSL, .stage = stage_, .num_samplers = (Uint32)nsamp, .num_uniform_buffers = (Uint32)nubo};
    SDL_GPUShader *sh = SDL_CreateGPUShader(dev, &si);
    if (!sh) WARN("D3D: shader %s: %s", entry, SDL_GetError());
    return sh;
}

static bool create_device(void) {
    if (dev) return true;
    d3d_default_states(rs_cur);
    gpu_mu = SDL_CreateMutex();
    dev = SDL_CreateGPUDevice(SDL_GPU_SHADERFORMAT_MSL, false, NULL);
    if (!dev) { WARN("D3D: SDL_CreateGPUDevice: %s", SDL_GetError()); return false; }
    LOG("D3D: SDL_GPU driver %s", SDL_GetGPUDeviceDriver(dev));
    const char *e = getenv("POPTB_D3D_MSAA");
    int want = e ? atoi(e) : 4;
    msaa = 1;
    if (want >= 8 && SDL_GPUTextureSupportsSampleCount(dev, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, SDL_GPU_SAMPLECOUNT_8)) msaa = 8;
    else if (want >= 4 && SDL_GPUTextureSupportsSampleCount(dev, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, SDL_GPU_SAMPLECOUNT_4)) msaa = 4;
    else if (want >= 2 && SDL_GPUTextureSupportsSampleCount(dev, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, SDL_GPU_SAMPLECOUNT_2)) msaa = 2;
    while (msaa > 1 && !SDL_GPUTextureSupportsSampleCount(dev, SDL_GPU_TEXTUREFORMAT_D32_FLOAT, msaa == 8 ? SDL_GPU_SAMPLECOUNT_8 : msaa == 4 ? SDL_GPU_SAMPLECOUNT_4 : SDL_GPU_SAMPLECOUNT_2)) msaa /= 2;
    if ((e = getenv("POPTB_D3D_SCALE"))) force_scale = atoi(e);
    if ((e = getenv("POPTB_D3D_FILTER"))) smooth = strcmp(e, "game") != 0;
    return true;
}

bool d3d_gpu_init(void) {
    static bool done;
    if (done) return true;
    if (!create_device()) return false;
    vs = make_shader(MSL, "vs_main", SDL_GPU_SHADERSTAGE_VERTEX, 0, 1);
    fs = make_shader(MSL, "fs_main", SDL_GPU_SHADERSTAGE_FRAGMENT, 1, 1);
    if (!vs || !fs) return false;
    for (int mode = 0; mode < 3; mode++) for (int clamp = 0; clamp < 2; clamp++) {
        SDL_GPUSamplerCreateInfo sc = {
            .min_filter = mode ? SDL_GPU_FILTER_LINEAR : SDL_GPU_FILTER_NEAREST,
            .mag_filter = mode ? SDL_GPU_FILTER_LINEAR : SDL_GPU_FILTER_NEAREST,
            .mipmap_mode = mode == 2 ? SDL_GPU_SAMPLERMIPMAPMODE_LINEAR : SDL_GPU_SAMPLERMIPMAPMODE_NEAREST,
            .address_mode_u = clamp ? SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE : SDL_GPU_SAMPLERADDRESSMODE_REPEAT,
            .address_mode_v = clamp ? SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE : SDL_GPU_SAMPLERADDRESSMODE_REPEAT,
            .address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_REPEAT,
            .max_anisotropy = 8, .enable_anisotropy = mode == 2,
            .min_lod = 0, .max_lod = mode == 2 ? 16 : 0};
        samplers[mode][clamp] = SDL_CreateGPUSampler(dev, &sc);
    }
    // 1x1 white texture for untextured draws (a texture must be bound even if the shader does not sample it)
    SDL_GPUTextureCreateInfo tc = {.type = SDL_GPU_TEXTURETYPE_2D, .format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM,
        .usage = SDL_GPU_TEXTUREUSAGE_SAMPLER, .width = 1, .height = 1, .layer_count_or_depth = 1, .num_levels = 1};
    tex_white = SDL_CreateGPUTexture(dev, &tc);
    SDL_GPUTransferBufferCreateInfo ti = {.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD, .size = 4};
    SDL_GPUTransferBuffer *tb = SDL_CreateGPUTransferBuffer(dev, &ti);
    uint32_t *p = SDL_MapGPUTransferBuffer(dev, tb, false); *p = 0xffffffffu; SDL_UnmapGPUTransferBuffer(dev, tb);
    SDL_GPUCommandBuffer *cb = SDL_AcquireGPUCommandBuffer(dev);
    SDL_GPUCopyPass *cp = SDL_BeginGPUCopyPass(cb);
    SDL_GPUTextureTransferInfo src = {.transfer_buffer = tb};
    SDL_GPUTextureRegion dst = {.texture = tex_white, .w = 1, .h = 1, .d = 1};
    SDL_UploadToGPUTexture(cp, &src, &dst, false);
    SDL_EndGPUCopyPass(cp);
    SDL_SubmitGPUCommandBuffer(cb);
    SDL_ReleaseGPUTransferBuffer(dev, tb);
    LOG("D3D: MSAA x%d, textures %s", msaa, smooth ? "trilinear + anisotropic" : "as requested by the game");
    done = true;
    return true;
}

static SDL_GPUGraphicsPipeline *get_pipeline(PipeKey k) {
    for (int i = 0; i < npipes; i++) if (!memcmp(&pipes[i].k, &k, sizeof k)) return pipes[i].p;
    static const SDL_GPUBlendFactor bf[14] = {
        SDL_GPU_BLENDFACTOR_ONE, SDL_GPU_BLENDFACTOR_ZERO, SDL_GPU_BLENDFACTOR_ONE, SDL_GPU_BLENDFACTOR_SRC_COLOR,
        SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_COLOR, SDL_GPU_BLENDFACTOR_SRC_ALPHA, SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA,
        SDL_GPU_BLENDFACTOR_DST_ALPHA, SDL_GPU_BLENDFACTOR_ONE_MINUS_DST_ALPHA, SDL_GPU_BLENDFACTOR_DST_COLOR,
        SDL_GPU_BLENDFACTOR_ONE_MINUS_DST_COLOR, SDL_GPU_BLENDFACTOR_SRC_ALPHA_SATURATE, SDL_GPU_BLENDFACTOR_SRC_ALPHA,
        SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA};
    static const SDL_GPUCompareOp cmp[9] = {
        SDL_GPU_COMPAREOP_ALWAYS, SDL_GPU_COMPAREOP_NEVER, SDL_GPU_COMPAREOP_LESS, SDL_GPU_COMPAREOP_EQUAL,
        SDL_GPU_COMPAREOP_LESS_OR_EQUAL, SDL_GPU_COMPAREOP_GREATER, SDL_GPU_COMPAREOP_NOT_EQUAL,
        SDL_GPU_COMPAREOP_GREATER_OR_EQUAL, SDL_GPU_COMPAREOP_ALWAYS};
    SDL_GPUBlendFactor sf = bf[k.src < 14 ? k.src : 2], df = bf[k.dst < 14 ? k.dst : 1];
    if (k.src == 12) { sf = SDL_GPU_BLENDFACTOR_SRC_ALPHA; df = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA; }       // BOTHSRCALPHA
    if (k.src == 13) { sf = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA; df = SDL_GPU_BLENDFACTOR_SRC_ALPHA; }       // BOTHINVSRCALPHA
    SDL_GPUColorTargetDescription ct = {.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, .blend_state = {
        .src_color_blendfactor = sf, .dst_color_blendfactor = df, .color_blend_op = SDL_GPU_BLENDOP_ADD,
        .src_alpha_blendfactor = sf, .dst_alpha_blendfactor = df, .alpha_blend_op = SDL_GPU_BLENDOP_ADD,
        .enable_blend = k.blend != 0}};
    SDL_GPUVertexBufferDescription vb = {.slot = 0, .pitch = sizeof(GVertex), .input_rate = SDL_GPU_VERTEXINPUTRATE_VERTEX};
    SDL_GPUVertexAttribute va[4] = {
        {.location = 0, .format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4, .offset = offsetof(GVertex, x)},
        {.location = 1, .format = SDL_GPU_VERTEXELEMENTFORMAT_UBYTE4_NORM, .offset = offsetof(GVertex, color)},
        {.location = 2, .format = SDL_GPU_VERTEXELEMENTFORMAT_UBYTE4_NORM, .offset = offsetof(GVertex, spec)},
        {.location = 3, .format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, .offset = offsetof(GVertex, u)}};
    SDL_GPUGraphicsPipelineCreateInfo pi = {
        .vertex_shader = vs, .fragment_shader = fs,
        .vertex_input_state = {.vertex_buffer_descriptions = &vb, .num_vertex_buffers = 1, .vertex_attributes = va, .num_vertex_attributes = 4},
        .primitive_type = k.topo == 1 ? SDL_GPU_PRIMITIVETYPE_LINELIST : k.topo == 2 ? SDL_GPU_PRIMITIVETYPE_POINTLIST : SDL_GPU_PRIMITIVETYPE_TRIANGLELIST,
        .rasterizer_state = {.fill_mode = SDL_GPU_FILLMODE_FILL, .front_face = SDL_GPU_FRONTFACE_CLOCKWISE,
            .cull_mode = k.cull == 2 ? SDL_GPU_CULLMODE_FRONT : k.cull == 3 ? SDL_GPU_CULLMODE_BACK : SDL_GPU_CULLMODE_NONE,
            .enable_depth_clip = true},
        .multisample_state = {.sample_count = msaa == 8 ? SDL_GPU_SAMPLECOUNT_8 : msaa == 4 ? SDL_GPU_SAMPLECOUNT_4 : msaa == 2 ? SDL_GPU_SAMPLECOUNT_2 : SDL_GPU_SAMPLECOUNT_1},
        .depth_stencil_state = {.compare_op = k.ztest ? cmp[k.zfunc < 9 ? k.zfunc : 4] : SDL_GPU_COMPAREOP_ALWAYS,
            .enable_depth_test = k.ztest != 0, .enable_depth_write = k.ztest && k.zwrite},
        .target_info = {.color_target_descriptions = &ct, .num_color_targets = 1,
            .depth_stencil_format = SDL_GPU_TEXTUREFORMAT_D32_FLOAT, .has_depth_stencil_target = true}};
    SDL_GPUGraphicsPipeline *p = SDL_CreateGPUGraphicsPipeline(dev, &pi);
    if (!p) WARN("D3D: pipeline creation failed: %s", SDL_GetError());
    if (npipes < MAX_PIPES) { pipes[npipes].k = k; pipes[npipes++].p = p; }
    else if (p) { WARN("D3D: pipeline cache full"); SDL_ReleaseGPUGraphicsPipeline(dev, p); return NULL; }   // TODO: evict instead of dropping the draw
    return p;
}

// hi-res factor: window pixels per game pixel, rounded; the 3D target is that much larger than the surface
static int wanted_scale(int w, int h) {
    if (force_scale > 0) return force_scale;
    if (!gwin) return 1;
    int pw, ph;
    SDL_GetWindowSizeInPixels(gwin, &pw, &ph);
    float f = (float)pw / (float)w < (float)ph / (float)h ? (float)pw / (float)w : (float)ph / (float)h;
    int s = (int)(f + 0.5f);
    return s < 1 ? 1 : s > 4 ? 4 : s;
}

static void ensure_target(int w, int h) {
    int sc = wanted_scale(w, h);
    if (rt_color && rt_w == w && rt_h == h && rt_scale == sc) return;
    if (rt_color) {
        SDL_WaitForGPUIdle(dev);
        SDL_ReleaseGPUTexture(dev, rt_color); SDL_ReleaseGPUTexture(dev, rt_depth);
        if (rt_msaa) SDL_ReleaseGPUTexture(dev, rt_msaa);
        if (rt_1x) SDL_ReleaseGPUTexture(dev, rt_1x);
        rt_msaa = rt_1x = NULL;
    }
    SDL_GPUSampleCount smp = msaa == 8 ? SDL_GPU_SAMPLECOUNT_8 : msaa == 4 ? SDL_GPU_SAMPLECOUNT_4 : msaa == 2 ? SDL_GPU_SAMPLECOUNT_2 : SDL_GPU_SAMPLECOUNT_1;
    SDL_GPUTextureCreateInfo tc = {.type = SDL_GPU_TEXTURETYPE_2D, .format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM,
        .usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER, .width = (Uint32)(w * sc), .height = (Uint32)(h * sc),
        .layer_count_or_depth = 1, .num_levels = 1};
    rt_color = SDL_CreateGPUTexture(dev, &tc);
    if (msaa > 1) {
        tc.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET; tc.sample_count = smp;
        rt_msaa = SDL_CreateGPUTexture(dev, &tc);
    }
    tc.format = SDL_GPU_TEXTUREFORMAT_D32_FLOAT; tc.usage = SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET; tc.sample_count = smp;
    rt_depth = SDL_CreateGPUTexture(dev, &tc);
    if (sc > 1) {
        tc.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM; tc.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
        tc.width = (Uint32)w; tc.height = (Uint32)h; tc.sample_count = SDL_GPU_SAMPLECOUNT_1;
        rt_1x = SDL_CreateGPUTexture(dev, &tc);
    }
    free(snap); snap = calloc((size_t)w * h, sizeof *snap);
    rt_w = w; rt_h = h; rt_scale = sc; first_pass = 1; hires_valid = 0;
    LOG("D3D: render target %dx%d (%dx), %dx MSAA", w * sc, h * sc, sc, msaa);
}

// ---------------------------------------------------------------------------------------------
// textures

static uint32_t expand_bits(uint32_t v, uint32_t mask, int *has) {
    if (!mask) { *has = 0; return 0xff; }
    *has = 1;
    int sh = 0; while (!((mask >> sh) & 1)) sh++;
    uint32_t max = mask >> sh, x = (v & mask) >> sh;
    return max ? (x * 255 + max / 2) / max : 0;
}

// converts a guest surface to RGBA8 (R,G,B,A byte order) at dst; returns whether the format carries alpha
static int convert_surface(const Surf *s, uint8_t *dst, int ckey) {
    int has_a = 0;
    const Pal *pal = s->pal ? com_native(s->pal) : NULL;
    for (int y = 0; y < s->h; y++) {
        const uint8_t *row = PTR(s->mem + (uint32_t)(y * s->pitch));
        uint8_t *o = dst + (size_t)y * s->w * 4;
        for (int x = 0; x < s->w; x++, o += 4) {
            uint32_t raw, r, g, b, a = 255;
            if (s->bpp == 8) {
                raw = row[x];
                uint32_t rgb = pal ? pal->rgb[raw] : raw * 0x010101;
                r = rgb >> 16 & 255; g = rgb >> 8 & 255; b = rgb & 255;
            } else {
                raw = *(const u16u *)(row + 2 * x);
                int ha, hr, hg, hb;
                uint32_t rm = s->rmask ? s->rmask : 0xf800, gm = s->gmask ? s->gmask : 0x07e0, bm = s->bmask ? s->bmask : 0x001f;
                r = expand_bits(raw, rm, &hr); g = expand_bits(raw, gm, &hg); b = expand_bits(raw, bm, &hb);
                a = expand_bits(raw, s->amask, &ha);
                if (ha) has_a = 1;
            }
            if (ckey && s->has_ck && raw == s->ck_lo) a = 0;
            o[0] = (uint8_t)r; o[1] = (uint8_t)g; o[2] = (uint8_t)b; o[3] = (uint8_t)a;
        }
    }
    return has_a || (ckey && s->has_ck);
}

static int mip_levels(int w, int h) {
    int n = 1;
    while (w > 1 || h > 1) { w = w > 1 ? w / 2 : 1; h = h > 1 ? h / 2 : 1; n++; }
    return n;
}

static TexVer *texture_version(Surf *s, int ckey) {
    if (!s->gpu_slot) {
        GROW(tex_entries, ntex_entries, captex_entries, TexEntry);
        memset(&tex_entries[ntex_entries], 0, sizeof tex_entries[0]);
        s->gpu_slot = ++ntex_entries;
    }
    TexEntry *e = &tex_entries[s->gpu_slot - 1];
    uint32_t pg = s->bpp == 8 ? g_pal_gen : 0;
    if (e->ver && e->gen == s->gen && e->pal_gen == pg && e->ckey == ckey) return e->ver;
    static int ntexlog; if (rt_verbose && ntexlog++ < 80) LOG("D3D texture %dx%d bpp=%d pal=%x pf=%x amask=%x ckey=%d/%d", s->w, s->h, s->bpp, s->pal, s->pf_flags, s->amask, s->has_ck, ckey);
    TexVer *v = calloc(1, sizeof *v);
    v->w = s->w; v->h = s->h;
    SDL_GPUTextureCreateInfo tc = {.type = SDL_GPU_TEXTURETYPE_2D, .format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM,
        .usage = SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COLOR_TARGET,   // COLOR_TARGET: needed to generate mip maps
        .width = (Uint32)s->w, .height = (Uint32)s->h, .layer_count_or_depth = 1, .num_levels = (Uint32)mip_levels(s->w, s->h)};
    v->tex = SDL_CreateGPUTexture(dev, &tc);
    uint32_t bytes = (uint32_t)s->w * (uint32_t)s->h * 4;
    if (stage_used + bytes > stage_cap) {
        stage_cap = (stage_used + bytes) * 2;
        stage = realloc(stage, stage_cap);
    }
    v->stage_off = stage_used;
    v->has_alpha = convert_surface(s, stage + stage_used, ckey);
    if (getenv("POPTB_D3D_DUMP_TEX")) {   // debugging: save the first textures as BMP files into that folder
        static int ndump;
        if (ndump < 60) {
            char path[1100]; snprintf(path, sizeof path, "%s/tex_%03d_%dx%d_a%x.bmp", getenv("POPTB_D3D_DUMP_TEX"), ndump++, s->w, s->h, s->amask);
            SDL_Surface *sf = SDL_CreateSurfaceFrom(s->w, s->h, SDL_PIXELFORMAT_RGBA32, stage + stage_used, s->w * 4);
            SDL_SaveBMP(sf, path);
            SDL_DestroySurface(sf);
        }
    }
    stage_used += bytes;
    v->pending = 1;
    GROW(scene_texs, nscene_texs, capscene_texs, TexVer *);
    scene_texs[nscene_texs++] = v;
    if (e->ver) { GROW(garbage, ngarbage, capgarbage, TexVer *); garbage[ngarbage++] = e->ver; }
    e->ver = v; e->gen = s->gen; e->pal_gen = pg; e->ckey = ckey;
    return v;
}

// ---------------------------------------------------------------------------------------------
// recording

void d3d_gpu_set_texture(Surf *tex) { cur_tex = tex; }
void d3d_gpu_set_state(uint32_t st, uint32_t v) { if (st < D3D_NRS) rs_cur[st] = v; }
void d3d_gpu_set_viewport(int x, int y, int w, int h) { vp_rect[0] = x; vp_rect[1] = y; vp_rect[2] = w; vp_rect[3] = h; }

void d3d_gpu_begin_scene(Surf *target) {
    if (!target) return;
    target_w = target->w; target_h = target->h;
}

static void scissor_of(Cmd *c) {
    int x0 = vp_rect[0], y0 = vp_rect[1], x1 = x0 + vp_rect[2], y1 = y0 + vp_rect[3];
    if (vp_rect[2] <= 0 || vp_rect[3] <= 0) { x0 = 0; y0 = 0; x1 = target_w; y1 = target_h; }
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > target_w) x1 = target_w;
    if (y1 > target_h) y1 = target_h;
    if (x1 < x0) x1 = x0;
    if (y1 < y0) y1 = y0;
    c->scissor[0] = x0; c->scissor[1] = y0; c->scissor[2] = x1 - x0; c->scissor[3] = y1 - y0;
}

void d3d_gpu_clear(int color, int depth, const float rgba[4], const int rect[4]) {
    GROW(cmds, ncmds, capcmds, Cmd);
    Cmd *c = &cmds[ncmds++];
    memset(c, 0, sizeof *c);
    c->kind = CMD_CLEAR; c->clear_color = color; c->clear_depth = depth;
    memcpy(c->rgba, rgba, sizeof c->rgba);
    c->scissor[0] = rect[0]; c->scissor[1] = rect[1]; c->scissor[2] = rect[2] - rect[0]; c->scissor[3] = rect[3] - rect[1];
}

static void put_vertex(GVertex *o, const D3DTLVertex *v) {
    float w = v->rhw != 0 ? 1.0f / v->rhw : 1.0f;
    o->x = (v->sx / (float)target_w * 2.0f - 1.0f) * w;
    o->y = (1.0f - v->sy / (float)target_h * 2.0f) * w;
    o->z = v->sz * w; o->w = w;
    o->color = v->color; o->spec = v->specular;
    o->u = v->tu; o->v = v->tv;
}

void d3d_gpu_draw(uint32_t prim, const D3DTLVertex *v, int n, const uint16_t *idx, int ni) {
    if (!dev || target_w == 0) return;
    int cnt = idx ? ni : n;
    int topo = 0;   // 0 triangles, 1 lines, 2 points
    uint32_t base = nverts;
    #define V(i) (&v[idx ? idx[(i)] : (i)])
    #define EMIT(p) do { GROW(verts, nverts, capverts, GVertex); put_vertex(&verts[nverts++], (p)); } while (0)
    switch (prim) {
    case 1: topo = 2; for (int i = 0; i < cnt; i++) EMIT(V(i)); break;                                           // POINTLIST
    case 2: topo = 1; for (int i = 0; i + 1 < cnt; i += 2) { EMIT(V(i)); EMIT(V(i + 1)); } break;               // LINELIST
    case 3: topo = 1; for (int i = 0; i + 1 < cnt; i++) { EMIT(V(i)); EMIT(V(i + 1)); } break;                  // LINESTRIP
    case 4: for (int i = 0; i + 2 < cnt; i += 3) { EMIT(V(i)); EMIT(V(i + 1)); EMIT(V(i + 2)); } break;         // TRIANGLELIST
    case 5:                                                                                                      // TRIANGLESTRIP
        for (int i = 0; i + 2 < cnt; i++) {
            if (i & 1) { EMIT(V(i + 1)); EMIT(V(i)); EMIT(V(i + 2)); }
            else { EMIT(V(i)); EMIT(V(i + 1)); EMIT(V(i + 2)); }
        }
        break;
    case 6: for (int i = 1; i + 1 < cnt; i++) { EMIT(V(0)); EMIT(V(i)); EMIT(V(i + 1)); } break;               // TRIANGLEFAN
    default: WARN("D3D: unknown primitive type %u", prim); return;
    }
    #undef V
    #undef EMIT
    uint32_t count = nverts - base;
    if (!count) return;

    Cmd c; memset(&c, 0, sizeof c);
    c.kind = CMD_DRAW;
    c.key.blend = rs_cur[RS_ALPHABLENDENABLE] != 0;
    c.key.src = (uint8_t)rs_cur[RS_SRCBLEND]; c.key.dst = (uint8_t)rs_cur[RS_DESTBLEND];
    c.key.ztest = rs_cur[RS_ZENABLE] != 0; c.key.zwrite = rs_cur[RS_ZWRITEENABLE] != 0; c.key.zfunc = (uint8_t)rs_cur[RS_ZFUNC];
    c.key.cull = (uint8_t)rs_cur[RS_CULLMODE]; c.key.topo = (uint8_t)topo;
    int ckey = rs_cur[RS_COLORKEYENABLE] != 0;
    TexVer *tv = cur_tex ? texture_version(cur_tex, ckey) : NULL;
    c.tex = tv;
    c.u.use_tex = tv != NULL; c.u.tex_alpha = tv && tv->has_alpha;
    c.u.texblend = (int32_t)rs_cur[RS_TEXTUREMAPBLEND]; c.u.use_spec = rs_cur[RS_SPECULARENABLE] != 0;
    c.u.alphatest = rs_cur[RS_ALPHATESTENABLE] != 0; c.u.alphafunc = (int32_t)rs_cur[RS_ALPHAFUNC];
    c.u.alpharef = (float)(rs_cur[RS_ALPHAREF] & 0xff) / 255.0f;
    if (ckey && cur_tex && cur_tex->has_ck && !c.u.alphatest) {   // keyed texels have alpha 0: do not draw them
        c.u.alphatest = 1; c.u.alphafunc = 7; c.u.alpharef = 0.5f;   // D3DCMP_GREATEREQUAL
    }
    c.filter = smooth ? 2 : rs_cur[RS_TEXTUREMAG] >= 2 ? 1 : 0;
    c.wrap = !(rs_cur[RS_TEXTUREADDRESS] == 3 || rs_cur[RS_TEXTUREADDRESSU] == 3);   // CLAMP
    scissor_of(&c);
    c.first = base; c.count = count;
    if (rt_verbose) {   // log every distinct state combination once
        static uint64_t seen[96]; static int nseen;
        uint64_t sig = (uint64_t)c.key.blend | (uint64_t)c.key.src << 4 | (uint64_t)c.key.dst << 8 | (uint64_t)c.u.texblend << 12 |
            (uint64_t)c.key.ztest << 16 | (uint64_t)c.key.zwrite << 17 | (uint64_t)c.u.alphatest << 18 | (uint64_t)c.u.alphafunc << 20 |
            (uint64_t)c.u.tex_alpha << 24 | (uint64_t)c.u.use_tex << 25 | (uint64_t)ckey << 26 | (uint64_t)c.key.cull << 28 | (uint64_t)prim << 32 |
            (uint64_t)c.u.use_spec << 36 | (uint64_t)(cur_tex ? cur_tex->amask != 0 : 0) << 37;
        int known = 0;
        for (int i = 0; i < nseen; i++) if (seen[i] == sig) { known = 1; break; }
        if (!known && nseen < 96) {
            seen[nseen++] = sig;
            LOG("D3D state: prim=%u blend=%d %u->%u texblend=%d tex=%d(alpha %d) ckey=%d ztest=%d zwrite=%d atest=%d func=%d ref=%.2f cull=%d spec=%d  v0 sx=%g sy=%g rhw=%g col=%08x uv=%g,%g",
                prim, c.key.blend, c.key.src, c.key.dst, c.u.texblend, c.u.use_tex, c.u.tex_alpha, ckey, c.key.ztest, c.key.zwrite,
                c.u.alphatest, c.u.alphafunc, c.u.alpharef, c.key.cull, c.u.use_spec, v[0].sx, v[0].sy, v[0].rhw, v[0].color, v[0].tu, v[0].tv);
        }
    }
    // merge with the previous draw when nothing differs
    if (ncmds && cmds[ncmds - 1].kind == CMD_DRAW) {
        Cmd *p = &cmds[ncmds - 1];
        if (p->first + p->count == base && !memcmp(&p->key, &c.key, sizeof c.key) && !memcmp(&p->u, &c.u, sizeof c.u) &&
            p->tex == c.tex && p->filter == c.filter && p->wrap == c.wrap && !memcmp(p->scissor, c.scissor, sizeof c.scissor)) {
            p->count += count;
            return;
        }
    }
    GROW(cmds, ncmds, capcmds, Cmd);
    cmds[ncmds++] = c;
}

// ---------------------------------------------------------------------------------------------
// execution

// Pooled per-scene buffers. EndScene waits for the scene's fence, so a buffer is idle when the next scene starts
// and can be reused; it only grows.
static SDL_GPUTransferBuffer *up_tb, *down_tb;
static SDL_GPUBuffer *vtx_buf;
static Uint32 up_cap, down_cap, vtx_cap;

static Uint32 pool_size(Uint32 need) {
    Uint32 n = 1u << 16;
    while (n < need) n *= 2;
    return n;
}

static SDL_GPUTransferBuffer *pool_transfer(SDL_GPUTransferBuffer **tb, Uint32 *cap, SDL_GPUTransferBufferUsage usage, Uint32 need) {
    if (*tb && *cap >= need) return *tb;
    if (*tb) SDL_ReleaseGPUTransferBuffer(dev, *tb);
    *cap = pool_size(need);
    SDL_GPUTransferBufferCreateInfo ti = {.usage = usage, .size = *cap};
    return *tb = SDL_CreateGPUTransferBuffer(dev, &ti);
}

static SDL_GPUBuffer *pool_vertex_buffer(Uint32 need) {
    if (vtx_buf && vtx_cap >= need) return vtx_buf;
    if (vtx_buf) SDL_ReleaseGPUBuffer(dev, vtx_buf);
    vtx_cap = pool_size(need);
    SDL_GPUBufferCreateInfo bi = {.usage = SDL_GPU_BUFFERUSAGE_VERTEX, .size = vtx_cap};
    return vtx_buf = SDL_CreateGPUBuffer(dev, &bi);
}

// The rectangles to copy back: the distinct areas the scene's draws and clears were limited to (the game switches
// viewports within a scene). Copying only these, not their bounding box, leaves the 2D content between them intact.
// TODO: pixels inside a rectangle that no primitive covered are still overwritten (hardware would keep the old content).
#define MAX_READBACK_RECTS 16
typedef struct Rect { int x, y, w, h; } Rect;

static int rect_contains(const Rect *a, const Rect *b) {
    return b->x >= a->x && b->y >= a->y && b->x + b->w <= a->x + a->w && b->y + b->h <= a->y + a->h;
}

static int collect_rects(const Surf *t, Rect *out) {
    int n = 0;
    for (int i = 0; i < ncmds; i++) {
        const int *sc = cmds[i].scissor;
        int x0 = sc[0] < 0 ? 0 : sc[0], y0 = sc[1] < 0 ? 0 : sc[1];
        int x1 = sc[0] + sc[2] > t->w ? t->w : sc[0] + sc[2], y1 = sc[1] + sc[3] > t->h ? t->h : sc[1] + sc[3];
        if (x1 <= x0 || y1 <= y0) continue;
        Rect r = {x0, y0, x1 - x0, y1 - y0};
        int covered = 0;
        for (int j = 0; j < n && !covered; j++) covered = rect_contains(&out[j], &r);
        if (covered) continue;
        for (int j = 0; j < n; ) {   // drop rectangles the new one covers
            if (rect_contains(&r, &out[j])) out[j] = out[--n]; else j++;
        }
        if (n < MAX_READBACK_RECTS) { out[n++] = r; continue; }
        Rect *l = &out[n - 1];   // too many: merge into the last one
        int ux1 = l->x + l->w > r.x + r.w ? l->x + l->w : r.x + r.w, uy1 = l->y + l->h > r.y + r.h ? l->y + l->h : r.y + r.h;
        l->x = l->x < r.x ? l->x : r.x; l->y = l->y < r.y ? l->y : r.y;
        l->w = ux1 - l->x; l->h = uy1 - l->y;
    }
    return n;
}

static void write_back(Surf *t, const uint8_t *rgba, int x0, int y0, int w, int h) {
    for (int y = 0; y < h; y++) {
        const uint8_t *s = rgba + (size_t)y * w * 4;
        int yy = y0 + y;
        uint8_t *d = PTR(t->mem + (uint32_t)(yy * t->pitch + x0 * 2));
        uint32_t *sn = snap + (size_t)yy * t->w + x0;
        for (int x = 0; x < w; x++, s += 4, d += 2) {
            uint16_t c = (uint16_t)((s[0] >> 3) << 11 | (s[1] >> 2) << 5 | (s[2] >> 3));
            d[0] = (uint8_t)c; d[1] = (uint8_t)(c >> 8);
            sn[x] = c | 0x10000u;
        }
    }
    t->gen++;
}

static void end_scene_locked(Surf *target) {
    if (!dev || !target) return;
    if (!ncmds || target->bpp != 16) {
        if (ncmds) WARN("D3D: render target with %d bpp is not supported", target->bpp);
        ncmds = 0; nverts = 0; stage_used = 0; nscene_texs = 0;
        return;
    }
    ensure_target(target->w, target->h);
    int S = rt_scale;

    // ---- upload: vertices and the textures converted during this scene
    uint32_t vbytes = nverts * (uint32_t)sizeof(GVertex);
    uint32_t total = vbytes + stage_used;
    SDL_GPUTransferBuffer *up = pool_transfer(&up_tb, &up_cap, SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD, total);
    uint8_t *m = SDL_MapGPUTransferBuffer(dev, up, false);
    memcpy(m, verts, vbytes);
    if (stage_used) memcpy(m + vbytes, stage, stage_used);
    SDL_UnmapGPUTransferBuffer(dev, up);
    SDL_GPUBuffer *vb = pool_vertex_buffer(vbytes);

    SDL_GPUCommandBuffer *cb = SDL_AcquireGPUCommandBuffer(dev);
    SDL_GPUCopyPass *cp = SDL_BeginGPUCopyPass(cb);
    if (vbytes) {
        SDL_GPUTransferBufferLocation src = {.transfer_buffer = up, .offset = 0};
        SDL_GPUBufferRegion dst = {.buffer = vb, .offset = 0, .size = vbytes};
        SDL_UploadToGPUBuffer(cp, &src, &dst, false);
    }
    for (int i = 0; i < nscene_texs; i++) {
        TexVer *t = scene_texs[i];
        if (!t->pending) continue;
        SDL_GPUTextureTransferInfo src = {.transfer_buffer = up, .offset = vbytes + t->stage_off, .pixels_per_row = (Uint32)t->w, .rows_per_layer = (Uint32)t->h};
        SDL_GPUTextureRegion dst = {.texture = t->tex, .w = (Uint32)t->w, .h = (Uint32)t->h, .d = 1};
        SDL_UploadToGPUTexture(cp, &src, &dst, false);
    }
    SDL_EndGPUCopyPass(cp);
    for (int i = 0; i < nscene_texs; i++) {
        TexVer *t = scene_texs[i];
        if (!t->pending) continue;
        if (t->w > 1 || t->h > 1) SDL_GenerateMipmapsForGPUTexture(cb, t->tex);
        t->pending = 0;
    }

    // ---- render
    SDL_GPURenderPass *rp = NULL;
    int pend_color = 0, pend_depth = 0; float pend_rgba[4] = {0, 0, 0, 1};
    SDL_GPUColorTargetInfo ci = {.texture = msaa > 1 ? rt_msaa : rt_color,
        .store_op = msaa > 1 ? SDL_GPU_STOREOP_RESOLVE_AND_STORE : SDL_GPU_STOREOP_STORE, .resolve_texture = msaa > 1 ? rt_color : NULL};
    SDL_GPUDepthStencilTargetInfo di = {.texture = rt_depth, .clear_depth = 1.0f, .store_op = SDL_GPU_STOREOP_STORE,
        .stencil_load_op = SDL_GPU_LOADOP_DONT_CARE, .stencil_store_op = SDL_GPU_STOREOP_DONT_CARE};
    #define BEGIN_PASS() do { \
        ci.load_op = (pend_color || first_pass) ? SDL_GPU_LOADOP_CLEAR : SDL_GPU_LOADOP_LOAD; \
        ci.clear_color = (SDL_FColor){pend_rgba[0], pend_rgba[1], pend_rgba[2], pend_rgba[3]}; \
        di.load_op = (pend_depth || first_pass) ? SDL_GPU_LOADOP_CLEAR : SDL_GPU_LOADOP_LOAD; \
        rp = SDL_BeginGPURenderPass(cb, &ci, 1, &di); first_pass = 0; pend_color = pend_depth = 0; } while (0)
    for (int i = 0; i < ncmds; i++) {
        Cmd *c = &cmds[i];
        if (c->kind == CMD_CLEAR) {
            if (rp) { SDL_EndGPURenderPass(rp); rp = NULL; }
            if (c->clear_color) { pend_color = 1; memcpy(pend_rgba, c->rgba, sizeof pend_rgba); }
            if (c->clear_depth) pend_depth = 1;
            continue;
        }
        if (c->scissor[2] <= 0 || c->scissor[3] <= 0) continue;
        if (!rp) BEGIN_PASS();
        SDL_GPUGraphicsPipeline *p = get_pipeline(c->key);
        if (!p) continue;
        SDL_BindGPUGraphicsPipeline(rp, p);
        SDL_Rect sc = {c->scissor[0] * S, c->scissor[1] * S, c->scissor[2] * S, c->scissor[3] * S};
        SDL_SetGPUScissor(rp, &sc);
        SDL_GPUBufferBinding bb = {.buffer = vb, .offset = 0};
        SDL_BindGPUVertexBuffers(rp, 0, &bb, 1);
        SDL_GPUTextureSamplerBinding tb = {.texture = c->tex ? c->tex->tex : tex_white, .sampler = samplers[c->filter][!c->wrap]};
        SDL_BindGPUFragmentSamplers(rp, 0, &tb, 1);
        SDL_PushGPUFragmentUniformData(cb, 0, &c->u, sizeof c->u);
        float point_size = (float)S;   // one game pixel
        SDL_PushGPUVertexUniformData(cb, 0, &point_size, sizeof point_size);
        SDL_DrawGPUPrimitives(rp, c->count, 1, c->first, 0);
    }
    if (!rp && (pend_color || pend_depth)) BEGIN_PASS();
    if (rp) SDL_EndGPURenderPass(rp);
    #undef BEGIN_PASS

    // ---- the 1x copy of every rectangle the scene drew or cleared goes back into the guest surface
    Rect rects[MAX_READBACK_RECTS];
    int nrects = collect_rects(target, rects);
    Uint32 down_bytes = 0, rect_off[MAX_READBACK_RECTS];
    for (int i = 0; i < nrects; i++) { rect_off[i] = down_bytes; down_bytes += (Uint32)(rects[i].w * rects[i].h * 4); }
    SDL_GPUTransferBuffer *down = NULL;
    if (nrects) {
        down = pool_transfer(&down_tb, &down_cap, SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD, down_bytes);
        SDL_GPUTexture *small = rt_color;
        if (S > 1) {
            for (int i = 0; i < nrects; i++) {
                const Rect *r = &rects[i];
                SDL_GPUBlitInfo bl = {
                    .source = {.texture = rt_color, .x = (Uint32)(r->x * S), .y = (Uint32)(r->y * S), .w = (Uint32)(r->w * S), .h = (Uint32)(r->h * S)},
                    .destination = {.texture = rt_1x, .x = (Uint32)r->x, .y = (Uint32)r->y, .w = (Uint32)r->w, .h = (Uint32)r->h},
                    .load_op = SDL_GPU_LOADOP_LOAD, .filter = SDL_GPU_FILTER_LINEAR};
                SDL_BlitGPUTexture(cb, &bl);
            }
            small = rt_1x;
        }
        SDL_GPUCopyPass *dp = SDL_BeginGPUCopyPass(cb);
        for (int i = 0; i < nrects; i++) {
            const Rect *r = &rects[i];
            SDL_GPUTextureRegion src = {.texture = small, .x = (Uint32)r->x, .y = (Uint32)r->y, .w = (Uint32)r->w, .h = (Uint32)r->h, .d = 1};
            SDL_GPUTextureTransferInfo dst = {.transfer_buffer = down, .offset = rect_off[i], .pixels_per_row = (Uint32)r->w, .rows_per_layer = (Uint32)r->h};
            SDL_DownloadFromGPUTexture(dp, &src, &dst);
        }
        SDL_EndGPUCopyPass(dp);
    }
    SDL_GPUFence *fence = SDL_SubmitGPUCommandBufferAndAcquireFence(cb);
    gil_unlock();   // the other guest threads (input, sound) must not starve while the GPU works
    SDL_WaitForGPUFences(dev, true, &fence, 1);   // the pooled buffers are reused by the next scene, and the game reads the surface next
    gil_lock();
    SDL_ReleaseGPUFence(dev, fence);
    if (down) {
        const uint8_t *px = SDL_MapGPUTransferBuffer(dev, down, false);
        for (int i = 0; i < nrects; i++) write_back(target, px + rect_off[i], rects[i].x, rects[i].y, rects[i].w, rects[i].h);
        SDL_UnmapGPUTransferBuffer(dev, down);
        hires_valid = 1;
    }
    for (int i = 0; i < ngarbage; i++) { SDL_ReleaseGPUTexture(dev, garbage[i]->tex); free(garbage[i]); }
    ngarbage = 0;
    if (rt_verbose && ++dbg_scenes % 120 == 1) LOG("D3D scene %d: %u vertices, %d commands, %u bytes of textures", dbg_scenes, nverts, ncmds, stage_used);
    ncmds = 0; nverts = 0; stage_used = 0; nscene_texs = 0;
}

void d3d_gpu_end_scene(Surf *target) {
    if (!gpu_mu) return;
    gpu_lock();
    end_scene_locked(target);
    SDL_UnlockMutex(gpu_mu);
}

// ---------------------------------------------------------------------------------------------
// presentation

static SDL_GPUTexture *ui_tex, *mask_tex;
static SDL_GPUTransferBuffer *ui_tb, *mask_tb;
static int ui_w, ui_h;

bool gpu_window_init(SDL_Window *w) {
    if (!create_device()) return false;
    if (!SDL_ClaimWindowForGPUDevice(dev, w)) { WARN("D3D: SDL_ClaimWindowForGPUDevice: %s", SDL_GetError()); return false; }
    // The game presents after every primary-surface operation, so never block on the display: tearing beats input lag
    SDL_GPUPresentMode pm = SDL_WindowSupportsGPUPresentMode(dev, w, SDL_GPU_PRESENTMODE_MAILBOX) ? SDL_GPU_PRESENTMODE_MAILBOX : SDL_GPU_PRESENTMODE_IMMEDIATE;
    SDL_SetGPUSwapchainParameters(dev, w, SDL_GPU_SWAPCHAINCOMPOSITION_SDR, pm);
    qvs = make_shader(MSL_PRESENT, "q_vs", SDL_GPU_SHADERSTAGE_VERTEX, 0, 0);
    qfs = make_shader(MSL_PRESENT, "q_fs", SDL_GPU_SHADERSTAGE_FRAGMENT, 3, 1);
    if (!qvs || !qfs) return false;
    SDL_GPUColorTargetDescription ct = {.format = SDL_GetGPUSwapchainTextureFormat(dev, w)};
    SDL_GPUGraphicsPipelineCreateInfo pi = {.vertex_shader = qvs, .fragment_shader = qfs,
        .primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLESTRIP,
        .rasterizer_state = {.fill_mode = SDL_GPU_FILLMODE_FILL, .cull_mode = SDL_GPU_CULLMODE_NONE},
        .target_info = {.color_target_descriptions = &ct, .num_color_targets = 1}};
    present_pipe = SDL_CreateGPUGraphicsPipeline(dev, &pi);
    SDL_GPUSamplerCreateInfo sc = {.min_filter = SDL_GPU_FILTER_LINEAR, .mag_filter = SDL_GPU_FILTER_LINEAR,
        .address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE, .address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE,
        .address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE};
    s_lin = SDL_CreateGPUSampler(dev, &sc);
    sc.min_filter = sc.mag_filter = SDL_GPU_FILTER_NEAREST;
    s_near = SDL_CreateGPUSampler(dev, &sc);
    if (!present_pipe || !s_lin || !s_near) { SDL_ReleaseWindowFromGPUDevice(dev, w); return false; }
    gwin = w;
    return true;
}

// draws the letterboxed picture (2D frame, hi-res 3D where the mask says so) into a target of tw x th pixels
static void composite(SDL_GPUCommandBuffer *cb, SDL_GPUTexture *target, int tw, int th, int w, int h, int use_hi) {
    SDL_GPUColorTargetInfo ci = {.texture = target, .load_op = SDL_GPU_LOADOP_CLEAR, .store_op = SDL_GPU_STOREOP_STORE, .clear_color = {0, 0, 0, 1}};
    SDL_GPURenderPass *rp = SDL_BeginGPURenderPass(cb, &ci, 1, NULL);
    float sc = (float)tw / (float)w < (float)th / (float)h ? (float)tw / (float)w : (float)th / (float)h;
    float vw = (float)w * sc, vh = (float)h * sc;
    SDL_GPUViewport vp = {.x = ((float)tw - vw) / 2, .y = ((float)th - vh) / 2, .w = vw, .h = vh, .min_depth = 0, .max_depth = 1};
    SDL_SetGPUViewport(rp, &vp);
    SDL_BindGPUGraphicsPipeline(rp, present_pipe);
    SDL_GPUTextureSamplerBinding tb[3] = {{ui_tex, s_lin}, {use_hi ? rt_color : ui_tex, s_lin}, {use_hi ? mask_tex : ui_tex, s_near}};   // slot i = texture(i) + sampler(i)
    SDL_BindGPUFragmentSamplers(rp, 0, tb, 3);
    struct { float w, h; int32_t use_hi, sharp; } u = {(float)w, (float)h, use_hi, sc > 1.0f};
    SDL_PushGPUFragmentUniformData(cb, 0, &u, sizeof u);
    SDL_DrawGPUPrimitives(rp, 4, 1, 0, 0);
    SDL_EndGPURenderPass(rp);
}

// Shows a 2D frame. rgba: the frame converted to 32 bits; pix565/pitch: the same frame as the game wrote it, when 16 bit.
// Pixels of the frame that still equal the 1x copy of the last 3D scene are replaced by the scene's hi-res image.
void gpu_present(const uint32_t *rgba, int w, int h, const uint8_t *pix565, int pitch) {
    if (!dev || !gwin || !present_pipe) return;
    gpu_lock();
    if (!ui_tex || ui_w != w || ui_h != h) {
        if (ui_tex) { SDL_WaitForGPUIdle(dev); SDL_ReleaseGPUTexture(dev, ui_tex); SDL_ReleaseGPUTexture(dev, mask_tex); SDL_ReleaseGPUTransferBuffer(dev, ui_tb); SDL_ReleaseGPUTransferBuffer(dev, mask_tb); }
        // the frame is XRGB8888 (little endian): bytes B, G, R, X
        SDL_GPUTextureCreateInfo tc = {.type = SDL_GPU_TEXTURETYPE_2D, .format = SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM,
            .usage = SDL_GPU_TEXTUREUSAGE_SAMPLER, .width = (Uint32)w, .height = (Uint32)h, .layer_count_or_depth = 1, .num_levels = 1};
        ui_tex = SDL_CreateGPUTexture(dev, &tc);
        tc.format = SDL_GPU_TEXTUREFORMAT_R8_UNORM;
        mask_tex = SDL_CreateGPUTexture(dev, &tc);
        SDL_GPUTransferBufferCreateInfo ti = {.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD, .size = (Uint32)(w * h * 4)};
        ui_tb = SDL_CreateGPUTransferBuffer(dev, &ti);
        ti.size = (Uint32)(w * h);
        mask_tb = SDL_CreateGPUTransferBuffer(dev, &ti);
        ui_w = w; ui_h = h;
    }
    int use_hi = hires_valid && pix565 && rt_w == w && rt_h == h && snap;
    uint8_t *m = SDL_MapGPUTransferBuffer(dev, ui_tb, true);
    memcpy(m, rgba, (size_t)w * h * 4);
    SDL_UnmapGPUTransferBuffer(dev, ui_tb);
    if (use_hi) {
        uint8_t *mk = SDL_MapGPUTransferBuffer(dev, mask_tb, true);
        for (int y = 0; y < h; y++) {
            const uint16_t *f = (const uint16_t *)(const void *)(pix565 + (size_t)y * pitch);
            const uint32_t *sn = snap + (size_t)y * w;
            for (int x = 0; x < w; x++) mk[(size_t)y * w + x] = (sn[x] == (f[x] | 0x10000u)) ? 255 : 0;
        }
        SDL_UnmapGPUTransferBuffer(dev, mask_tb);
    }
    SDL_GPUCommandBuffer *cb = SDL_AcquireGPUCommandBuffer(dev);
    SDL_GPUCopyPass *cp = SDL_BeginGPUCopyPass(cb);
    SDL_GPUTextureTransferInfo src = {.transfer_buffer = ui_tb, .pixels_per_row = (Uint32)w, .rows_per_layer = (Uint32)h};
    SDL_GPUTextureRegion dst = {.texture = ui_tex, .w = (Uint32)w, .h = (Uint32)h, .d = 1};
    SDL_UploadToGPUTexture(cp, &src, &dst, true);
    if (use_hi) {
        src.transfer_buffer = mask_tb; dst.texture = mask_tex;
        SDL_UploadToGPUTexture(cp, &src, &dst, true);
    }
    SDL_EndGPUCopyPass(cp);

    SDL_GPUTexture *sct = NULL; Uint32 sw = 0, sh = 0;
    gil_unlock();   // waits for the display (vsync): let the other guest threads run meanwhile
    bool got = SDL_WaitAndAcquireGPUSwapchainTexture(cb, gwin, &sct, &sw, &sh);
    gil_lock();
    if (got && sct) {
        composite(cb, sct, (int)sw, (int)sh, w, h, use_hi);
        // POPTB_D3D_DUMP=<dir>: also save every POPTB_D3D_DUMP_EVERY-th (default 120) window picture as a BMP
        static const char *dump_dir; static int dump_every = -1, nframes;
        if (dump_every < 0) {
            dump_dir = getenv("POPTB_D3D_DUMP");
            const char *e = getenv("POPTB_D3D_DUMP_EVERY");
            dump_every = dump_dir ? (e ? atoi(e) : 120) : 0;
        }
        if (dump_every > 0 && ++nframes % dump_every == 0) {
            SDL_GPUTextureCreateInfo tc = {.type = SDL_GPU_TEXTURETYPE_2D, .format = SDL_GetGPUSwapchainTextureFormat(dev, gwin),
                .usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET, .width = sw, .height = sh, .layer_count_or_depth = 1, .num_levels = 1};
            SDL_GPUTexture *off = SDL_CreateGPUTexture(dev, &tc);
            composite(cb, off, (int)sw, (int)sh, w, h, use_hi);
            SDL_GPUTransferBufferCreateInfo di = {.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD, .size = sw * sh * 4};
            SDL_GPUTransferBuffer *down = SDL_CreateGPUTransferBuffer(dev, &di);
            SDL_GPUCopyPass *dp = SDL_BeginGPUCopyPass(cb);
            SDL_GPUTextureRegion r = {.texture = off, .w = sw, .h = sh, .d = 1};
            SDL_GPUTextureTransferInfo t = {.transfer_buffer = down, .pixels_per_row = sw, .rows_per_layer = sh};
            SDL_DownloadFromGPUTexture(dp, &r, &t);
            SDL_EndGPUCopyPass(dp);
            SDL_GPUFence *f = SDL_SubmitGPUCommandBufferAndAcquireFence(cb);
            gil_unlock();
            SDL_WaitForGPUFences(dev, true, &f, 1);
            gil_lock();
            SDL_ReleaseGPUFence(dev, f);
            void *px = SDL_MapGPUTransferBuffer(dev, down, false);
            SDL_Surface *surf = SDL_CreateSurfaceFrom((int)sw, (int)sh,
                tc.format == SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM ? SDL_PIXELFORMAT_BGRA32 : SDL_PIXELFORMAT_RGBA32, px, (int)sw * 4);
            char path[1100]; snprintf(path, sizeof path, "%s/window_%05d.bmp", dump_dir, nframes);
            SDL_SaveBMP(surf, path);
            SDL_DestroySurface(surf);
            SDL_UnmapGPUTransferBuffer(dev, down);
            SDL_ReleaseGPUTransferBuffer(dev, down);
            SDL_ReleaseGPUTexture(dev, off);
            SDL_UnlockMutex(gpu_mu);
            return;
        }
    }
    SDL_SubmitGPUCommandBuffer(cb);
    SDL_UnlockMutex(gpu_mu);
}
