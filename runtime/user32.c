// USER32/GDI32/IMM32/SHELL32/OLE32: single window, message queue, guest WndProc.
#include "handles.h"
#include "input.h"
#include <unistd.h>
#include <pthread.h>

#define QN 1024
static WMsg q[QN];
static int qh, qt;
static uint32_t seqtime;

static pthread_mutex_t q_mu = PTHREAD_MUTEX_INITIALIZER;
uint32_t g_main_tid = 0x100;
static uint32_t win_tid(uint32_t hwnd);

void msgq_post_tid(uint32_t tid, uint32_t hwnd, uint32_t msg, uint32_t wp, uint32_t lp) {
    pthread_mutex_lock(&q_mu);
    int n = (qt + 1) % QN;
    if (n != qh) {
        q[qt] = (WMsg){hwnd, msg, wp, lp, now_ms(), (uint32_t)g_mouse_x, (uint32_t)g_mouse_y, tid};
        qt = n;
    } else {   // queue full: drop
        static int ndrop; if (ndrop++ < 5) WARN("message queue full, dropping msg %x for window %x (thread %x)", msg, hwnd, tid);
    }
    pthread_mutex_unlock(&q_mu);
    (void)seqtime;
}
void msgq_post(uint32_t hwnd, uint32_t msg, uint32_t wp, uint32_t lp) {
    if (!hwnd) hwnd = g_hwnd;
    msgq_post_tid(win_tid(hwnd), hwnd, msg, wp, lp);
}
int msgq_peek(WMsg *out, uint32_t tid, uint32_t hwnd, uint32_t lo, uint32_t hi, int remove) {
    pthread_mutex_lock(&q_mu);
    int r = 0;
    for (int i = qh; i != qt; i = (i + 1) % QN) {
        WMsg *m = &q[i];
        if (m->tid != tid) continue;
        if (hwnd && m->hwnd != hwnd) continue;
        if ((lo || hi) && (m->msg < lo || m->msg > hi)) continue;
        *out = *m;
        if (remove) {
            // removes from the middle of the queue preserving order
            for (int j = i; j != qh; j = (j - 1 + QN) % QN) q[j] = q[(j - 1 + QN) % QN];
            qh = (qh + 1) % QN;
        }
        r = 1;
        break;
    }
    pthread_mutex_unlock(&q_mu);
    return r;
}

// ---- classes / windows ----
typedef struct { char name[64]; uint32_t atom, wndproc; } WClass;
static WClass classes[32];
static int nclasses;
typedef struct { uint32_t hwnd, wndproc, style, exstyle; int x, y, w, h; uint32_t longs[16]; uint32_t tid; } Win;
static Win wins[32];
static int nwins;
static int quit_posted;

static Win *win_of(uint32_t h) { for (int i = 0; i < nwins; i++) if (wins[i].hwnd == h) return &wins[i]; return NULL; }
static uint32_t win_tid(uint32_t hwnd) { Win *w = win_of(hwnd); return w && w->tid ? w->tid : g_main_tid; }

static uint32_t call_wndproc(CPU *R, uint32_t hwnd, uint32_t msg, uint32_t wp, uint32_t lp) {
    Win *w = win_of(hwnd);
    if (!w || !w->wndproc) return 0;
    return guest_call(R, w->wndproc, 4, hwnd, msg, wp, lp);
}

SHIM(RegisterClassA, 1) {
    uint32_t wc = ARG(0);
    WClass *c = &classes[nclasses++];
    c->wndproc = rd32(wc + 4);
    uint32_t nm = rd32(wc + 36);
    if (nm >= 0x10000) snprintf(c->name, sizeof c->name, "%s", gstr(nm));
    c->atom = 0xc000 + nclasses;
    LOG("RegisterClassA(%s) wndproc=%x", c->name, c->wndproc);
    return c->atom;
}
SHIM(UnregisterClassA, 2) { return 1; }
SHIM(CreateWindowExA, 12) {
    uint32_t cls = ARG(1);
    WClass *c = NULL;
    for (int i = 0; i < nclasses; i++)
        if ((cls < 0x10000 && classes[i].atom == cls) || (cls >= 0x10000 && !strcasecmp(classes[i].name, gstr(cls)))) c = &classes[i];
    if (!c) { WARN("CreateWindowExA: unknown class %s", cls >= 0x10000 ? gstr(cls) : "(atom)"); return 0; }
    Win *w = &wins[nwins++];
    memset(w, 0, sizeof *w);
    w->hwnd = 0x10010 + nwins * 0x10;
    w->wndproc = c->wndproc;
    w->tid = nthread(R)->tid;
    w->exstyle = ARG(0); w->style = ARG(3);
    w->x = (int)ARG(4); w->y = (int)ARG(5); w->w = (int)ARG(6); w->h = (int)ARG(7);
    // main window = the first one created by the game itself (QMixer creates a hidden window earlier)
    if (!g_hwnd && w->wndproc >= GUEST_IMAGE_BASE && w->wndproc < 0x01000000) { g_hwnd = w->hwnd; g_wndproc = w->wndproc; }
    LOG("CreateWindowExA(%s, %s) -> %x", c->name, ARG(2) ? gstr(ARG(2)) : "", w->hwnd);
    uint32_t cs = gmalloc(48);
    wr32(cs + 0, ARG(11)); wr32(cs + 4, ARG(10)); wr32(cs + 8, ARG(9)); wr32(cs + 12, ARG(8));
    wr32(cs + 16, ARG(7)); wr32(cs + 20, ARG(6)); wr32(cs + 24, ARG(5)); wr32(cs + 28, ARG(4));
    wr32(cs + 32, ARG(3)); wr32(cs + 36, ARG(2)); wr32(cs + 40, ARG(1)); wr32(cs + 44, ARG(0));
    uint32_t hwnd = w->hwnd;
    call_wndproc(R, hwnd, 0x81, 0, cs);   // WM_NCCREATE
    call_wndproc(R, hwnd, 0x01, 0, cs);   // WM_CREATE
    gfree(cs);
    return hwnd;
}
SHIM(DestroyWindow, 1) {
    uint32_t h = ARG(0);
    call_wndproc(R, h, 0x02, 0, 0);   // WM_DESTROY
    Win *w = win_of(h);
    if (w) w->wndproc = 0;
    return 1;
}
SHIM(ShowWindow, 2) {
    uint32_t h = ARG(0);
    static int shown;
    if (!shown && h == g_hwnd) {
        shown = 1;
        msgq_post(h, 0x1c, 1, 0);       // WM_ACTIVATEAPP
        msgq_post(h, 0x06, 1, 0);       // WM_ACTIVATE (WA_ACTIVE)
        msgq_post(h, 0x07, 0, 0);       // WM_SETFOCUS
    }
    return 0;
}
SHIM(UpdateWindow, 1) { return 1; }
SHIM(SetWindowPos, 7) { return 1; }
SHIM(GetWindowRect, 2) {
    Win *w = win_of(ARG(0));
    uint32_t r = ARG(1);
    wr32(r, 0); wr32(r + 4, 0); wr32(r + 8, w ? w->w : 640); wr32(r + 12, w ? w->h : 480);
    return 1;
}
SHIM(GetClientRect, 2) {
    Win *w = win_of(ARG(0));
    uint32_t r = ARG(1);
    wr32(r, 0); wr32(r + 4, 0); wr32(r + 8, w ? w->w : 640); wr32(r + 12, w ? w->h : 480);
    return 1;
}
SHIM(ClientToScreen, 2) { return 1; }
SHIM(AdjustWindowRectEx, 4) { return 1; }
SHIM(GetWindowLongA, 2) {
    Win *w = win_of(ARG(0)); int32_t i = (int32_t)ARG(1);
    if (!w) return 0;
    if (i == -16) return w->style; if (i == -20) return w->exstyle; if (i == -4) return w->wndproc;
    if (i >= 0 && i < 64) return w->longs[i / 4];
    return 0;
}
SHIM(SetWindowLongA, 3) {
    Win *w = win_of(ARG(0)); int32_t i = (int32_t)ARG(1); uint32_t v = ARG(2), old = 0;
    if (!w) return 0;
    if (i == -16) { old = w->style; w->style = v; }
    else if (i == -20) { old = w->exstyle; w->exstyle = v; }
    else if (i == -4) { old = w->wndproc; w->wndproc = v; }
    else if (i >= 0 && i < 64) { old = w->longs[i / 4]; w->longs[i / 4] = v; }
    return old;
}
SHIM(SetWindowTextA, 2) { return 1; }
SHIM(SetDlgItemTextA, 3) { return 1; }
SHIM(CreateDialogParamA, 5) { WARN("CreateDialogParamA ignored"); return 0; }
SHIM(InvalidateRect, 3) { return 1; }
SHIM(BeginPaint, 2) { memset(PTR(ARG(1)), 0, 64); return 0xdc01; }
SHIM(EndPaint, 2) { return 1; }
SHIM(GetForegroundWindow, 0) { return g_hwnd; }
SHIM(GetActiveWindow, 0) { return g_hwnd; }
SHIM(SetCursor, 1) { return 0; }
SHIM(SetCursorPos, 2) { return 1; }
SHIM(LoadIconA, 2) { return 0x1c01; }
SHIM(LoadCursorA, 2) { return 0x1c02; }
SHIM(GetDC, 1) { return 0xdc01; }
SHIM(ReleaseDC, 2) { return 1; }
SHIM(GetKeyboardType, 1) { uint32_t t = ARG(0); return t == 0 ? 4 : t == 1 ? 0 : 12; }
SHIM(GetKeyboardLayout, 1) { return 0x04090409; }
SHIM(GetDoubleClickTime, 0) { return 500; }
SHIM(MessageBoxA, 4) {
    fprintf(stderr, "[MessageBox] %s: %s\n", ARG(2) ? gstr(ARG(2)) : "", ARG(1) ? gstr(ARG(1)) : "");
    return 1;   // IDOK
}
// UTF-16 guest string -> UTF-8 (surrogate pairs combined) for diagnostics
static void wide_to_utf8(uint32_t p, char *out, size_t cap) {
    size_t n = 0;
    while (p && n + 5 < cap) {
        uint32_t c = rd16(p); p += 2;
        if (!c) break;
        if (c >= 0xd800 && c < 0xdc00) {
            uint32_t lo = rd16(p);
            if (lo >= 0xdc00 && lo < 0xe000) { c = 0x10000 + ((c - 0xd800) << 10) + (lo - 0xdc00); p += 2; }
        }
        if (c < 0x80) out[n++] = (char)c;
        else if (c < 0x800) { out[n++] = (char)(0xc0 | c >> 6); out[n++] = (char)(0x80 | (c & 0x3f)); }
        else if (c < 0x10000) { out[n++] = (char)(0xe0 | c >> 12); out[n++] = (char)(0x80 | (c >> 6 & 0x3f)); out[n++] = (char)(0x80 | (c & 0x3f)); }
        else { out[n++] = (char)(0xf0 | c >> 18); out[n++] = (char)(0x80 | (c >> 12 & 0x3f)); out[n++] = (char)(0x80 | (c >> 6 & 0x3f)); out[n++] = (char)(0x80 | (c & 0x3f)); }
    }
    out[n] = 0;
}
SHIM(MessageBoxW, 4) {
    char text[1024], caption[256];
    wide_to_utf8(ARG(1), text, sizeof text);
    wide_to_utf8(ARG(2), caption, sizeof caption);
    fprintf(stderr, "[MessageBoxW] %s: %s\n", caption, text);
    return 1;
}
SHIM(OpenClipboard, 1) { return 0; }
SHIM(CloseClipboard, 0) { return 1; }
SHIM(GetClipboardData, 1) { return 0; }
SHIM(IsClipboardFormatAvailable, 1) { return 0; }
SHIM(SetRect, 5) { uint32_t r = ARG(0); wr32(r, ARG(1)); wr32(r + 4, ARG(2)); wr32(r + 8, ARG(3)); wr32(r + 12, ARG(4)); return 1; }
uint32_t gsprintf_va(char *out, size_t cap, const char *fmt, uint32_t args);
SHIM(wvsprintfA, 3) {
    char buf[1025];
    uint32_t n = gsprintf_va(buf, sizeof buf, gstr(ARG(1)), ARG(2));
    memcpy(PTR(ARG(0)), buf, n + 1);
    return n;
}
SHIM(EnumWindows, 2) { return 1; }
SHIM(GetWindowThreadProcessId, 2) { if (ARG(1)) wr32(ARG(1), 0x42); return 0x100; }
SHIM(LoadStringA, 4) { if (ARG(3)) wr8(ARG(2), 0); return 0; }

SHIM(DefWindowProcA, 4) {
    uint32_t msg = ARG(1);
    if (msg == 0x10) { call_wndproc(R, ARG(0), 0x02, 0, 0); return 0; }   // WM_CLOSE -> WM_DESTROY
    if (msg == 0x81) return 1;   // WM_NCCREATE
    return 0;
}
SHIM(PostMessageA, 4) { msgq_post(ARG(0), ARG(1), ARG(2), ARG(3)); return 1; }
SHIM(PostQuitMessage, 1) { msgq_post_tid(nthread(R)->tid, 0, 0x12, ARG(0), 0); quit_posted = 1; return 0; }

static void put_msg(uint32_t p, WMsg *m) {
    wr32(p, m->hwnd); wr32(p + 4, m->msg); wr32(p + 8, m->wp); wr32(p + 12, m->lp);
    wr32(p + 16, m->time); wr32(p + 20, m->x); wr32(p + 24, m->y);
}
SHIM(PeekMessageA, 5) {
    static uint32_t last;
    uint32_t t = now_ms();
    if (t != last) { plat_pump(); last = t; }
    gil_yield();
    WMsg m;
    if (!msgq_peek(&m, nthread(R)->tid, ARG(1), ARG(2), ARG(3), ARG(4) & 1)) return 0;
    put_msg(ARG(0), &m);
    return 1;
}
SHIM(GetMessageA, 4) {
    WMsg m;
    for (;;) {
        plat_pump();
        if (msgq_peek(&m, nthread(R)->tid, ARG(1), ARG(2), ARG(3), 1)) break;
        gil_unlock(); usleep(2000); gil_lock();
    }
    put_msg(ARG(0), &m);
    return m.msg != 0x12;
}
SHIM(TranslateMessage, 1) { return 0; }
SHIM(DispatchMessageA, 1) {
    uint32_t p = ARG(0);
    return call_wndproc(R, rd32(p), rd32(p + 4), rd32(p + 8), rd32(p + 12));
}

// ---- GDI ----
SHIM(GetSystemPaletteEntries, 4) {
    uint32_t st = ARG(1), n = ARG(2), out = ARG(3);
    if (!out) return 256;
    for (uint32_t i = 0; i < n; i++) { uint32_t c = st + i; wr8(out + i * 4, c); wr8(out + i * 4 + 1, c); wr8(out + i * 4 + 2, c); wr8(out + i * 4 + 3, 0); }
    return n;
}
SHIM(GetStockObject, 1) { return 0x5100 + ARG(0); }

// ---- IMM32 ----
SHIM(ImmGetContext, 1) { return 0; }
SHIM(ImmReleaseContext, 2) { return 1; }
SHIM(ImmGetCompositionStringA, 4) { return 0; }
SHIM(ImmSetOpenStatus, 2) { return 1; }
SHIM(ImmGetOpenStatus, 1) { return 0; }
SHIM(ImmSetCompositionWindow, 2) { return 1; }
SHIM(ImmGetCandidateListA, 4) { return 0; }
SHIM(ImmGetDescriptionA, 3) { return 0; }
SHIM(ImmNotifyIME, 4) { return 0; }
SHIM(ImmSetCompositionStringA, 6) { return 0; }

// ---- SHELL32 / OLE32 ----
SHIM(ShellExecuteA, 6) { WARN("ShellExecuteA(%s) ignored", ARG(2) ? gstr(ARG(2)) : ""); return 33; }
SHIM(CoInitialize, 1) { return 0; }
SHIM(CoUninitialize, 0) { return 0; }
uint32_t dsound_create_object(void);
SHIM(CoCreateInstance, 5) {
    uint32_t clsid = ARG(0), out = ARG(4);
    if (rd32(clsid) == 0x47D4D946) {   // CLSID_DirectSound
        uint32_t o = dsound_create_object();
        if (o) { wr32(out, o); return 0; }
    }
    LOG("CoCreateInstance(%08x) not supported", rd32(clsid));
    if (out) wr32(out, 0);
    return 0x80040154;   // REGDB_E_CLASSNOTREG
}
SHIM(CoFreeUnusedLibraries, 0) { return 0; }
