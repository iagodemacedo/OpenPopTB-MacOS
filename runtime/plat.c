// SDL3 platform: window, frame presentation, events -> Win32 messages and DirectInput.
#include "rt.h"
#include "input.h"
#include <SDL3/SDL.h>
#include <pthread.h>

static SDL_Window *win;
static SDL_Renderer *ren;
static SDL_Texture *tex;
static int tex_w = 640, tex_h = 480;
static uint32_t *rgba;
int g_mouse_x = 320, g_mouse_y = 240;
uint32_t g_start_ms;
static float vx = 320, vy = 240;   // virtual cursor in game coordinates (the game draws its own)

// moves the virtual cursor and generates the relative deltas DirectInput delivers to the game
static void vmove(float nx, float ny) {
    if (nx < 0) nx = 0; if (ny < 0) ny = 0;
    if (nx > tex_w - 1) nx = (float)(tex_w - 1); if (ny > tex_h - 1) ny = (float)(tex_h - 1);
    int ox = (int)vx, oy = (int)vy;
    vx = nx; vy = ny;
    int dx = (int)vx - ox, dy = (int)vy - oy;
    g_mouse_x = (int)vx; g_mouse_y = (int)vy;
    if (dx) { di_mouse_dx += dx; di_push_mouse(0, (uint32_t)dx); }
    if (dy) { di_mouse_dy += dy; di_push_mouse(4, (uint32_t)dy); }
}

// input script: "ms:key:NAME;ms:click:x,y;ms:rclick:x,y;ms:move:x,y;ms:quit"
typedef struct { uint32_t t; int kind; int a, b; } ScriptEv;
static ScriptEv script[256];
static int nscript, iscript;
static int ev_cmp(const void *a, const void *b) { const ScriptEv *x = a, *y = b; return x->t < y->t ? -1 : x->t > y->t; }
void plat_script(const char *spec) {
    char buf[4096]; snprintf(buf, sizeof buf, "%s", spec);
    char *save = NULL;
    for (char *it = strtok_r(buf, ";", &save); it && nscript < 256; it = strtok_r(NULL, ";", &save)) {
        char kind[32] = "", arg[64] = "";
        unsigned t = 0;
        if (sscanf(it, "%u:%31[^:]:%63s", &t, kind, arg) < 2) continue;
        ScriptEv *e = &script[nscript++];
        e->t = t;
        if (!strcmp(kind, "key")) { e->kind = 1; e->a = (int)SDL_GetScancodeFromName(arg); }
        else if (!strcmp(kind, "click") || !strcmp(kind, "rclick")) {
            // button: move+press at t, release at t+120 ms
            // the game applies deltas only at the end of each DirectInput batch: move first, press after
            int right = kind[0] == 'r';
            e->kind = 4; sscanf(arg, "%d,%d", &e->a, &e->b);
            ScriptEv *d = &script[nscript++];
            *d = *e; d->t = t + 80; d->kind = right ? 3 : 2;
            ScriptEv *u = &script[nscript++];
            *u = *e; u->t = t + 200; u->kind = right ? 7 : 6;
        }
        else if (!strcmp(kind, "move")) { e->kind = 4; sscanf(arg, "%d,%d", &e->a, &e->b); }
        else if (!strcmp(kind, "quit")) e->kind = 5;
    }
    qsort(script, nscript, sizeof script[0], ev_cmp);
}
static void game_to_window(int gx, int gy, float *wx, float *wy) {
    SDL_RenderCoordinatesToWindow(ren, (float)gx + 0.5f, (float)gy + 0.5f, wx, wy);
}
static void script_tick(void) {
    uint32_t t = now_ms() - g_start_ms;
    while (iscript < nscript && script[iscript].t <= t) {
        ScriptEv *e = &script[iscript++];
        SDL_Event ev; memset(&ev, 0, sizeof ev);
        float wx, wy;
        switch (e->kind) {
        case 1:
            ev.type = SDL_EVENT_KEY_DOWN; ev.key.scancode = (SDL_Scancode)e->a; ev.key.down = true; SDL_PushEvent(&ev);
            ev.type = SDL_EVENT_KEY_UP; ev.key.down = false; SDL_PushEvent(&ev);
            break;
        case 2: case 3: case 4:
            game_to_window(e->a, e->b, &wx, &wy);
            vmove((float)e->a, (float)e->b);
            msgq_post(g_hwnd, 0x200, 0, (uint32_t)(g_mouse_x & 0xffff) | ((uint32_t)g_mouse_y << 16));
            if (e->kind == 4) break;
            memset(&ev, 0, sizeof ev);
            ev.type = SDL_EVENT_MOUSE_BUTTON_DOWN; ev.button.x = wx; ev.button.y = wy;
            ev.button.button = e->kind == 2 ? SDL_BUTTON_LEFT : SDL_BUTTON_RIGHT; ev.button.down = true; SDL_PushEvent(&ev);
            break;
        case 6: case 7:
            memset(&ev, 0, sizeof ev);
            ev.type = SDL_EVENT_MOUSE_BUTTON_UP;
            ev.button.button = e->kind == 6 ? SDL_BUTTON_LEFT : SDL_BUTTON_RIGHT; ev.button.down = false; SDL_PushEvent(&ev);
            break;
        case 5: fprintf(stderr, "script: end\n"); exit(0);
        }
    }
}

void plat_init(void) {
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_EVENTS)) rt_fatal("SDL_Init: %s", SDL_GetError());
    win = SDL_CreateWindow("Populous: The Beginning", 1280, 960, SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
    ren = SDL_CreateRenderer(win, NULL);
    SDL_SetRenderVSync(ren, 0);
    SDL_SetRenderLogicalPresentation(ren, tex_w, tex_h, SDL_LOGICAL_PRESENTATION_LETTERBOX);
    SDL_SetRenderDrawColor(ren, 0, 0, 0, 255);
    SDL_RenderClear(ren);
    SDL_RenderPresent(ren);
    SDL_HideCursor();
    SDL_SetWindowRelativeMouseMode(win, true);
    g_start_ms = now_ms();
}

static uint32_t last_present;
void plat_present(const uint8_t *pix, int w, int h, int pitch, const uint32_t *pal) {
    if (w != tex_w || h != tex_h || !tex) {
        if (tex) SDL_DestroyTexture(tex);
        tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_XRGB8888, SDL_TEXTUREACCESS_STREAMING, w, h);
        SDL_SetTextureScaleMode(tex, SDL_SCALEMODE_NEAREST);
        SDL_SetRenderLogicalPresentation(ren, w, h, SDL_LOGICAL_PRESENTATION_LETTERBOX);
        free(rgba); rgba = malloc((size_t)w * h * 4);
        tex_w = w; tex_h = h;
    }
    for (int y = 0; y < h; y++) {
        const uint8_t *s = pix + (size_t)y * pitch;
        uint32_t *d = rgba + (size_t)y * w;
        if (pal) for (int x = 0; x < w; x++) d[x] = pal[s[x]];
        else for (int x = 0; x < w; x++) {   // RGB565
            uint32_t c = (uint32_t)(s[2 * x] | s[2 * x + 1] << 8);
            uint32_t r = (c >> 11) & 31, g = (c >> 5) & 63, b = c & 31;
            d[x] = ((r << 3 | r >> 2) << 16) | ((g << 2 | g >> 4) << 8) | (b << 3 | b >> 2);
        }
    }
    SDL_UpdateTexture(tex, NULL, rgba, w * 4);
    SDL_RenderClear(ren);
    SDL_RenderTexture(ren, tex, NULL, NULL);
    SDL_RenderPresent(ren);
    last_present = now_ms();
    if (rt_verbose > 1) { static int n; if (n++ % 10 == 0) LOG("[%u] present #%d %dx%d", now_ms() % 100000, n, w, h); }
    extern const char *g_shot_path, *g_shot_dir; extern int g_shot_frame, g_shot_every;
    static int frames;
    frames++;
    if (g_shot_dir && g_shot_every && frames % g_shot_every == 0) {
        char p[1024]; snprintf(p, sizeof p, "%s/shot_%06u_f%05d.bmp", g_shot_dir, now_ms() - g_start_ms, frames);
        SDL_Surface *s = SDL_CreateSurfaceFrom(w, h, SDL_PIXELFORMAT_XRGB8888, rgba, w * 4);
        SDL_SaveBMP(s, p);
        SDL_DestroySurface(s);
    }
    if (g_shot_path && frames == g_shot_frame) {
        SDL_Surface *s = SDL_CreateSurfaceFrom(w, h, SDL_PIXELFORMAT_XRGB8888, rgba, w * 4);
        SDL_SaveBMP(s, g_shot_path);
        SDL_DestroySurface(s);
        fprintf(stderr, "screenshot saved to %s (frame %d)\n", g_shot_path, frames);
    }
}

// ---- key tables: SDL scancode -> DIK (PC set 1 scancode) and VK ----
typedef struct { SDL_Scancode sc; uint8_t dik, vk; } KeyMap;
static const KeyMap keymap[] = {
    {SDL_SCANCODE_ESCAPE, 0x01, 0x1B}, {SDL_SCANCODE_1, 0x02, '1'}, {SDL_SCANCODE_2, 0x03, '2'}, {SDL_SCANCODE_3, 0x04, '3'},
    {SDL_SCANCODE_4, 0x05, '4'}, {SDL_SCANCODE_5, 0x06, '5'}, {SDL_SCANCODE_6, 0x07, '6'}, {SDL_SCANCODE_7, 0x08, '7'},
    {SDL_SCANCODE_8, 0x09, '8'}, {SDL_SCANCODE_9, 0x0A, '9'}, {SDL_SCANCODE_0, 0x0B, '0'}, {SDL_SCANCODE_MINUS, 0x0C, 0xBD},
    {SDL_SCANCODE_EQUALS, 0x0D, 0xBB}, {SDL_SCANCODE_BACKSPACE, 0x0E, 0x08}, {SDL_SCANCODE_TAB, 0x0F, 0x09},
    {SDL_SCANCODE_Q, 0x10, 'Q'}, {SDL_SCANCODE_W, 0x11, 'W'}, {SDL_SCANCODE_E, 0x12, 'E'}, {SDL_SCANCODE_R, 0x13, 'R'},
    {SDL_SCANCODE_T, 0x14, 'T'}, {SDL_SCANCODE_Y, 0x15, 'Y'}, {SDL_SCANCODE_U, 0x16, 'U'}, {SDL_SCANCODE_I, 0x17, 'I'},
    {SDL_SCANCODE_O, 0x18, 'O'}, {SDL_SCANCODE_P, 0x19, 'P'}, {SDL_SCANCODE_LEFTBRACKET, 0x1A, 0xDB},
    {SDL_SCANCODE_RIGHTBRACKET, 0x1B, 0xDD}, {SDL_SCANCODE_RETURN, 0x1C, 0x0D}, {SDL_SCANCODE_LCTRL, 0x1D, 0x11},
    {SDL_SCANCODE_A, 0x1E, 'A'}, {SDL_SCANCODE_S, 0x1F, 'S'}, {SDL_SCANCODE_D, 0x20, 'D'}, {SDL_SCANCODE_F, 0x21, 'F'},
    {SDL_SCANCODE_G, 0x22, 'G'}, {SDL_SCANCODE_H, 0x23, 'H'}, {SDL_SCANCODE_J, 0x24, 'J'}, {SDL_SCANCODE_K, 0x25, 'K'},
    {SDL_SCANCODE_L, 0x26, 'L'}, {SDL_SCANCODE_SEMICOLON, 0x27, 0xBA}, {SDL_SCANCODE_APOSTROPHE, 0x28, 0xDE},
    {SDL_SCANCODE_GRAVE, 0x29, 0xC0}, {SDL_SCANCODE_LSHIFT, 0x2A, 0x10}, {SDL_SCANCODE_BACKSLASH, 0x2B, 0xDC},
    {SDL_SCANCODE_Z, 0x2C, 'Z'}, {SDL_SCANCODE_X, 0x2D, 'X'}, {SDL_SCANCODE_C, 0x2E, 'C'}, {SDL_SCANCODE_V, 0x2F, 'V'},
    {SDL_SCANCODE_B, 0x30, 'B'}, {SDL_SCANCODE_N, 0x31, 'N'}, {SDL_SCANCODE_M, 0x32, 'M'}, {SDL_SCANCODE_COMMA, 0x33, 0xBC},
    {SDL_SCANCODE_PERIOD, 0x34, 0xBE}, {SDL_SCANCODE_SLASH, 0x35, 0xBF}, {SDL_SCANCODE_RSHIFT, 0x36, 0x10},
    {SDL_SCANCODE_KP_MULTIPLY, 0x37, 0x6A}, {SDL_SCANCODE_LALT, 0x38, 0x12}, {SDL_SCANCODE_SPACE, 0x39, 0x20},
    {SDL_SCANCODE_CAPSLOCK, 0x3A, 0x14}, {SDL_SCANCODE_F1, 0x3B, 0x70}, {SDL_SCANCODE_F2, 0x3C, 0x71},
    {SDL_SCANCODE_F3, 0x3D, 0x72}, {SDL_SCANCODE_F4, 0x3E, 0x73}, {SDL_SCANCODE_F5, 0x3F, 0x74}, {SDL_SCANCODE_F6, 0x40, 0x75},
    {SDL_SCANCODE_F7, 0x41, 0x76}, {SDL_SCANCODE_F8, 0x42, 0x77}, {SDL_SCANCODE_F9, 0x43, 0x78}, {SDL_SCANCODE_F10, 0x44, 0x79},
    {SDL_SCANCODE_NUMLOCKCLEAR, 0x45, 0x90}, {SDL_SCANCODE_SCROLLLOCK, 0x46, 0x91}, {SDL_SCANCODE_KP_7, 0x47, 0x67},
    {SDL_SCANCODE_KP_8, 0x48, 0x68}, {SDL_SCANCODE_KP_9, 0x49, 0x69}, {SDL_SCANCODE_KP_MINUS, 0x4A, 0x6D},
    {SDL_SCANCODE_KP_4, 0x4B, 0x64}, {SDL_SCANCODE_KP_5, 0x4C, 0x65}, {SDL_SCANCODE_KP_6, 0x4D, 0x66},
    {SDL_SCANCODE_KP_PLUS, 0x4E, 0x6B}, {SDL_SCANCODE_KP_1, 0x4F, 0x61}, {SDL_SCANCODE_KP_2, 0x50, 0x62},
    {SDL_SCANCODE_KP_3, 0x51, 0x63}, {SDL_SCANCODE_KP_0, 0x52, 0x60}, {SDL_SCANCODE_KP_PERIOD, 0x53, 0x6E},
    {SDL_SCANCODE_F11, 0x57, 0x7A}, {SDL_SCANCODE_F12, 0x58, 0x7B}, {SDL_SCANCODE_KP_ENTER, 0x9C, 0x0D},
    {SDL_SCANCODE_RCTRL, 0x9D, 0x11}, {SDL_SCANCODE_KP_DIVIDE, 0xB5, 0x6F}, {SDL_SCANCODE_RALT, 0xB8, 0x12},
    {SDL_SCANCODE_PAUSE, 0xC5, 0x13}, {SDL_SCANCODE_HOME, 0xC7, 0x24}, {SDL_SCANCODE_UP, 0xC8, 0x26},
    {SDL_SCANCODE_PAGEUP, 0xC9, 0x21}, {SDL_SCANCODE_LEFT, 0xCB, 0x25}, {SDL_SCANCODE_RIGHT, 0xCD, 0x27},
    {SDL_SCANCODE_END, 0xCF, 0x23}, {SDL_SCANCODE_DOWN, 0xD0, 0x28}, {SDL_SCANCODE_PAGEDOWN, 0xD1, 0x22},
    {SDL_SCANCODE_INSERT, 0xD2, 0x2D}, {SDL_SCANCODE_DELETE, 0xD3, 0x2E}, {SDL_SCANCODE_LGUI, 0xDB, 0x5B},
    {SDL_SCANCODE_RGUI, 0xDC, 0x5C},
};
static const KeyMap *key_of(SDL_Scancode sc) {
    for (size_t i = 0; i < sizeof keymap / sizeof keymap[0]; i++) if (keymap[i].sc == sc) return &keymap[i];
    return NULL;
}

static void mouse_to_game(float wx, float wy) {
    float x, y;
    SDL_RenderCoordinatesFromWindow(ren, wx, wy, &x, &y);
    int ix = (int)x, iy = (int)y;
    if (ix < 0) ix = 0; if (iy < 0) iy = 0;
    if (ix >= tex_w) ix = tex_w - 1; if (iy >= tex_h) iy = tex_h - 1;
    g_mouse_x = ix; g_mouse_y = iy;
}
static uint32_t mk_state(void) {
    uint32_t s = 0;
    if (di_mouse_btn[0]) s |= 1; if (di_mouse_btn[1]) s |= 2; if (di_mouse_btn[2]) s |= 0x10;
    const bool *k = SDL_GetKeyboardState(NULL);
    if (k[SDL_SCANCODE_LSHIFT] || k[SDL_SCANCODE_RSHIFT]) s |= 4;
    if (k[SDL_SCANCODE_LCTRL] || k[SDL_SCANCODE_RCTRL]) s |= 8;
    return s;
}

void plat_pump(void) {
    SDL_Event e;
    if (!pthread_main_np()) return;   // Cocoa only accepts events on the main thread
    if (nscript) script_tick();
    while (SDL_PollEvent(&e)) {
        switch (e.type) {
        case SDL_EVENT_QUIT:
            msgq_post(g_hwnd, 0x10, 0, 0);   // WM_CLOSE
            break;
        case SDL_EVENT_KEY_DOWN: case SDL_EVENT_KEY_UP: {
            int down = e.type == SDL_EVENT_KEY_DOWN;
            // Cmd+Q / Cmd+Enter: Mac shortcuts
            if (down && (e.key.mod & SDL_KMOD_GUI) && e.key.scancode == SDL_SCANCODE_Q) { fprintf(stderr, "quitting (Cmd+Q)\n"); exit(0); }
            if (down && (e.key.mod & SDL_KMOD_GUI) && e.key.scancode == SDL_SCANCODE_RETURN) {
                SDL_SetWindowFullscreen(win, !(SDL_GetWindowFlags(win) & SDL_WINDOW_FULLSCREEN)); break;
            }
            const KeyMap *k = key_of(e.key.scancode);
            if (!k) break;
            if (!(down && e.key.repeat)) di_push_key(k->dik, down);
            uint32_t lp = 1 | ((uint32_t)(k->dik & 0x7f) << 16) | ((k->dik & 0x80) ? 1u << 24 : 0) | (down ? 0 : 0xC0000000u);
            if (down && e.key.repeat) lp |= 1u << 30;
            int alt = (e.key.mod & SDL_KMOD_ALT) != 0;
            msgq_post(g_hwnd, alt ? (down ? 0x104 : 0x105) : (down ? 0x100 : 0x101), k->vk, lp);
            // WM_CHAR for control keys (text comes from SDL_EVENT_TEXT_INPUT)
            if (down && (k->vk == 0x0D || k->vk == 0x08 || k->vk == 0x1B || k->vk == 0x09)) msgq_post(g_hwnd, 0x102, k->vk, lp);
            break;
        }
        case SDL_EVENT_TEXT_INPUT:
            for (const unsigned char *c = (const unsigned char *)e.text.text; *c; c++)
                if (*c >= 32 && *c < 128) msgq_post(g_hwnd, 0x102, *c, 1);
            break;
        case SDL_EVENT_MOUSE_MOTION: {
            // deltas in window points -> game pixels
            float x0, y0, x1, y1;
            SDL_RenderCoordinatesFromWindow(ren, 0, 0, &x0, &y0);
            SDL_RenderCoordinatesFromWindow(ren, e.motion.xrel, e.motion.yrel, &x1, &y1);
            vmove(vx + (x1 - x0), vy + (y1 - y0));
            msgq_post(g_hwnd, 0x200, mk_state(), (uint32_t)(g_mouse_x & 0xffff) | ((uint32_t)g_mouse_y << 16));
            break;
        }
        case SDL_EVENT_MOUSE_BUTTON_DOWN: case SDL_EVENT_MOUSE_BUTTON_UP: {
            int down = e.type == SDL_EVENT_MOUSE_BUTTON_DOWN;
            int b = e.button.button == SDL_BUTTON_LEFT ? 0 : e.button.button == SDL_BUTTON_RIGHT ? 1 : e.button.button == SDL_BUTTON_MIDDLE ? 2 : -1;
            if (b < 0) break;
            di_mouse_btn[b] = down ? 0x80 : 0;
            di_push_mouse(12 + b, down ? 0x80 : 0);
            static const uint32_t msgs[3][2] = {{0x202, 0x201}, {0x205, 0x204}, {0x208, 0x207}};
            msgq_post(g_hwnd, msgs[b][down], mk_state(), (uint32_t)(g_mouse_x & 0xffff) | ((uint32_t)g_mouse_y << 16));
            break;
        }
        case SDL_EVENT_MOUSE_WHEEL: {
            int dz = (int)(e.wheel.y * 120);
            di_mouse_dz += dz;
            di_push_mouse(8, (uint32_t)dz);
            msgq_post(g_hwnd, 0x20A, ((uint32_t)(dz & 0xffff) << 16) | mk_state(), (uint32_t)(g_mouse_x & 0xffff) | ((uint32_t)g_mouse_y << 16));
            break;
        }
        case SDL_EVENT_WINDOW_FOCUS_GAINED:
            msgq_post(g_hwnd, 0x1c, 1, 0);
            SDL_SetWindowRelativeMouseMode(win, true);
            break;
        case SDL_EVENT_WINDOW_FOCUS_LOST:
            memset(di_keys, 0, sizeof di_keys);
            SDL_SetWindowRelativeMouseMode(win, false);
            break;
        default: break;
        }
    }
}
