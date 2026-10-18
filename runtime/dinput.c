// DirectInput (system keyboard and mouse) fed by SDL events.
#include "rt.h"
#include "input.h"
#include "handles.h"

uint8_t di_keys[256];
int32_t di_mouse_dx, di_mouse_dy, di_mouse_dz;
uint8_t di_mouse_btn[4];

#define BUFN 256
static DIEvent kbuf[BUFN], mbuf[BUFN];
static int kh, kt, mh, mt;
static uint32_t seq;
static uint32_t ev_key, ev_mouse;   // SetEventNotification events

static void signal_ev(uint32_t h) {
    Handle *x = handle_get(h);
    if (x && x->type == H_EVENT) { x->signaled = 1; handles_signal(); }
}

void di_push_key(uint32_t dik, int down) {
    di_keys[dik & 0xff] = down ? 0x80 : 0;
    int n = (kt + 1) % BUFN;
    if (n == kh) kh = (kh + 1) % BUFN;
    kbuf[kt] = (DIEvent){dik, down ? 0x80 : 0, now_ms(), ++seq};
    kt = n;
    if (ev_key) signal_ev(ev_key);
}
void di_push_mouse(uint32_t ofs, uint32_t data) {
    int n = (mt + 1) % BUFN;
    if (n == mh) mh = (mh + 1) % BUFN;
    mbuf[mt] = (DIEvent){ofs, data, now_ms(), ++seq};
    mt = n;
    if (ev_mouse) signal_ev(ev_mouse);
}
int di_pop_key(DIEvent *e) { if (kh == kt) return 0; *e = kbuf[kh]; kh = (kh + 1) % BUFN; return 1; }
int di_pop_mouse(DIEvent *e) { if (mh == mt) return 0; *e = mbuf[mh]; mh = (mh + 1) % BUFN; return 1; }
int di_key_count(void) { return (kt - kh + BUFN) % BUFN; }
int di_mouse_count(void) { return (mt - mh + BUFN) % BUFN; }

typedef struct { int mouse; int acquired; uint32_t fmt_size; } Dev;
static uint32_t vt_di, vt_dev;

COMFN(di_QueryInterface, 2) { wr32(ARG(2), THIS); return 0; }
COMFN(di_CreateDevice, 3) {
    uint32_t g = ARG(1);
    Dev *d = calloc(1, sizeof *d);
    uint32_t d1 = rd32(g);
    if (d1 == 0x6F1D2B60) d->mouse = 1;
    else if (d1 == 0x6F1D2B61) d->mouse = 0;
    else { WARN("DirectInput::CreateDevice unknown GUID %08x", d1); free(d); return 0x80040154; }
    wr32(ARG(2), com_new("IDirectInputDevice", vt_dev, 0, d));
    LOG("DirectInput CreateDevice %s", d->mouse ? "mouse" : "keyboard");
    return 0;
}
COMFN(di_EnumDevices, 4) { return 0; }
COMFN(di_GetDeviceStatus, 1) { return 0; }
COMFN(di_Initialize, 2) { return 0; }

static Dev *D(uint32_t o) { return com_native(o); }
COMFN(dv_GetCapabilities, 1) {
    uint32_t c = ARG(1); uint32_t sz = rd32(c);
    memset(PTR(c), 0, sz); wr32(c, sz);
    wr32(c + 4, 5);   // DIDC_ATTACHED | DIDC_POLLEDDEVICE
    Dev *d = D(THIS);
    wr32(c + 8, d->mouse ? 0x12 : 0x13);
    wr32(c + 12, d->mouse ? 3 : 0); wr32(c + 16, d->mouse ? 3 : 128);
    return 0;
}
COMFN(dv_EnumObjects, 3) { return 0; }
COMFN(dv_GetProperty, 2) { return 0; }
COMFN(dv_SetProperty, 2) { return 0; }
COMFN(dv_Acquire, 0) { D(THIS)->acquired = 1; return 0; }
COMFN(dv_Unacquire, 0) { D(THIS)->acquired = 0; return 0; }
COMFN(dv_GetDeviceState, 2) {
    Dev *d = D(THIS);
    { static int once[2]; if (!once[d->mouse]++) LOG("GetDeviceState %s n=%u", d->mouse ? "mouse" : "keyboard", ARG(1)); }
    uint32_t n = ARG(1), p = ARG(2);
    plat_pump();
    if (!d->mouse) { memcpy(PTR(p), di_keys, n < 256 ? n : 256); return 0; }
    memset(PTR(p), 0, n);
    wr32(p, (uint32_t)di_mouse_dx); wr32(p + 4, (uint32_t)di_mouse_dy); wr32(p + 8, (uint32_t)di_mouse_dz);
    di_mouse_dx = di_mouse_dy = di_mouse_dz = 0;
    for (uint32_t i = 0; i < 4 && 12 + i < n; i++) wr8(p + 12 + i, di_mouse_btn[i]);
    return 0;
}
COMFN(dv_GetDeviceData, 4) {
    Dev *d = D(THIS);
    { static int once[2]; if (!once[d->mouse]++) LOG("GetDeviceData %s sz=%u", d->mouse ? "mouse" : "keyboard", ARG(1)); }
    uint32_t sz = ARG(1), out = ARG(2), pn = ARG(3), fl = ARG(4);
    plat_pump();
    uint32_t cap = rd32(pn), k = 0;
    if (!out) {   // query / flush
        uint32_t c = d->mouse ? (uint32_t)di_mouse_count() : (uint32_t)di_key_count();
        if (!(fl & 1)) { DIEvent e; while (k < cap && (d->mouse ? di_pop_mouse(&e) : di_pop_key(&e))) k++; }
        wr32(pn, (fl & 1) ? c : k);
        return 0;
    }
    DIEvent e;
    while (k < cap && (d->mouse ? di_pop_mouse(&e) : di_pop_key(&e))) {
        uint32_t p = out + k * sz;
        wr32(p, e.ofs); wr32(p + 4, e.data); wr32(p + 8, e.time); wr32(p + 12, e.seq);
        if (d->mouse && e.ofs >= 12) LOG("DI mouse button ofs=%u data=%x (sz=%u)", e.ofs, e.data, sz);
        k++;
    }
    if (d->mouse && k) {
        di_mouse_dx = di_mouse_dy = di_mouse_dz = 0;
    }
    wr32(pn, k);
    return 0;
}
COMFN(dv_SetDataFormat, 1) { D(THIS)->fmt_size = rd32(ARG(1) + 12); return 0; }
COMFN(dv_SetEventNotification, 1) {
    if (D(THIS)->mouse) ev_mouse = ARG(1); else ev_key = ARG(1);
    LOG("SetEventNotification %s -> %x", D(THIS)->mouse ? "mouse" : "keyboard", ARG(1));
    return 0;
}
COMFN(dv_SetCooperativeLevel, 2) { return 0; }
COMFN(dv_GetObjectInfo, 3) { return 0x80004001; }
COMFN(dv_GetDeviceInfo, 1) { return 0x80004001; }
COMFN(dv_RunControlPanel, 2) { return 0; }
COMFN(dv_Initialize, 3) { return 0; }

SHIM(DirectInputCreateA, 4) {
    if (!vt_di) {
        static const char *dm[] = {"QueryInterface", "AddRef", "Release", "CreateDevice", "EnumDevices", "GetDeviceStatus", "RunControlPanel", "Initialize"};
        native_fn df[] = {di_QueryInterface, com_addref, com_release, di_CreateDevice, di_EnumDevices, di_GetDeviceStatus, NULL, di_Initialize};
        vt_di = com_vtbl("IDirectInput", dm, 8, df);
        static const char *vm[] = {"QueryInterface", "AddRef", "Release", "GetCapabilities", "EnumObjects", "GetProperty",
            "SetProperty", "Acquire", "Unacquire", "GetDeviceState", "GetDeviceData", "SetDataFormat", "SetEventNotification",
            "SetCooperativeLevel", "GetObjectInfo", "GetDeviceInfo", "RunControlPanel", "Initialize"};
        native_fn vf[] = {di_QueryInterface, com_addref, com_release, dv_GetCapabilities, dv_EnumObjects, dv_GetProperty,
            dv_SetProperty, dv_Acquire, dv_Unacquire, dv_GetDeviceState, dv_GetDeviceData, dv_SetDataFormat,
            dv_SetEventNotification, dv_SetCooperativeLevel, dv_GetObjectInfo, dv_GetDeviceInfo, dv_RunControlPanel, dv_Initialize};
        vt_dev = com_vtbl("IDirectInputDevice", vm, 18, vf);
    }
    wr32(ARG(2), com_new("IDirectInput", vt_di, 0, NULL));
    LOG("DirectInputCreateA version %x", ARG(1));
    return 0;
}
