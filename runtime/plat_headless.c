// Headless platform: no window. Replaces plat.c (POPTB_HEADLESS, and the WebAssembly/Node build).
// With POPTB_WEB (browser build) each frame is also handed to the page, which draws it on a canvas
// (web/main.js).
// Frames are converted to XRGB like plat.c and written as BMP screenshots (--shot*, same file names);
// input comes only from --script, injected straight into DirectInput and the Win32 message queue with
// the same timing as plat.c. Audio goes through SDL's dummy driver unless SDL_AUDIO_DRIVER says
// otherwise, so DirectSound's play cursor (which paces the FMVs) still advances.
#include "rt.h"
#include "input.h"
#include "gpu.h"
#include <SDL3/SDL.h>
#ifdef POPTB_WEB
#include <emscripten/emscripten.h>
#include <emscripten/threading.h>
#endif

int g_mouse_x = 320, g_mouse_y = 240;
uint32_t g_start_ms;
static int tex_w = 640, tex_h = 480;
static uint32_t *rgba;

static void vmove(int nx, int ny) {
    if (nx < 0) nx = 0; if (ny < 0) ny = 0;
    if (nx > tex_w - 1) nx = tex_w - 1; if (ny > tex_h - 1) ny = tex_h - 1;
    int dx = nx - g_mouse_x, dy = ny - g_mouse_y;
    g_mouse_x = nx; g_mouse_y = ny;
    if (dx) { di_mouse_dx += dx; di_push_mouse(0, (uint32_t)dx); }
    if (dy) { di_mouse_dy += dy; di_push_mouse(4, (uint32_t)dy); }
}

// key names accepted by --script (same names as SDL_GetScancodeFromName for these keys)
typedef struct { const char *name; uint8_t dik, vk; } KeyName;
static const KeyName keys[] = {
    {"Escape", 0x01, 0x1B}, {"Return", 0x1C, 0x0D}, {"Space", 0x39, 0x20}, {"Tab", 0x0F, 0x09},
    {"Backspace", 0x0E, 0x08}, {"Up", 0xC8, 0x26}, {"Down", 0xD0, 0x28}, {"Left", 0xCB, 0x25},
    {"Right", 0xCD, 0x27}, {"Home", 0xC7, 0x24}, {"End", 0xCF, 0x23}, {"PageUp", 0xC9, 0x21},
    {"PageDown", 0xD1, 0x22}, {"Insert", 0xD2, 0x2D}, {"Delete", 0xD3, 0x2E},
    {"F1", 0x3B, 0x70}, {"F2", 0x3C, 0x71}, {"F3", 0x3D, 0x72}, {"F4", 0x3E, 0x73}, {"F5", 0x3F, 0x74},
    {"F6", 0x40, 0x75}, {"F7", 0x41, 0x76}, {"F8", 0x42, 0x77}, {"F9", 0x43, 0x78}, {"F10", 0x44, 0x79},
    {"F11", 0x57, 0x7A}, {"F12", 0x58, 0x7B},
};
static const char letter_dik[26] = {0x1E, 0x30, 0x2E, 0x20, 0x12, 0x21, 0x22, 0x23, 0x17, 0x24, 0x25, 0x26, 0x32,
                                    0x31, 0x18, 0x19, 0x10, 0x13, 0x1F, 0x14, 0x16, 0x2F, 0x11, 0x2D, 0x15, 0x2C};
static int key_lookup(const char *n, uint8_t *dik, uint8_t *vk) {
    for (size_t i = 0; i < sizeof keys / sizeof keys[0]; i++)
        if (!strcasecmp(keys[i].name, n)) { *dik = keys[i].dik; *vk = keys[i].vk; return 1; }
    if (n[0] && !n[1]) {
        char c = n[0];
        if (c >= 'a' && c <= 'z') c -= 32;
        if (c >= 'A' && c <= 'Z') { *dik = (uint8_t)letter_dik[c - 'A']; *vk = (uint8_t)c; return 1; }
        if (c >= '1' && c <= '9') { *dik = (uint8_t)(0x02 + c - '1'); *vk = (uint8_t)c; return 1; }
        if (c == '0') { *dik = 0x0B; *vk = '0'; return 1; }
    }
    return 0;
}
static void key_event(uint8_t dik, uint8_t vk, int down) {
    di_push_key(dik, down);
    uint32_t lp = 1 | ((uint32_t)(dik & 0x7f) << 16) | ((dik & 0x80) ? 1u << 24 : 0) | (down ? 0 : 0xC0000000u);
    msgq_post(g_hwnd, down ? 0x100 : 0x101, vk, lp);
    if (down && (vk == 0x0D || vk == 0x08 || vk == 0x1B || vk == 0x09)) msgq_post(g_hwnd, 0x102, vk, lp);
}
static void mouse_button(int b, int down) {
    static const uint32_t msgs[3][2] = {{0x202, 0x201}, {0x205, 0x204}, {0x208, 0x207}};
    di_mouse_btn[b] = down ? 0x80 : 0;
    di_push_mouse(12 + b, down ? 0x80 : 0);
    uint32_t mk = (di_mouse_btn[0] ? 1 : 0) | (di_mouse_btn[1] ? 2 : 0) | (di_mouse_btn[2] ? 0x10 : 0);
    msgq_post(g_hwnd, msgs[b][down], mk, (uint32_t)(g_mouse_x & 0xffff) | ((uint32_t)g_mouse_y << 16));
}

// input script, same syntax as plat.c: "ms:key:NAME;ms:click:x,y[,hold];ms:rclick:x,y;ms:move:x,y;ms:quit"
enum { EV_KEY = 1, EV_LDOWN, EV_RDOWN, EV_MOVE, EV_QUIT, EV_LUP, EV_RUP };
typedef struct { uint32_t t; int kind; int a, b; } ScriptEv;
static ScriptEv script[256];
static int nscript, iscript;
static int ev_cmp(const void *a, const void *b) { const ScriptEv *x = a, *y = b; return x->t < y->t ? -1 : x->t > y->t; }
void plat_script(const char *spec) {
    char buf[4096]; snprintf(buf, sizeof buf, "%s", spec);
    char *save = NULL;
    for (char *it = strtok_r(buf, ";", &save); it && nscript < 250; it = strtok_r(NULL, ";", &save)) {
        char kind[32] = "", arg[64] = "";
        unsigned t = 0;
        if (sscanf(it, "%u:%31[^:]:%63s", &t, kind, arg) < 2) continue;
        ScriptEv *e = &script[nscript++];
        e->t = t;
        if (!strcmp(kind, "key")) {
            uint8_t dik, vk;
            if (!key_lookup(arg, &dik, &vk)) { WARN("script: unknown key %s", arg); nscript--; continue; }
            e->kind = EV_KEY; e->a = dik; e->b = vk;
        } else if (!strcmp(kind, "click") || !strcmp(kind, "rclick")) {
            // move at t, press at t+80, release after hold ms (default 120): the game applies mouse
            // deltas only at the end of each DirectInput batch, so it must move first and press after
            int right = kind[0] == 'r', hold = 120;
            e->kind = EV_MOVE; sscanf(arg, "%d,%d,%d", &e->a, &e->b, &hold);
            ScriptEv *d = &script[nscript++];
            *d = *e; d->t = t + 80; d->kind = right ? EV_RDOWN : EV_LDOWN;
            ScriptEv *u = &script[nscript++];
            *u = *e; u->t = t + 80 + (uint32_t)hold; u->kind = right ? EV_RUP : EV_LUP;
        } else if (!strcmp(kind, "move")) { e->kind = EV_MOVE; sscanf(arg, "%d,%d", &e->a, &e->b); }
        else if (!strcmp(kind, "quit")) e->kind = EV_QUIT;
        else nscript--;
    }
    qsort(script, nscript, sizeof script[0], ev_cmp);
}
static void script_tick(void) {
    uint32_t t = now_ms() - g_start_ms;
    while (iscript < nscript && script[iscript].t <= t) {
        ScriptEv *e = &script[iscript++];
        switch (e->kind) {
        case EV_KEY: key_event((uint8_t)e->a, (uint8_t)e->b, 1); key_event((uint8_t)e->a, (uint8_t)e->b, 0); break;
        case EV_MOVE:
            vmove(e->a, e->b);
            msgq_post(g_hwnd, 0x200, 0, (uint32_t)(g_mouse_x & 0xffff) | ((uint32_t)g_mouse_y << 16));
            break;
        case EV_LDOWN: case EV_RDOWN: mouse_button(e->kind == EV_LDOWN ? 0 : 1, 1); break;
        case EV_LUP: case EV_RUP: mouse_button(e->kind == EV_LUP ? 0 : 1, 0); break;
        case EV_QUIT: fprintf(stderr, "script: end\n"); rt_exit(0);
        }
    }
}

void plat_init(void) {
    // only audio: DirectSound mixes into an SDL audio stream
    if (!getenv("SDL_AUDIO_DRIVER")) SDL_SetHint(SDL_HINT_AUDIO_DRIVER, "dummy");
    if (!SDL_Init(SDL_INIT_AUDIO)) WARN("SDL audio init: %s", SDL_GetError());
    g_start_ms = now_ms();
    LOG("headless platform");
}
void plat_pump(void) { if (nscript) script_tick(); }
void plat_quit(void) { SDL_Quit(); }

SDL_GPUDevice *plat_gpu_device(int *window_pixel_h) { (void)window_pixel_h; return NULL; }

static void write_bmp(const char *path, const uint32_t *px, int w, int h) {
    FILE *f = fopen(path, "wb");
    if (!f) { WARN("screenshot: cannot write %s", path); return; }
    uint32_t row = (uint32_t)w * 4, size = 54 + row * (uint32_t)h;
    uint8_t hd[54] = {'B', 'M'};
    #define P32(o, v) do { uint32_t _v = (v); hd[o] = _v; hd[o + 1] = _v >> 8; hd[o + 2] = _v >> 16; hd[o + 3] = _v >> 24; } while (0)
    P32(2, size); P32(10, 54); P32(14, 40); P32(18, (uint32_t)w); P32(22, (uint32_t)-h);   // top-down
    hd[26] = 1; hd[28] = 32; P32(34, row * (uint32_t)h);
    #undef P32
    fwrite(hd, 1, 54, f);
    fwrite(px, 4, (size_t)w * h, f);   // little-endian XRGB = BGRA bytes, what BMP expects
    fclose(f);
}

#ifdef POPTB_WEB
// The game runs in a worker; the page owns the canvas. One frame in flight: the page copies it and
// clears web_busy, and frames that arrive meanwhile are dropped (the game keeps its own pace).
static uint8_t *web_fb;
static int web_fb_size;
static volatile int web_busy;
static void web_present(int w, int h) {
    if (__atomic_load_n(&web_busy, __ATOMIC_ACQUIRE)) return;
    int n = w * h * 4;
    if (n > web_fb_size) { free(web_fb); web_fb = malloc((size_t)n); web_fb_size = n; }
    const uint32_t *s = rgba;
    uint8_t *d = web_fb;
    for (int i = 0; i < w * h; i++, d += 4) {   // XRGB -> RGBA bytes (ImageData)
        uint32_t c = s[i];
        d[0] = (uint8_t)(c >> 16); d[1] = (uint8_t)(c >> 8); d[2] = (uint8_t)c; d[3] = 255;
    }
    __atomic_store_n(&web_busy, 1, __ATOMIC_RELEASE);
    MAIN_THREAD_ASYNC_EM_ASM({
        var px = HEAPU8.slice($0, $0 + $1 * $2 * 4);
        Atomics.store(HEAP32, $3 >> 2, 0);
        if (Module.poptbFrame) Module.poptbFrame(px, $1, $2);
    }, web_fb, w, h, &web_busy);
}
#endif

static void finish_frame(int w, int h) {
    extern const char *g_shot_path, *g_shot_dir; extern int g_shot_frame, g_shot_every;
    static int frames;
    frames++;
    char p[1024] = "";
    if (g_shot_dir && g_shot_every && frames % g_shot_every == 0)
        snprintf(p, sizeof p, "%s/shot_%06u_f%05d.bmp", g_shot_dir, now_ms() - g_start_ms, frames);
    if (g_shot_path && frames == g_shot_frame) snprintf(p, sizeof p, "%s", g_shot_path);
    if (p[0]) {
        write_bmp(p, rgba, w, h);
        if (g_shot_path && frames == g_shot_frame) fprintf(stderr, "screenshot saved to %s (frame %d)\n", p, frames);
    }
#ifdef POPTB_WEB
    web_present(w, h);
#endif
}
static void set_size(int w, int h) {
    if (w == tex_w && h == tex_h && rgba) return;
    free(rgba); rgba = malloc((size_t)w * h * 4);
    tex_w = w; tex_h = h;
}
static inline uint32_t rgb565_to_xrgb(uint32_t c) {
    uint32_t r = (c >> 11) & 31, g = (c >> 5) & 63, b = c & 31;
    return ((r << 3 | r >> 2) << 16) | ((g << 2 | g >> 4) << 8) | (b << 3 | b >> 2);
}
void plat_present(const uint8_t *pix, int w, int h, int pitch, const uint32_t *pal) {
    set_size(w, h);
    for (int y = 0; y < h; y++) {
        const uint8_t *s = pix + (size_t)y * pitch;
        uint32_t *d = rgba + (size_t)y * w;
        if (pal) for (int x = 0; x < w; x++) d[x] = pal[s[x]];
        else for (int x = 0; x < w; x++) d[x] = rgb565_to_xrgb((uint32_t)(s[2 * x] | s[2 * x + 1] << 8));
    }
    finish_frame(w, h);
}
void plat_present_hybrid(const uint16_t *pix, int w, int h, int pitch, const uint8_t *mask) {
    (void)pix; (void)w; (void)h; (void)pitch; (void)mask;
    rt_fatal("the Direct3D build needs a window (not supported by the headless platform)");
}
