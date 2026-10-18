// DirectDraw on top of guest memory; the primary surface is presented via SDL.
#include "ddraw.h"

static uint32_t vt_dd1, vt_dd2, vt_surf, vt_pal, vt_clip;
static int mode_w = 640, mode_h = 480, mode_bpp = 8;
static Surf *primary;
uint32_t g_dd_obj;
uint32_t g_pal_gen;

static uint32_t default_pal[256];

// display palette: in real DirectDraw it is global hardware state in 8-bit mode
static uint32_t display_pal;

static void present_surf(Surf *s) {
    if (!s) return;
    if (s->bpp == 16) { primary = s; plat_present(PTR(s->mem), s->w, s->h, s->pitch, NULL); return; }
    if (s->bpp != 8) return;
    primary = s;
    if (s->pal) display_pal = s->pal;
    Pal *p = display_pal ? com_native(display_pal) : NULL;
    plat_present(PTR(s->mem), s->w, s->h, s->pitch, p ? p->rgb : default_pal);
}
static void present(void) { present_surf(primary); }
// the game draws its first frame: the point where the guest is up and the display mode is set
static void present_primary(CPU *R, Surf *s) { preset_apply_once(R); present_surf(s); }
void ddraw_present(void) { present(); }

static uint32_t new_surface(int w, int h, uint32_t caps) {
    Surf *s = calloc(1, sizeof *s);
    s->w = w; s->h = h; s->bpp = mode_bpp; s->caps = caps;
    s->pitch = (w * (s->bpp / 8) + 3) & ~3;
    s->mem = gmalloc((uint32_t)(s->pitch * h + 64));
    s->obj = com_new("IDirectDrawSurface", vt_surf, 0, s);
    return s->obj;
}

static void fill_pixfmt(uint32_t p, const Surf *s) {
    int bpp = s->bpp;
    memset(PTR(p), 0, 32);
    wr32(p, 32);
    if (s->pf_flags) {
        wr32(p + 4, s->pf_flags); wr32(p + 12, (uint32_t)bpp);
        wr32(p + 16, s->rmask); wr32(p + 20, s->gmask); wr32(p + 24, s->bmask); wr32(p + 28, s->amask);
    } else if (bpp == 8) { wr32(p + 4, 0x60); wr32(p + 12, 8); }   // DDPF_RGB | DDPF_PALETTEINDEXED8
    else { wr32(p + 4, 0x40); wr32(p + 12, 16); wr32(p + 16, 0xf800); wr32(p + 20, 0x07e0); wr32(p + 24, 0x001f); }
}
static void fill_desc(uint32_t d, Surf *s, uint32_t lp) {
    uint32_t size = rd32(d);
    if (size != 108 && size != 124) size = 108;
    memset(PTR(d), 0, size);
    wr32(d, size);
    wr32(d + 4, 0x100f | 0x800);   // CAPS|HEIGHT|WIDTH|PITCH|PIXELFORMAT|LPSURFACE
    wr32(d + 8, s->h); wr32(d + 12, s->w); wr32(d + 16, s->pitch);
    wr32(d + 36, lp);
    fill_pixfmt(d + 72, s);
    wr32(d + 104, s->caps);
}

// ---------------- IUnknown ----------------
void com_addref(CPU *R) { uint32_t o = ARG(0); wr32(o + 4, rd32(o + 4) + 1); R->eax = rd32(o + 4); R->esp += 8; }
void com_release(CPU *R) { uint32_t o = ARG(0); uint32_t c = rd32(o + 4); if (c) c--; wr32(o + 4, c); R->eax = c; R->esp += 8; }

// ---------------- IDirectDraw ----------------
COMFN(dd_QueryInterface, 2) {
    uint32_t iid = ARG(1), out = ARG(2);
    uint32_t d1 = rd32(iid);
    if (d1 == 0xB3A6F3E0 || d1 == 0x9c59509a || d1 == 0x6C14DB80) {   // IDirectDraw2 / 4 / IDirectDraw
        uint32_t o = com_new("IDirectDraw2", d1 == 0x6C14DB80 ? vt_dd1 : vt_dd2, 0, NULL);
        wr32(out, o);
        return 0;
    }
    if (d1 == IID_IDIRECT3D2) {
        wr32(out, d3d_create_interface());
        return 0;
    }
    WARN("IDirectDraw::QueryInterface unknown iid %08x", d1);
    wr32(out, 0);
    return 0x80004002;
}
COMFN(dd_SetCooperativeLevel, 2) { LOG("SetCooperativeLevel hwnd=%x flags=%x", ARG(1), ARG(2)); return 0; }
COMFN(dd_SetDisplayMode1, 3) { mode_w = ARG(1); mode_h = ARG(2); mode_bpp = ARG(3); LOG("SetDisplayMode %dx%dx%d", mode_w, mode_h, mode_bpp); return 0; }
COMFN(dd_SetDisplayMode2, 5) { mode_w = ARG(1); mode_h = ARG(2); mode_bpp = ARG(3); LOG("SetDisplayMode %dx%dx%d", mode_w, mode_h, mode_bpp); return 0; }
COMFN(dd_RestoreDisplayMode, 0) { return 0; }
COMFN(dd_WaitForVerticalBlank, 2) { return 0; }
COMFN(dd_FlipToGDISurface, 0) { return 0; }
COMFN(dd_Compact, 0) { return 0; }
COMFN(dd_GetMonitorFrequency, 1) { wr32(ARG(1), 60); return 0; }
COMFN(dd_GetVerticalBlankStatus, 1) { wr32(ARG(1), 1); return 0; }
COMFN(dd_GetScanLine, 1) { wr32(ARG(1), 0); return 0; }
COMFN(dd_GetAvailableVidMem, 3) { if (ARG(2)) wr32(ARG(2), 64 << 20); if (ARG(3)) wr32(ARG(3), 64 << 20); return 0; }
COMFN(dd_GetCaps, 2) {
    uint32_t c = ARG(1);
    for (int k = 0; k < 2; k++, c = ARG(2)) {
        if (!c) continue;
        uint32_t sz = rd32(c);
        if (sz < 8 || sz > 0x200) sz = 0x13c;
        memset(PTR(c), 0, sz); wr32(c, sz);
        wr32(c + 4, 0x00000040 | 0x00000400);   // DDCAPS_BLT | DDCAPS_PALETTE
        wr32(c + 0x3c, 64 << 20); wr32(c + 0x40, 64 << 20);
    }
    return 0;
}
COMFN(dd_GetDisplayMode, 1) {
    uint32_t d = ARG(1);
    Surf tmp = {.w = mode_w, .h = mode_h, .bpp = mode_bpp, .pitch = mode_w * mode_bpp / 8};
    fill_desc(d, &tmp, 0);
    wr32(d + 4, 0x100e);
    return 0;
}
COMFN(dd_EnumDisplayModes, 4) {
    uint32_t ctx = ARG(3), cb = ARG(4);
    static const int modes[][2] = {{640, 480}, {800, 600}, {1024, 768}, {1280, 1024}};
    uint32_t d = gmalloc(124);
    extern const char *g_exe_name;
    int nmodes = strstr(g_exe_name, "D3D") ? 8 : 4;   // the software builds only know 8-bit modes
    for (int i = 0; i < nmodes; i++) {
        int bpp = i < 4 ? 8 : 16;
        Surf tmp = {.w = modes[i & 3][0], .h = modes[i & 3][1], .bpp = bpp, .pitch = modes[i & 3][0] * bpp / 8};
        wr32(d, 108);
        fill_desc(d, &tmp, 0);
        wr32(d + 4, 0x100e | 0x20000);   // + REFRESHRATE
        wr32(d + 24, 60);
        uint32_t r = guest_call(R, cb, 2, d, ctx);
        if (!r) break;
    }
    gfree(d);
    return 0;
}
COMFN(dd_CreatePalette, 4) {
    uint32_t ent = ARG(2), out = ARG(3);
    Pal *p = calloc(1, sizeof *p);
    p->obj = com_new("IDirectDrawPalette", vt_pal, 0, p);
    if (ent) {
        memcpy(p->raw, PTR(ent), 1024);
        for (int i = 0; i < 256; i++) p->rgb[i] = (p->raw[i * 4] << 16) | (p->raw[i * 4 + 1] << 8) | p->raw[i * 4 + 2];
    }
    wr32(out, p->obj);
    return 0;
}
COMFN(dd_CreateClipper, 3) { wr32(ARG(2), com_new("IDirectDrawClipper", vt_clip, 0, NULL)); return 0; }
COMFN(dd_CreateSurface, 3) {
    uint32_t d = ARG(1), out = ARG(2);
    uint32_t fl = rd32(d + 4), caps = rd32(d + 104);
    int w = (fl & 4) ? (int)rd32(d + 12) : mode_w, h = (fl & 2) ? (int)rd32(d + 8) : mode_h;
    if (caps & 0x200) {   // PRIMARYSURFACE
        w = mode_w; h = mode_h;
        uint32_t o = new_surface(w, h, caps | 0x20 | 0x4000);
        Surf *s = com_native(o);
        s->primary = 1;
        primary = s;
        int nback = (fl & 0x20) ? (int)rd32(d + 20) : 0;
        if (nback > 0) {
            uint32_t b = new_surface(w, h, (caps & ~0x200u & ~0x20u) | 0x4 | 0x4000);
            s->back = b;
        }
        LOG("CreateSurface primary %dx%d back=%d", w, h, nback);
        wr32(out, o);
        return 0;
    }
    if ((fl & 0x1000) && (rd32(d + 72 + 4) & 0x4)) {   // DDPF_FOURCC: no compressed formats
        LOG("CreateSurface %dx%d fourcc %08x refused", w, h, rd32(d + 72 + 8));
        wr32(out, 0);
        return 0x88760091;   // DDERR_INVALIDPIXELFORMAT
    }
    uint32_t o = new_surface(w, h, caps ? caps : 0x40);
    Surf *s = com_native(o);
    if (fl & 0x1000) {   // explicit pixel format
        uint32_t bits = rd32(d + 72 + 12);
        s->pf_flags = rd32(d + 72 + 4);
        s->rmask = rd32(d + 72 + 16); s->gmask = rd32(d + 72 + 20); s->bmask = rd32(d + 72 + 24); s->amask = rd32(d + 72 + 28);
        if (bits && bits != (uint32_t)s->bpp) { s->bpp = (int)bits; s->pitch = (w * s->bpp / 8 + 3) & ~3; gfree(s->mem); s->mem = gmalloc((uint32_t)(s->pitch * h + 64)); }
    }
    LOG("CreateSurface %dx%d caps=%x bpp=%d pf=%x r=%x g=%x b=%x a=%x", w, h, caps, s->bpp, s->pf_flags, s->rmask, s->gmask, s->bmask, s->amask);
    wr32(out, o);
    return 0;
}
COMFN(dd_DuplicateSurface, 2) { return 0x80004001; }
COMFN(dd_GetFourCCCodes, 2) { if (ARG(1)) wr32(ARG(1), 0); return 0; }
COMFN(dd_GetGDISurface, 1) { wr32(ARG(1), primary ? primary->obj : 0); return primary ? 0 : 0x80004005; }
COMFN(dd_Initialize, 1) { return 0; }
COMFN(dd_EnumSurfaces, 4) { return 0; }
COMFN(dd_TestCooperativeLevel, 0) { return 0; }
COMFN(dd_RestoreAllSurfaces, 0) { return 0; }
COMFN(dd_GetDeviceIdentifier, 2) {
    uint32_t d = ARG(1);
    memset(PTR(d), 0, 1064);   // DDDEVICEIDENTIFIER (the DirectDraw 4 one, no WHQL level)
    strcpy(gstr(d), "display"); strcpy(gstr(d + 512), "OpenPopTB virtual display adapter");
    return 0;
}

// ---------------- IDirectDrawSurface ----------------
static Surf *S(uint32_t o) { return com_native(o); }

COMFN(s_QueryInterface, 2) {
    uint32_t d1 = rd32(ARG(1));
    if (d1 == IID_IDIRECT3DTEXTURE2) {
        wr32(ARG(2), d3d_texture_object(S(THIS)));
        return 0;
    }
    if (d1 == 0 || d1 == 0x6C14DB81 || d1 == 0x57805885 || d1 == 0xDA044E00 || d1 == 0x0B2B8630 || d1 == 0x06675A80) {   // IUnknown, IDirectDrawSurface 1/2/3/4/7
        wr32(ARG(2), THIS); wr32(THIS + 4, rd32(THIS + 4) + 1);
        return 0;
    }
    // e.g. IDirectDrawColorControl / IDirectDrawGammaControl: not supported
    LOG("IDirectDrawSurface::QueryInterface unsupported iid %08x", d1);
    wr32(ARG(2), 0);
    return 0x80004002;   // E_NOINTERFACE
}
COMFN(s_GetAttachedSurface, 2) {
    Surf *s = S(THIS);
    if (s->back) { wr32(ARG(2), s->back); return 0; }
    // primary without a declared back buffer: create one on demand
    if (s->primary) { s->back = new_surface(s->w, s->h, 0x4 | 0x4000); wr32(ARG(2), s->back); return 0; }
    wr32(ARG(2), 0);
    return 0x887600ff;   // DDERR_NOTFOUND
}
COMFN(s_AddAttachedSurface, 1) {
    Surf *s = S(THIS), *a = S(ARG(1));
    if (a && (a->caps & 0x20000)) s->zbuf = ARG(1);   // DDSCAPS_ZBUFFER
    else s->back = ARG(1);
    return 0;
}
COMFN(s_DeleteAttachedSurface, 2) { return 0; }
COMFN(s_GetCaps, 1) { wr32(ARG(1), S(THIS)->caps); return 0; }
COMFN(s_GetSurfaceDesc, 1) { Surf *s = S(THIS); fill_desc(ARG(1), s, s->mem); return 0; }
COMFN(s_GetPixelFormat, 1) { fill_pixfmt(ARG(1), S(THIS)); return 0; }
COMFN(s_Lock, 4) {
    Surf *s = S(THIS);
    if (rt_verbose > 1) LOG("Lock %x %dx%d primary=%d", THIS, s->w, s->h, s->primary);
    uint32_t rc = ARG(1), d = ARG(2);
    uint32_t lp = s->mem;
    if (rc) lp += rd32(rc + 4) * s->pitch + rd32(rc) * (s->bpp / 8);
    fill_desc(d, s, lp);
    s->locked = 1; s->gen++;
    return 0;
}
COMFN(s_Unlock, 1) {
    Surf *s = S(THIS);
    s->locked = 0; s->gen++;
    if (s->primary) present_primary(R, s);
    return 0;
}
COMFN(s_Flip, 2) {
    Surf *s = S(THIS);
    if (rt_verbose > 1) {
        Surf *b = s->back ? S(s->back) : NULL;
        int nz = 0; if (b) for (int i = 0; i < b->pitch * b->h; i++) nz += MEM[b->mem + i] != 0;
        LOG("[%u] Flip %x primary=%d back=%x nonzero=%d pal=%x bpp=%d w=%d", now_ms() % 100000, THIS, s->primary, s->back, nz, s->pal, s->bpp, s->w);
    }
    if (s->back) {
        Surf *b = S(s->back);
        uint32_t t = s->mem; s->mem = b->mem; b->mem = t;
    }
    if (s->primary) present_primary(R, s);
    return 0;
}
COMFN(s_IsLost, 0) { return 0; }
COMFN(s_Restore, 0) { return 0; }
COMFN(s_SetPalette, 1) {
    Surf *s = S(THIS);
    s->pal = ARG(1); g_pal_gen++;
    if (s->back) S(s->back)->pal = ARG(1);
    if (s->primary) display_pal = ARG(1);
    return 0;
}
COMFN(s_GetPalette, 1) { Surf *s = S(THIS); wr32(ARG(1), s->pal); return s->pal ? 0 : 0x88760083; }
COMFN(s_SetClipper, 1) { return 0; }
COMFN(s_GetClipper, 1) { wr32(ARG(1), 0); return 0x88760094; }
COMFN(s_SetColorKey, 2) {
    Surf *s = S(THIS);
    if (ARG(2)) { s->ck_lo = rd32(ARG(2)); s->ck_hi = rd32(ARG(2) + 4); s->has_ck = 1; }
    else s->has_ck = 0;
    s->gen++;
    return 0;
}
COMFN(s_GetColorKey, 2) { Surf *s = S(THIS); wr32(ARG(2), s->ck_lo); wr32(ARG(2) + 4, s->ck_hi); return s->has_ck ? 0 : 0x88760078; }
COMFN(s_GetBltStatus, 1) { return 0; }
COMFN(s_GetFlipStatus, 1) { return 0; }
COMFN(s_GetDC, 1) { wr32(ARG(1), 0xdc00 + (THIS & 0xff)); return 0; }
COMFN(s_ReleaseDC, 1) { Surf *s = S(THIS); s->gen++; if (s->primary) present_primary(R, s); return 0; }
COMFN(s_PageLock, 1) { return 0; }
COMFN(s_PageUnlock, 1) { return 0; }
COMFN(s_GetDDInterface, 1) { wr32(ARG(1), g_dd_obj); return 0; }

static void blit16(Surf *d, int dx, int dy, int dw, int dh, Surf *s, int sx, int sy, int sw, int sh, int key, uint32_t ck) {
    for (int y = 0; y < dh; y++) {
        int yy = dy + y; if (yy < 0 || yy >= d->h) continue;
        int ys = sy + (int)((int64_t)y * sh / dh); if (ys < 0 || ys >= s->h) continue;
        for (int x = 0; x < dw; x++) {
            int xx = dx + x; if (xx < 0 || xx >= d->w) continue;
            int xs = sx + (int)((int64_t)x * sw / dw); if (xs < 0 || xs >= s->w) continue;
            uint32_t c = rd16(s->mem + ys * s->pitch + xs * 2);
            if (key && c == (ck & 0xffff)) continue;
            wr16(d->mem + yy * d->pitch + xx * 2, c);
        }
    }
}
static void blit(Surf *d, int dx, int dy, int dw, int dh, Surf *s, int sx, int sy, int sw, int sh, int key, uint32_t ck) {
    if (d->bpp == 16 && s->bpp == 16) { blit16(d, dx, dy, dw, dh, s, sx, sy, sw, sh, key, ck); return; }
    if (d->bpp != 8 || s->bpp != 8) return;
    for (int y = 0; y < dh; y++) {
        int yy = dy + y; if (yy < 0 || yy >= d->h) continue;
        int ys = sy + (int)((int64_t)y * sh / dh); if (ys < 0 || ys >= s->h) continue;
        uint8_t *dp = PTR(d->mem + yy * d->pitch);
        uint8_t *sp = PTR(s->mem + ys * s->pitch);
        if (!key && dw == sw && dx >= 0 && dx + dw <= d->w && sx >= 0 && sx + sw <= s->w) { memmove(dp + dx, sp + sx, dw); continue; }
        for (int x = 0; x < dw; x++) {
            int xx = dx + x; if (xx < 0 || xx >= d->w) continue;
            int xs = sx + (int)((int64_t)x * sw / dw); if (xs < 0 || xs >= s->w) continue;
            uint8_t c = sp[xs];
            if (key && c == (uint8_t)ck) continue;
            dp[xx] = c;
        }
    }
}
static void rect_of(uint32_t rc, Surf *s, int *x, int *y, int *w, int *h) {
    if (!rc) { *x = 0; *y = 0; *w = s->w; *h = s->h; return; }
    *x = (int)rd32(rc); *y = (int)rd32(rc + 4); *w = (int)rd32(rc + 8) - *x; *h = (int)rd32(rc + 12) - *y;
}
COMFN(s_Blt, 5) {
    Surf *d = S(THIS);
    if (rt_verbose > 1) LOG("Blt %x <- %x flags=%x primary=%d", THIS, ARG(2), ARG(4), d->primary);
    uint32_t drc = ARG(1), so = ARG(2), src = ARG(3), fl = ARG(4), fx = ARG(5);
    int dx, dy, dw, dh; rect_of(drc, d, &dx, &dy, &dw, &dh);
    d->gen++;
    if ((fl & 0x400) && d->bpp == 16) {
        uint32_t c = rd32(fx + 80) & 0xffff;
        for (int y = dy; y < dy + dh; y++) if (y >= 0 && y < d->h)
            for (int x = dx; x < dx + dw; x++) if (x >= 0 && x < d->w) wr16(d->mem + y * d->pitch + x * 2, c);
    } else if (fl & 0x400) {   // COLORFILL
        uint8_t c = (uint8_t)rd32(fx + 80);
        for (int y = dy; y < dy + dh; y++) if (y >= 0 && y < d->h) {
            int x0 = dx < 0 ? 0 : dx, x1 = dx + dw > d->w ? d->w : dx + dw;
            if (x1 > x0) memset(PTR(d->mem + y * d->pitch + x0), c, x1 - x0);
        }
    } else if (so) {
        Surf *s = S(so);
        int sx, sy, sw, sh; rect_of(src, s, &sx, &sy, &sw, &sh);
        int key = 0; uint32_t ck = 0;
        if (fl & 0x8000) { key = s->has_ck; ck = s->ck_lo; }
        if (fl & 0x10000) { key = 1; ck = rd32(fx + 92); }
        blit(d, dx, dy, dw, dh, s, sx, sy, sw, sh, key, ck);
    }
    if (d->primary) present_primary(R, d);
    return 0;
}
COMFN(s_BltFast, 5) {
    Surf *d = S(THIS);
    if (rt_verbose > 1) LOG("BltFast %x <- %x primary=%d", THIS, ARG(3), d->primary);
    int x = (int)ARG(1), y = (int)ARG(2);
    d->gen++;
    Surf *s = S(ARG(3));
    int sx, sy, sw, sh; rect_of(ARG(4), s, &sx, &sy, &sw, &sh);
    int key = (ARG(5) & 1) && s->has_ck;
    blit(d, x, y, sw, sh, s, sx, sy, sw, sh, key, s->ck_lo);
    if (d->primary) present_primary(R, d);
    return 0;
}
COMFN(s_EnumAttachedSurfaces, 2) {
    Surf *s = S(THIS);
    if (s->back) {
        uint32_t d = gmalloc(124); wr32(d, 108);
        fill_desc(d, S(s->back), 0);
        guest_call(R, ARG(2), 3, s->back, d, ARG(1));
        gfree(d);
    }
    return 0;
}
COMFN(s_Initialize, 2) { return 0x88760005; }
COMFN(s_SetSurfaceDesc, 2) {
    Surf *s = S(THIS); uint32_t d = ARG(1);
    if (rd32(d + 4) & 0x800) s->mem = rd32(d + 36);
    if (rd32(d + 4) & 4) s->w = (int)rd32(d + 12);
    if (rd32(d + 4) & 2) s->h = (int)rd32(d + 8);
    if (rd32(d + 4) & 8) s->pitch = (int)rd32(d + 16);
    return 0;
}

// ---------------- IDirectDrawPalette ----------------
// QueryInterface of objects that only know IUnknown and their own interface
static uint32_t qi_own(CPU *R, uint32_t own_iid) {
    uint32_t d1 = rd32(ARG(1));
    if (d1 == 0 || d1 == own_iid) { wr32(ARG(2), THIS); wr32(THIS + 4, rd32(THIS + 4) + 1); return 0; }
    wr32(ARG(2), 0);
    return 0x80004002;   // E_NOINTERFACE
}
COMFN(p_QueryInterface, 2) { return qi_own(R, 0x6C14DB84); }
COMFN(c_QueryInterface, 2) { return qi_own(R, 0x6C14DB85); }
COMFN(p_GetCaps, 1) { wr32(ARG(1), 0x4 | 0x40); return 0; }
COMFN(p_GetEntries, 4) {
    Pal *p = com_native(THIS);
    uint32_t st = ARG(2), n = ARG(3), out = ARG(4);
    if (st + n > 256) n = 256 - st;
    memcpy(PTR(out), p->raw + st * 4, n * 4);
    return 0;
}
COMFN(p_SetEntries, 4) {
    Pal *p = com_native(THIS);
    if (rt_verbose > 1) LOG("Palette SetEntries %x start=%u n=%u", THIS, ARG(2), ARG(3));
    uint32_t st = ARG(2), n = ARG(3), in = ARG(4);
    if (st + n > 256) n = 256 - st;
    memcpy(p->raw + st * 4, PTR(in), n * 4);
    g_pal_gen++;
    for (uint32_t i = st; i < st + n; i++) p->rgb[i] = (p->raw[i * 4] << 16) | (p->raw[i * 4 + 1] << 8) | p->raw[i * 4 + 2];
    if (display_pal == THIS) present();
    return 0;
}
COMFN(p_Initialize, 3) { return 0; }

// ---------------- IDirectDrawClipper ----------------
COMFN(c_SetHWnd, 2) { return 0; }
COMFN(c_GetHWnd, 1) { wr32(ARG(1), g_hwnd); return 0; }
COMFN(c_GetClipList, 3) { return 0x80004001; }
COMFN(c_SetClipList, 2) { return 0; }
COMFN(c_IsClipListChanged, 1) { wr32(ARG(1), 0); return 0; }
COMFN(c_Initialize, 2) { return 0; }

static void build_vtables(void) {
    static const char *ddm[] = {"QueryInterface", "AddRef", "Release", "Compact", "CreateClipper", "CreatePalette",
        "CreateSurface", "DuplicateSurface", "EnumDisplayModes", "EnumSurfaces", "FlipToGDISurface", "GetCaps",
        "GetDisplayMode", "GetFourCCCodes", "GetGDISurface", "GetMonitorFrequency", "GetScanLine",
        "GetVerticalBlankStatus", "Initialize", "RestoreDisplayMode", "SetCooperativeLevel", "SetDisplayMode",
        "WaitForVerticalBlank", "GetAvailableVidMem", "GetSurfaceFromDC", "RestoreAllSurfaces",
        "TestCooperativeLevel", "GetDeviceIdentifier"};
    native_fn ddf[] = {dd_QueryInterface, com_addref, com_release, dd_Compact, dd_CreateClipper, dd_CreatePalette,
        dd_CreateSurface, dd_DuplicateSurface, dd_EnumDisplayModes, dd_EnumSurfaces, dd_FlipToGDISurface, dd_GetCaps,
        dd_GetDisplayMode, dd_GetFourCCCodes, dd_GetGDISurface, dd_GetMonitorFrequency, dd_GetScanLine,
        dd_GetVerticalBlankStatus, dd_Initialize, dd_RestoreDisplayMode, dd_SetCooperativeLevel, dd_SetDisplayMode1,
        dd_WaitForVerticalBlank, dd_GetAvailableVidMem, NULL, dd_RestoreAllSurfaces, dd_TestCooperativeLevel, dd_GetDeviceIdentifier};
    vt_dd1 = com_vtbl("IDirectDraw", ddm, 23, ddf);
    ddf[21] = dd_SetDisplayMode2;
    vt_dd2 = com_vtbl("IDirectDraw2", ddm, 28, ddf);
    static const char *sm[] = {"QueryInterface", "AddRef", "Release", "AddAttachedSurface", "AddOverlayDirtyRect",
        "Blt", "BltBatch", "BltFast", "DeleteAttachedSurface", "EnumAttachedSurfaces", "EnumOverlayZOrders", "Flip",
        "GetAttachedSurface", "GetBltStatus", "GetCaps", "GetClipper", "GetColorKey", "GetDC", "GetFlipStatus",
        "GetOverlayPosition", "GetPalette", "GetPixelFormat", "GetSurfaceDesc", "Initialize", "IsLost", "Lock",
        "ReleaseDC", "Restore", "SetClipper", "SetColorKey", "SetOverlayPosition", "SetPalette", "Unlock",
        "UpdateOverlay", "UpdateOverlayDisplay", "UpdateOverlayZOrder", "GetDDInterface", "PageLock", "PageUnlock",
        "SetSurfaceDesc"};
    native_fn sf[] = {s_QueryInterface, com_addref, com_release, s_AddAttachedSurface, NULL,
        s_Blt, NULL, s_BltFast, s_DeleteAttachedSurface, s_EnumAttachedSurfaces, NULL, s_Flip,
        s_GetAttachedSurface, s_GetBltStatus, s_GetCaps, s_GetClipper, s_GetColorKey, s_GetDC, s_GetFlipStatus,
        NULL, s_GetPalette, s_GetPixelFormat, s_GetSurfaceDesc, s_Initialize, s_IsLost, s_Lock,
        s_ReleaseDC, s_Restore, s_SetClipper, s_SetColorKey, NULL, s_SetPalette, s_Unlock,
        NULL, NULL, NULL, s_GetDDInterface, s_PageLock, s_PageUnlock, s_SetSurfaceDesc};
    vt_surf = com_vtbl("IDirectDrawSurface", sm, 40, sf);
    static const char *pm[] = {"QueryInterface", "AddRef", "Release", "GetCaps", "GetEntries", "Initialize", "SetEntries"};
    native_fn pf[] = {p_QueryInterface, com_addref, com_release, p_GetCaps, p_GetEntries, p_Initialize, p_SetEntries};
    vt_pal = com_vtbl("IDirectDrawPalette", pm, 7, pf);
    static const char *cm[] = {"QueryInterface", "AddRef", "Release", "GetClipList", "GetHWnd", "Initialize",
        "IsClipListChanged", "SetClipList", "SetHWnd"};
    native_fn cf[] = {c_QueryInterface, com_addref, com_release, c_GetClipList, c_GetHWnd, c_Initialize,
        c_IsClipListChanged, c_SetClipList, c_SetHWnd};
    vt_clip = com_vtbl("IDirectDrawClipper", cm, 9, cf);
    for (int i = 0; i < 256; i++) default_pal[i] = (uint32_t)(i * 0x010101);
}

SHIM(DirectDrawCreate, 3) {
    if (!vt_dd1) build_vtables();
    g_dd_obj = com_new("IDirectDraw", vt_dd1, 0, NULL);
    wr32(ARG(1), g_dd_obj);
    LOG("DirectDrawCreate -> %x", g_dd_obj);
    return 0;
}
SHIM(DirectDrawEnumerateA, 2) {
    uint32_t name = gstrdup("Primary Display Driver"), drv = gstrdup("display");
    guest_call(R, ARG(0), 4, 0, name, drv, ARG(1));
    return 0;
}
