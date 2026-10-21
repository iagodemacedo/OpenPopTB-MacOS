// Direct3D 5 TL-vertex scenes on SDL_GPU. A scene is recorded into CPU arrays (vertices, rebased indices and a
// list of draw commands, each with a cached pipeline), then rendered and read back in one go at gpu_end_scene.
#include "gpu.h"
#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
// POPTB_D3D_DUMP=<file>: writes the state and vertices of every draw of the first scene that starts at least
// POPTB_D3D_DUMP_MS after launch
static FILE *dumpf;
static int dumped;

static const char msl[] =
    "#include <metal_stdlib>\n"
    "using namespace metal;\n"
    "struct VIn { float4 pos [[attribute(0)]]; float4 dif [[attribute(1)]]; float4 spe [[attribute(2)]]; float2 uv [[attribute(3)]]; };\n"
    "struct VOut { float4 pos [[position]]; float psize [[point_size]];\n"
    "  float4 dif [[user(d)]]; float4 spe [[user(s)]]; float4 fdif [[user(fd), flat]]; float4 fspe [[user(fs), flat]]; float2 uv [[user(t)]]; };\n"
    "struct FIn { float4 pos [[position]]; float4 dif [[user(d)]]; float4 spe [[user(s)]]; float4 fdif [[user(fd), flat]]; float4 fspe [[user(fs), flat]]; float2 uv [[user(t)]]; };\n"
    "struct VU { float4 vp; };\n"
    "struct FU { uint mode, afunc, aref, flags; float scale; };\n"
    "vertex VOut vs_main(VIn v [[stage_in]], constant VU &u [[buffer(0)]]) {\n"
    "  float w = v.pos.w > 0 ? 1.0 / v.pos.w : 1.0;\n"
    "  float2 ndc = float2(v.pos.x / u.vp.x * 2 - 1, 1 - v.pos.y / u.vp.y * 2);\n"
    "  VOut o; o.pos = float4(ndc * w, v.pos.z * w, w); o.psize = u.vp.z;\n"
    "  o.dif = o.fdif = v.dif.zyxw; o.spe = o.fspe = v.spe.zyxw; o.uv = v.uv;\n"
    "  return o;\n"
    "}\n"
    "fragment float4 fs_main(FIn i [[stage_in]], constant FU &u [[buffer(0)]], texture2d<float> t [[texture(0)]], sampler s [[sampler(0)]]) {\n"
    "  float4 d = (u.flags & 2) ? i.fdif : i.dif, sp = (u.flags & 2) ? i.fspe : i.spe;\n"
    // A point-sampled draw takes the texel D3D picks for the whole game pixel: at its sample point, the pixel's
    // integer corner (UV is affine: the game's rhw is constant). The game puts texel edges exactly on those
    // points; 1/64 inward lands on the side exact interpolation gives.
    "  float2 g = i.pos.xy / u.scale, o = (floor(g) + 1.0 / 64 - g) * u.scale;\n"
    "  float2 uv = i.uv + ((u.flags & 4) ? dfdx(i.uv) * o.x + dfdy(i.uv) * o.y : 0);\n"
    "  float4 x = t.sample(s, uv), c;\n"
    "  switch (u.mode) {\n"
    "  case 1: c = x; break;\n"
    "  case 2: c = float4(x.rgb * d.rgb, x.a); break;\n"
    "  case 3: c = float4(x.rgb * d.rgb, d.a); break;\n"
    "  case 4: c = x * d; break;\n"
    "  case 5: c = float4(mix(d.rgb, x.rgb, x.a), d.a); break;\n"
    "  case 6: c = float4(saturate(x.rgb + d.rgb), d.a); break;\n"
    "  default: c = d; break;\n"
    "  }\n"
    "  if (u.flags & 1) c.rgb = saturate(c.rgb + sp.rgb);\n"
    "  uint a = uint(rint(saturate(c.a) * 255.0)), r = u.aref;\n"
    "  bool pass;\n"
    "  switch (u.afunc) {\n"
    "  case 1: pass = false; break;\n"
    "  case 2: pass = a < r; break;\n"
    "  case 3: pass = a == r; break;\n"
    "  case 4: pass = a <= r; break;\n"
    "  case 5: pass = a > r; break;\n"
    "  case 6: pass = a != r; break;\n"
    "  case 7: pass = a >= r; break;\n"
    "  default: pass = true; break;\n"
    "  }\n"
    "  if (!pass) discard_fragment();\n"
    "  return c;\n"
    "}\n";

// D3DCMPFUNC 1..8 and D3DBLEND 1..11 (12/13, BOTH(INV)SRCALPHA, are expanded before lookup)
static const SDL_GPUCompareOp cmp_op[9] = {
    SDL_GPU_COMPAREOP_ALWAYS, SDL_GPU_COMPAREOP_NEVER, SDL_GPU_COMPAREOP_LESS, SDL_GPU_COMPAREOP_EQUAL,
    SDL_GPU_COMPAREOP_LESS_OR_EQUAL, SDL_GPU_COMPAREOP_GREATER, SDL_GPU_COMPAREOP_NOT_EQUAL,
    SDL_GPU_COMPAREOP_GREATER_OR_EQUAL, SDL_GPU_COMPAREOP_ALWAYS,
};
static const SDL_GPUBlendFactor blend_factor[12] = {
    SDL_GPU_BLENDFACTOR_ONE, SDL_GPU_BLENDFACTOR_ZERO, SDL_GPU_BLENDFACTOR_ONE, SDL_GPU_BLENDFACTOR_SRC_COLOR,
    SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_COLOR, SDL_GPU_BLENDFACTOR_SRC_ALPHA, SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA,
    SDL_GPU_BLENDFACTOR_DST_ALPHA, SDL_GPU_BLENDFACTOR_ONE_MINUS_DST_ALPHA, SDL_GPU_BLENDFACTOR_DST_COLOR,
    SDL_GPU_BLENDFACTOR_ONE_MINUS_DST_COLOR, SDL_GPU_BLENDFACTOR_SRC_ALPHA_SATURATE,
};
// D3DCULL -> {cull mode, front face}; SDL_GPU winding is in y-down framebuffer space, like D3D's
static const struct { SDL_GPUCullMode mode; SDL_GPUFrontFace front; } cull_tab[4] = {
    {SDL_GPU_CULLMODE_NONE, SDL_GPU_FRONTFACE_CLOCKWISE}, {SDL_GPU_CULLMODE_NONE, SDL_GPU_FRONTFACE_CLOCKWISE},
    {SDL_GPU_CULLMODE_BACK, SDL_GPU_FRONTFACE_COUNTER_CLOCKWISE}, {SDL_GPU_CULLMODE_BACK, SDL_GPU_FRONTFACE_CLOCKWISE},
};
// fragment shader mode per D3DTBLEND, [texture has no alpha, texture has alpha]
enum { M_DIFFUSE, M_TEX, M_MOD_TEXA, M_MOD_DIFA, M_MODALPHA, M_DECALALPHA, M_ADD };
static const uint8_t tex_mode[9][2] = {
    {M_MOD_DIFA, M_MOD_TEXA}, {M_TEX, M_TEX}, {M_MOD_DIFA, M_MOD_TEXA}, {M_DECALALPHA, M_DECALALPHA},
    {M_MODALPHA, M_MODALPHA}, {M_TEX, M_TEX}, {M_MOD_DIFA, M_MOD_TEXA}, {M_TEX, M_TEX}, {M_ADD, M_ADD},
};

// Pipeline key: prim 1 | zenable 1 | zwrite 1 | zfunc 4 | blend 1 | src 4 | dst 4 | cull 2 | colour write 1
typedef uint32_t PipeKey;
typedef struct { PipeKey key; SDL_GPUGraphicsPipeline *pipe; } PipeEntry;
typedef struct { uint32_t mode, afunc, aref, flags; float scale; } FragU;   // flags: 1 specular, 2 flat, 4 point-sampled
typedef struct { PipeKey key; uint8_t samp; SDL_GPUTexture *tex; FragU fu; uint32_t first, count; } Cmd;
typedef struct { uint32_t key; SDL_GPUTexture *tex; int has_alpha; } Tex;

static SDL_GPUDevice *dev;
static SDL_GPUShader *vs, *fs;
static SDL_GPUTextureFormat depth_fmt;
static PipeEntry pipes[64];
static int npipes;
static SDL_GPUSampler *samplers[24];   // mag_linear | min_linear << 1 | clamp << 2, x3 mip modes
static SDL_GPUTexture *white;
static Tex *texs;
static size_t ntex, captex;
static SDL_GPUTexture **dead;          // replaced or freed mid-scene: queued draws still reference them
static size_t ndead, capdead;

static int sw, sh, scale;              // scene size in game pixels, internal scale
static SDL_GPUTexture *target, *depth, *bgtex, *small;
static int tw, th;                     // size the targets were created for (game pixels and scale)
static int tscale;
static uint32_t *bg;                   // starting image, RGBA8
static GpuVertex *verts;
static size_t nverts, capverts;
static uint32_t *idx;
static size_t nidx, capidx;
static Cmd *cmds;
static size_t ncmds, capcmds;
static int in_scene;
static SDL_GPUBuffer *vbuf, *ibuf;
static uint32_t vbuf_sz, ibuf_sz;
static SDL_GPUTransferBuffer *up, *down;
static uint32_t up_sz, down_sz;

static void die(const char *what) { fprintf(stderr, "gpu: %s: %s\n", what, SDL_GetError()); exit(1); }
static void *grow(void *p, size_t *cap, size_t need, size_t elem) {
    if (need <= *cap) return p;
    size_t n = *cap ? *cap : 256;
    while (n < need) n *= 2;
    p = realloc(p, n * elem);
    if (!p) die("out of memory");
    *cap = n;
    return p;
}

static SDL_GPUShader *shader(const char *entry, SDL_GPUShaderStage stage, int samplers) {
    SDL_GPUShaderCreateInfo ci = {
        .code_size = sizeof msl - 1, .code = (const Uint8 *)msl, .entrypoint = entry,
        .format = SDL_GPU_SHADERFORMAT_MSL, .stage = stage, .num_samplers = (Uint32)samplers, .num_uniform_buffers = 1,
    };
    SDL_GPUShader *s = SDL_CreateGPUShader(dev, &ci);
    if (!s) die(entry);
    return s;
}

static void init(void) {
    if (dev) return;
    dev = plat_gpu_device(NULL);
    if (!dev) { fprintf(stderr, "gpu: no SDL_GPU device (renderer is not the GPU renderer)\n"); exit(1); }
    if (!(SDL_GetGPUShaderFormats(dev) & SDL_GPU_SHADERFORMAT_MSL)) die("device does not take MSL");
    vs = shader("vs_main", SDL_GPU_SHADERSTAGE_VERTEX, 0);
    fs = shader("fs_main", SDL_GPU_SHADERSTAGE_FRAGMENT, 1);
    depth_fmt = SDL_GPUTextureSupportsFormat(dev, SDL_GPU_TEXTUREFORMAT_D32_FLOAT, SDL_GPU_TEXTURETYPE_2D,
                                             SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET)
        ? SDL_GPU_TEXTUREFORMAT_D32_FLOAT : SDL_GPU_TEXTUREFORMAT_D24_UNORM;
    static const uint32_t one = 0xffffffffu;
    const uint32_t *lv = &one;
    gpu_texture_upload(0, 1, 1, 1, &lv);
    white = texs[--ntex].tex;   // key 0 is reserved for "untextured"; keep it out of the table
}

static PipeKey pipe_key(const GpuState *st, int cmask) {
    int zen = st->zenable != 0, bl = st->blend != 0;
    int src = bl ? st->srcblend : 0, dst = bl ? st->dstblend : 0;
    if (src == 12) src = 5, dst = 6;
    else if (src == 13) src = 6, dst = 5;
    if (src > 11) src = 0;
    if (dst > 11) dst = 0;
    return (PipeKey)(st->prim & 1) | zen << 1 | (zen && st->zwrite) << 2 | (zen ? st->zfunc & 15 : 0) << 3
         | bl << 7 | src << 8 | dst << 12 | (st->cull & 3) << 16 | (cmask & 1) << 18;
}

static SDL_GPUGraphicsPipeline *pipeline(PipeKey k) {
    for (int i = 0; i < npipes; i++) if (pipes[i].key == k) return pipes[i].pipe;
    int zfunc = k >> 3 & 15, src = k >> 8 & 15, dst = k >> 12 & 15, cull = k >> 16 & 3;
    SDL_GPUVertexBufferDescription vb = { .slot = 0, .pitch = sizeof(GpuVertex), .input_rate = SDL_GPU_VERTEXINPUTRATE_VERTEX };
    SDL_GPUVertexAttribute attrs[4] = {
        {0, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4, 0},
        {1, 0, SDL_GPU_VERTEXELEMENTFORMAT_UBYTE4_NORM, 16},
        {2, 0, SDL_GPU_VERTEXELEMENTFORMAT_UBYTE4_NORM, 20},
        {3, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, 24},
    };
    SDL_GPUColorTargetDescription ct = {
        .format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM,
        .blend_state = {
            .src_color_blendfactor = blend_factor[src], .dst_color_blendfactor = blend_factor[dst],
            .color_blend_op = SDL_GPU_BLENDOP_ADD,
            .src_alpha_blendfactor = blend_factor[src], .dst_alpha_blendfactor = blend_factor[dst],
            .alpha_blend_op = SDL_GPU_BLENDOP_ADD,
            .enable_blend = (k >> 7 & 1) != 0,
            .enable_color_write_mask = !(k >> 18 & 1),
        },
    };
    SDL_GPUGraphicsPipelineCreateInfo ci = {
        .vertex_shader = vs, .fragment_shader = fs,
        .vertex_input_state = { .vertex_buffer_descriptions = &vb, .num_vertex_buffers = 1,
                                .vertex_attributes = attrs, .num_vertex_attributes = 4 },
        .primitive_type = (k & 1) ? SDL_GPU_PRIMITIVETYPE_TRIANGLELIST : SDL_GPU_PRIMITIVETYPE_POINTLIST,
        .rasterizer_state = { .fill_mode = SDL_GPU_FILLMODE_FILL, .cull_mode = cull_tab[cull].mode,
                              .front_face = cull_tab[cull].front, .enable_depth_clip = false },
        .depth_stencil_state = { .compare_op = cmp_op[zfunc < 9 ? zfunc : 0],
                                 .enable_depth_test = (k >> 1 & 1) != 0, .enable_depth_write = (k >> 2 & 1) != 0 },
        .target_info = { .color_target_descriptions = &ct, .num_color_targets = 1,
                         .depth_stencil_format = depth_fmt, .has_depth_stencil_target = true },
    };
    SDL_GPUGraphicsPipeline *p = SDL_CreateGPUGraphicsPipeline(dev, &ci);
    if (!p) die("pipeline");
    if (npipes == (int)(sizeof pipes / sizeof pipes[0])) die("pipeline cache full");
    pipes[npipes++] = (PipeEntry){k, p};
    return p;
}

static uint8_t sampler_id(const GpuState *st) {
    return (uint8_t)((st->mag_linear != 0) | (st->min_linear != 0) << 1 | (st->clamp != 0) << 2 | (st->mip > 2 ? 0 : st->mip) << 3);
}
static SDL_GPUSampler *sampler(uint8_t id) {
    if (samplers[id]) return samplers[id];
    SDL_GPUSamplerAddressMode am = (id & 4) ? SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE : SDL_GPU_SAMPLERADDRESSMODE_REPEAT;
    int mip = id >> 3;
    SDL_GPUSamplerCreateInfo ci = {
        .mag_filter = (id & 1) ? SDL_GPU_FILTER_LINEAR : SDL_GPU_FILTER_NEAREST,
        .min_filter = (id & 2) ? SDL_GPU_FILTER_LINEAR : SDL_GPU_FILTER_NEAREST,
        .mipmap_mode = mip == 2 ? SDL_GPU_SAMPLERMIPMAPMODE_LINEAR : SDL_GPU_SAMPLERMIPMAPMODE_NEAREST,
        .address_mode_u = am, .address_mode_v = am, .address_mode_w = am,
        .min_lod = 0, .max_lod = mip ? 1000.0f : 0.0f,
    };
    if (!(samplers[id] = SDL_CreateGPUSampler(dev, &ci))) die("sampler");
    return samplers[id];
}

static Tex *tex_find(uint32_t key) {
    for (size_t i = 0; i < ntex; i++) if (texs[i].key == key) return &texs[i];
    return NULL;
}
static void tex_retire(SDL_GPUTexture *t) {
    if (!in_scene) { SDL_ReleaseGPUTexture(dev, t); return; }
    dead = grow(dead, &capdead, ndead + 1, sizeof *dead);
    dead[ndead++] = t;
}

static void *map_upload(uint32_t size) {
    if (size > up_sz) {
        if (up) SDL_ReleaseGPUTransferBuffer(dev, up);
        up_sz = size < (1u << 20) ? 1u << 20 : size;
        SDL_GPUTransferBufferCreateInfo ci = { .usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD, .size = up_sz };
        if (!(up = SDL_CreateGPUTransferBuffer(dev, &ci))) die("upload buffer");
    }
    void *p = SDL_MapGPUTransferBuffer(dev, up, true);
    if (!p) die("map upload buffer");
    return p;
}

void gpu_texture_upload(uint32_t key, int w, int h, int levels, const uint32_t *const *rgba) {
    init();
    Tex *t = tex_find(key);
    if (t) tex_retire(t->tex);
    else {
        texs = grow(texs, &captex, ntex + 1, sizeof *texs);
        t = &texs[ntex++];
        t->key = key;
    }
    SDL_GPUTextureCreateInfo ci = {
        .type = SDL_GPU_TEXTURETYPE_2D, .format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, .usage = SDL_GPU_TEXTUREUSAGE_SAMPLER,
        .width = (Uint32)w, .height = (Uint32)h, .layer_count_or_depth = 1, .num_levels = (Uint32)levels, .sample_count = SDL_GPU_SAMPLECOUNT_1,
    };
    if (!(t->tex = SDL_CreateGPUTexture(dev, &ci))) die("texture");
    t->has_alpha = 0;
    for (size_t i = 0, n = (size_t)w * h; i < n && !t->has_alpha; i++) t->has_alpha = rgba[0][i] >> 24 != 255;
    uint32_t total = 0;
    for (int l = 0; l < levels; l++) total += (uint32_t)SDL_max(w >> l, 1) * SDL_max(h >> l, 1) * 4;
    uint8_t *p = map_upload(total);
    for (int l = 0, off = 0; l < levels; l++) {
        int n = SDL_max(w >> l, 1) * SDL_max(h >> l, 1) * 4;
        memcpy(p + off, rgba[l], (size_t)n);
        off += n;
    }
    SDL_UnmapGPUTransferBuffer(dev, up);
    SDL_GPUCommandBuffer *cb = SDL_AcquireGPUCommandBuffer(dev);
    SDL_GPUCopyPass *cp = SDL_BeginGPUCopyPass(cb);
    for (int l = 0, off = 0; l < levels; l++) {
        int lw = SDL_max(w >> l, 1), lh = SDL_max(h >> l, 1);
        SDL_GPUTextureTransferInfo src = { .transfer_buffer = up, .offset = (Uint32)off };
        SDL_GPUTextureRegion dst = { .texture = t->tex, .mip_level = (Uint32)l, .w = (Uint32)lw, .h = (Uint32)lh, .d = 1 };
        SDL_UploadToGPUTexture(cp, &src, &dst, false);
        off += lw * lh * 4;
    }
    SDL_EndGPUCopyPass(cp);
    if (!SDL_SubmitGPUCommandBuffer(cb)) die("submit texture upload");
}

void gpu_texture_free(uint32_t key) {
    Tex *t = tex_find(key);
    if (!t) return;
    tex_retire(t->tex);
    *t = texs[--ntex];
}

static SDL_GPUTexture *make_tex(int w, int h, SDL_GPUTextureFormat fmt, SDL_GPUTextureUsageFlags usage) {
    SDL_GPUTextureCreateInfo ci = {
        .type = SDL_GPU_TEXTURETYPE_2D, .format = fmt, .usage = usage, .width = (Uint32)w, .height = (Uint32)h,
        .layer_count_or_depth = 1, .num_levels = 1, .sample_count = SDL_GPU_SAMPLECOUNT_1,
    };
    SDL_GPUTexture *t = SDL_CreateGPUTexture(dev, &ci);
    if (!t) die("render target");
    return t;
}

static int pick_scale(int h) {
    const char *e = getenv("POPTB_D3D_SCALE");
    int s;
    if (e && *e) s = atoi(e);
    else { int ph = h; plat_gpu_device(&ph); s = (ph + h / 2) / h; }
    return s < 1 ? 1 : s > 4 ? 4 : s;
}

void gpu_begin_scene(int w, int h, const uint16_t *src, int pitch) {
    init();
    sw = w; sh = h; scale = pick_scale(h);
    if (w != tw || h != th || scale != tscale) {
        SDL_GPUTexture *old[4] = {target, depth, bgtex, small};
        for (int i = 0; i < 4; i++) if (old[i]) SDL_ReleaseGPUTexture(dev, old[i]);
        target = make_tex(w * scale, h * scale, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM,
                          SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER);
        depth = make_tex(w * scale, h * scale, depth_fmt, SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET);
        bgtex = make_tex(w, h, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, SDL_GPU_TEXTUREUSAGE_SAMPLER);
        small = make_tex(w, h, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, SDL_GPU_TEXTUREUSAGE_COLOR_TARGET);
        free(bg);
        if (!(bg = malloc((size_t)w * h * 4))) die("out of memory");
        tw = w; th = h; tscale = scale;
    }
    for (int y = 0; y < h; y++) {
        const uint16_t *s = (const uint16_t *)((const uint8_t *)src + (size_t)y * pitch);
        uint32_t *d = bg + (size_t)y * w;
        for (int x = 0; x < w; x++) {
            uint32_t c = s[x], r = c >> 11, g = c >> 5 & 63, b = c & 31;
            d[x] = (r << 3 | r >> 2) | (g << 2 | g >> 4) << 8 | (b << 3 | b >> 2) << 16 | 0xff000000u;
        }
    }
    nverts = nidx = ncmds = 0;
    in_scene = 1;
    if (dumpf) { fclose(dumpf); dumpf = NULL; }
    const char *dump = dumped ? NULL : getenv("POPTB_D3D_DUMP"), *ms = getenv("POPTB_D3D_DUMP_MS");
    if (dump && SDL_GetTicks() >= (Uint64)(ms ? atoi(ms) : 0)) { dumped = 1; dumpf = fopen(dump, "w"); }
}

static void queue(const GpuState *st, int cmask, const GpuVertex *v, int nv, const uint16_t *ix, int ni) {
    if (nv <= 0 || (ix && ni <= 0)) return;
    int n = ix ? ni : nv;
    uint32_t base = (uint32_t)nverts, first = (uint32_t)nidx;
    verts = grow(verts, &capverts, nverts + (size_t)nv, sizeof *verts);
    memcpy(verts + nverts, v, (size_t)nv * sizeof *v);
    if (st->prim == GPU_POINTS)   // a D3D point at integer x fills pixel x, whose centre is x + 0.5 here
        for (int i = 0; i < nv; i++) verts[nverts + i].x += 0.5f, verts[nverts + i].y += 0.5f;
    nverts += (size_t)nv;
    idx = grow(idx, &capidx, nidx + (size_t)n, sizeof *idx);
    for (int i = 0; i < n; i++) idx[nidx + i] = base + (ix ? ix[i] : (uint32_t)i);
    nidx += (size_t)n;

    Tex *t = st->tex ? tex_find(st->tex) : NULL;
    Cmd c = {
        .key = pipe_key(st, cmask), .samp = t ? sampler_id(st) : 0, .tex = t ? t->tex : white,
        .fu = { t ? tex_mode[st->mapblend < 9 ? st->mapblend : 2][t->has_alpha] : M_DIFFUSE,
                st->alphatest ? st->alphafunc : 8, st->alpharef,
                (st->specular != 0) | (st->flat != 0) << 1 | (t && !st->mag_linear) << 2, (float)scale },
        .first = first, .count = (uint32_t)n,
    };
    Cmd *last = ncmds ? &cmds[ncmds - 1] : NULL;
    if (last && last->key == c.key && last->samp == c.samp && last->tex == c.tex && !memcmp(&last->fu, &c.fu, sizeof c.fu)) {
        last->count += c.count;
        return;
    }
    cmds = grow(cmds, &capcmds, ncmds + 1, sizeof *cmds);
    cmds[ncmds++] = c;
}

void gpu_draw(const GpuState *st, const GpuVertex *v, int nv, const uint16_t *ix, int ni) {
    if (dumpf) {
        fprintf(dumpf, "D tex=%u prim=%d z=%d%d%d blend=%d %d %d at=%d %d %d mb=%d spec=%d flat=%d f=%d%d%d clamp=%d cull=%d nv=%d ni=%d\n",
            st->tex, st->prim, st->zenable, st->zwrite, st->zfunc, st->blend, st->srcblend, st->dstblend, st->alphatest, st->alphafunc,
            st->alpharef, st->mapblend, st->specular, st->flat, st->mag_linear, st->min_linear, st->mip, st->clamp, st->cull, nv, ni);
        for (int i = 0; i < nv; i++) fprintf(dumpf, "V %.4f %.4f %.6f %.4f %08x %08x %.6f %.6f\n", v[i].x, v[i].y, v[i].z, v[i].rhw, v[i].diffuse, v[i].specular, v[i].u, v[i].v);
        if (ix) { fprintf(dumpf, "I"); for (int i = 0; i < ni; i++) fprintf(dumpf, " %d", ix[i]); fprintf(dumpf, "\n"); }
    }
    queue(st, 1, v, nv, ix, ni);
}

void gpu_clear(int x, int y, int w, int h, uint32_t flags, uint32_t argb, float z) {
    if (!(flags & 3) || w <= 0 || h <= 0) return;
    float x0 = (float)x, y0 = (float)y, x1 = x0 + (float)w, y1 = y0 + (float)h;
    GpuVertex q[4] = {
        {x0, y0, z, 1, argb, 0, 0, 0}, {x1, y0, z, 1, argb, 0, 0, 0},
        {x0, y1, z, 1, argb, 0, 0, 0}, {x1, y1, z, 1, argb, 0, 0, 0},
    };
    static const uint16_t qi[6] = {0, 1, 2, 2, 1, 3};
    int zb = (flags & 2) != 0;
    GpuState st = { .prim = GPU_TRIANGLES, .zenable = (uint8_t)zb, .zwrite = (uint8_t)zb, .zfunc = 8, .cull = 1 };
    queue(&st, flags & 1, q, 4, qi, 6);
}

static void ensure_buffer(SDL_GPUBuffer **b, uint32_t *sz, uint32_t need, SDL_GPUBufferUsageFlags usage) {
    if (need <= *sz) return;
    if (*b) SDL_ReleaseGPUBuffer(dev, *b);
    *sz = 1u << 16;
    while (*sz < need) *sz *= 2;
    SDL_GPUBufferCreateInfo ci = { .usage = usage, .size = *sz };
    if (!(*b = SDL_CreateGPUBuffer(dev, &ci))) die("buffer");
}

void gpu_end_scene(uint16_t *out, int pitch) {
    uint32_t bg_sz = (uint32_t)sw * sh * 4, v_sz = (uint32_t)(nverts * sizeof *verts), i_sz = (uint32_t)(nidx * sizeof *idx);
    ensure_buffer(&vbuf, &vbuf_sz, v_sz, SDL_GPU_BUFFERUSAGE_VERTEX);
    ensure_buffer(&ibuf, &ibuf_sz, i_sz, SDL_GPU_BUFFERUSAGE_INDEX);
    uint8_t *p = map_upload(bg_sz + v_sz + i_sz);
    memcpy(p, bg, bg_sz);
    if (v_sz) memcpy(p + bg_sz, verts, v_sz);
    if (i_sz) memcpy(p + bg_sz + v_sz, idx, i_sz);
    SDL_UnmapGPUTransferBuffer(dev, up);

    SDL_GPUCommandBuffer *cb = SDL_AcquireGPUCommandBuffer(dev);
    SDL_GPUCopyPass *cp = SDL_BeginGPUCopyPass(cb);
    SDL_UploadToGPUTexture(cp, &(SDL_GPUTextureTransferInfo){ .transfer_buffer = up },
                           &(SDL_GPUTextureRegion){ .texture = bgtex, .w = (Uint32)sw, .h = (Uint32)sh, .d = 1 }, false);
    if (v_sz) SDL_UploadToGPUBuffer(cp, &(SDL_GPUTransferBufferLocation){ up, bg_sz },
                                    &(SDL_GPUBufferRegion){ vbuf, 0, v_sz }, false);
    if (i_sz) SDL_UploadToGPUBuffer(cp, &(SDL_GPUTransferBufferLocation){ up, bg_sz + v_sz },
                                    &(SDL_GPUBufferRegion){ ibuf, 0, i_sz }, false);
    SDL_EndGPUCopyPass(cp);

    SDL_BlitGPUTexture(cb, &(SDL_GPUBlitInfo){
        .source = { .texture = bgtex, .w = (Uint32)sw, .h = (Uint32)sh },
        .destination = { .texture = target, .w = (Uint32)(sw * scale), .h = (Uint32)(sh * scale) },
        .load_op = SDL_GPU_LOADOP_DONT_CARE, .filter = SDL_GPU_FILTER_NEAREST });

    if (ncmds) {
        SDL_GPUColorTargetInfo col = { .texture = target, .load_op = SDL_GPU_LOADOP_LOAD, .store_op = SDL_GPU_STOREOP_STORE };
        SDL_GPUDepthStencilTargetInfo dep = { .texture = depth, .clear_depth = 1, .load_op = SDL_GPU_LOADOP_CLEAR,
                                              .store_op = SDL_GPU_STOREOP_DONT_CARE,
                                              .stencil_load_op = SDL_GPU_LOADOP_DONT_CARE,
                                              .stencil_store_op = SDL_GPU_STOREOP_DONT_CARE };
        SDL_GPURenderPass *rp = SDL_BeginGPURenderPass(cb, &col, 1, &dep);
        SDL_BindGPUVertexBuffers(rp, 0, &(SDL_GPUBufferBinding){ vbuf, 0 }, 1);
        SDL_BindGPUIndexBuffer(rp, &(SDL_GPUBufferBinding){ ibuf, 0 }, SDL_GPU_INDEXELEMENTSIZE_32BIT);
        float vu[4] = { (float)sw, (float)sh, (float)scale, 0 };
        SDL_PushGPUVertexUniformData(cb, 0, vu, sizeof vu);
        SDL_GPUGraphicsPipeline *bound = NULL;
        const Cmd *prev = NULL;
        for (size_t i = 0; i < ncmds; i++) {
            const Cmd *c = &cmds[i];
            SDL_GPUGraphicsPipeline *pp = pipeline(c->key);
            if (pp != bound) SDL_BindGPUGraphicsPipeline(rp, bound = pp);
            if (!prev || prev->tex != c->tex || prev->samp != c->samp)
                SDL_BindGPUFragmentSamplers(rp, 0, &(SDL_GPUTextureSamplerBinding){ c->tex, sampler(c->samp) }, 1);
            if (!prev || memcmp(&prev->fu, &c->fu, sizeof c->fu)) SDL_PushGPUFragmentUniformData(cb, 0, &c->fu, sizeof c->fu);
            SDL_DrawGPUIndexedPrimitives(rp, c->count, 1, c->first, 0, 0);
            prev = c;
        }
        SDL_EndGPURenderPass(rp);
    }

    SDL_BlitGPUTexture(cb, &(SDL_GPUBlitInfo){
        .source = { .texture = target, .w = (Uint32)(sw * scale), .h = (Uint32)(sh * scale) },
        .destination = { .texture = small, .w = (Uint32)sw, .h = (Uint32)sh },
        .load_op = SDL_GPU_LOADOP_DONT_CARE, .filter = SDL_GPU_FILTER_LINEAR });
    if (bg_sz > down_sz) {
        if (down) SDL_ReleaseGPUTransferBuffer(dev, down);
        down_sz = bg_sz;
        SDL_GPUTransferBufferCreateInfo ci = { .usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD, .size = down_sz };
        if (!(down = SDL_CreateGPUTransferBuffer(dev, &ci))) die("download buffer");
    }
    cp = SDL_BeginGPUCopyPass(cb);
    SDL_DownloadFromGPUTexture(cp, &(SDL_GPUTextureRegion){ .texture = small, .w = (Uint32)sw, .h = (Uint32)sh, .d = 1 },
                               &(SDL_GPUTextureTransferInfo){ .transfer_buffer = down });
    SDL_EndGPUCopyPass(cp);
    SDL_GPUFence *f = SDL_SubmitGPUCommandBufferAndAcquireFence(cb);
    if (!f) die("submit scene");
    SDL_WaitForGPUFences(dev, true, &f, 1);
    SDL_ReleaseGPUFence(dev, f);

    const uint8_t *px = SDL_MapGPUTransferBuffer(dev, down, false);
    if (!px) die("map download buffer");
    for (int y = 0; y < sh; y++) {
        const uint8_t *s = px + (size_t)y * sw * 4;
        uint16_t *d = (uint16_t *)((uint8_t *)out + (size_t)y * pitch);
        for (int x = 0; x < sw; x++, s += 4) d[x] = (uint16_t)((s[0] >> 3) << 11 | (s[1] >> 2) << 5 | s[2] >> 3);
    }
    SDL_UnmapGPUTransferBuffer(dev, down);
    for (size_t i = 0; i < ndead; i++) SDL_ReleaseGPUTexture(dev, dead[i]);
    ndead = 0;
    in_scene = 0;
}

SDL_GPUTexture *gpu_image(int *w, int *h) {
    if (w) *w = tw * tscale;
    if (h) *h = th * tscale;
    return target;
}
