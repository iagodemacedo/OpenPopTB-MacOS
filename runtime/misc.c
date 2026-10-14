// WINMM (timers, mmio, MCI/MIDI), WSOCK32 and networking (weanetr) — networking disabled.
#include "handles.h"
#include <unistd.h>
#include <fcntl.h>
#include <pthread.h>

// ---------------- timers ----------------
typedef struct { uint32_t id, delay, cb, user, periodic; volatile int dead; pthread_t th; } Timer;
static Timer *timers[64];

static void *timer_main(void *arg) {
    Timer *t = arg;
    CPU *R = NULL;
    do {
        usleep(t->delay * 1000);
        if (t->dead) break;
        gil_lock();
        if (!R) R = cpu_new_thread();
        if (!t->dead) guest_call(R, t->cb, 5, t->id, 0, t->user, 0, 0);
        gil_unlock();
    } while (t->periodic && !t->dead);
    return NULL;
}
SHIM(timeSetEvent, 5) {
    for (uint32_t i = 1; i < 64; i++) if (!timers[i]) {
        Timer *t = calloc(1, sizeof *t);
        t->id = i; t->delay = ARG(0) ? ARG(0) : 1; t->cb = ARG(2); t->user = ARG(3); t->periodic = ARG(4) & 1;
        timers[i] = t;
        pthread_create(&t->th, NULL, timer_main, t);
        pthread_detach(t->th);
        LOG("timeSetEvent(%u ms, cb=%x, periodic=%u) -> %u", t->delay, t->cb, t->periodic, i);
        return i;
    }
    return 0;
}
SHIM(timeKillEvent, 1) {
    uint32_t i = ARG(0);
    if (i < 64 && timers[i]) { timers[i]->dead = 1; timers[i] = NULL; }
    return 0;
}
SHIM(timeGetTime, 0) { gil_yield(); return now_ms(); }
SHIM(timeGetSystemTime, 2) { return 0; }
SHIM(timeBeginPeriod, 1) { return 0; }
SHIM(timeEndPeriod, 1) { return 0; }

// ---------------- MIDI / aux / MCI: no devices ----------------
SHIM(midiOutGetNumDevs, 0) { return 0; }
SHIM(midiOutGetDevCapsA, 3) { return 2; }   // MMSYSERR_BADDEVICEID
SHIM(midiOutOpen, 5) { return 2; }
SHIM(midiOutClose, 1) { return 0; }
SHIM(midiOutShortMsg, 2) { return 0; }
SHIM(midiOutLongMsg, 3) { return 0; }
SHIM(midiOutPrepareHeader, 3) { return 0; }
SHIM(auxGetNumDevs, 0) { return 0; }
SHIM(auxGetDevCapsA, 3) { return 2; }
SHIM(auxGetVolume, 2) { if (ARG(1)) wr32(ARG(1), 0xffffffff); return 0; }
SHIM(auxSetVolume, 2) { return 0; }
SHIM(mciSendCommandA, 4) { LOG("mciSendCommandA(dev=%x msg=%x)", ARG(0), ARG(1)); return 263; }   // MCIERR_INVALID_DEVICE_NAME

// ---------------- mmio (file reading) ----------------
SHIM(mmioOpenA, 3) {
    if (!ARG(0)) return 0;
    char p[1024]; path_map(gstr(ARG(0)), p, sizeof p);
    int fd = open(p, O_RDONLY);
    if (fd < 0) { LOG("mmioOpenA(%s) failed", gstr(ARG(0))); return 0; }
    uint32_t h = handle_new(H_FILE);
    handle_get(h)->fd = fd;
    LOG("mmioOpenA(%s) -> %x", gstr(ARG(0)), h);
    return h;
}
SHIM(mmioClose, 2) { handle_close(ARG(0)); return 0; }
SHIM(mmioRead, 3) { Handle *x = handle_get(ARG(0)); if (!x) return 0xffffffff; ssize_t r = read(x->fd, PTR(ARG(1)), ARG(2)); return r < 0 ? 0xffffffff : (uint32_t)r; }
SHIM(mmioSeek, 3) { Handle *x = handle_get(ARG(0)); if (!x) return 0xffffffff; return (uint32_t)lseek(x->fd, (int32_t)ARG(1), (int)ARG(2)); }
SHIM(mmioSetBuffer, 4) { return 0; }

// ---------------- weanetr (MLDPlay, thiscall): used only if weanetr.dll is not present ----------------
SHIM_AS(mld_enumsess, "?EnumerateSessions@MLDPlay@@QAEHKP6GXPAUMLDPLAY_SESSIONDESC@@PAX@ZK1@Z", 4) { return 0; }
SHIM_AS(mld_msresults, "?SendMSResults@MLDPlay@@QAEKPAD@Z", 1) { return 0; }
SHIM_AS(mld_shutdown, "?ShutdownNetwork@MLDPlay@@QAEHXZ", 0) { return 1; }
SHIM_AS(mld_startup, "?StartupNetwork@MLDPlay@@QAEHP6GXKPAXKK0@Z@Z", 1) { return 0; }
SHIM_AS(mld_lobbied, "?AreWeLobbied@MLDPlay@@QAEKP6GXKPAXKK0@ZPAU_GUID@@PAUMLDPLAY_LOBBYINFO@@PAKPAG5KK@Z", 8) { return 0x20; }   // 0x20 = not launched by a lobby (default value in weanetr.dll)
// The game assumes service providers exist (TCP/IP, IPX, modem); we create fake objects
// with the layout read from weanetr.dll: +0x10 GUID count, +0x18 name (UTF-16), +0x1c next, +0x24 GUIDs.
SHIM_AS(mld_enumsvc, "?EnumerateServices@MLDPlay@@QAEHP6GXPAXPAGPAU_GUID@@K0@Z0@Z", 2) {
    static uint32_t svc[3];
    static const char *names[3] = {"Internet TCP/IP", "IPX", "Modem"};
    if (!svc[0]) {
        static const char *vm[16] = {"m0","m1","m2","m3","m4","m5","m6","m7","m8","m9","m10","m11","m12","m13","m14","m15"};
        native_fn none[16] = {0};
        uint32_t vt = com_vtbl("MLDService", vm, 16, none);
        for (int i = 0; i < 3; i++) {
            svc[i] = gmalloc(0x600);
            wr32(svc[i], vt);
            uint32_t nm = gmalloc(64);
            for (int k = 0; names[i][k]; k++) wr16(nm + 2 * k, (uint8_t)names[i][k]);
            wr32(svc[i] + 0x18, nm);
        }
        for (int i = 0; i < 2; i++) wr32(svc[i] + 0x1c, svc[i + 1]);
    }
    uint32_t cb = ARG(0), ctx = ARG(1);
    for (int i = 0; i < 3; i++)
        guest_call(R, cb, 5, svc[i], rd32(svc[i] + 0x18), rd32(svc[i] + 0x24), rd32(svc[i] + 0x10), ctx);
    return 1;
}
SHIM_AS(mld_enablenew, "?EnableNewPlayers@MLDPlay@@QAEXH@Z", 1) { return 0; }
SHIM_AS(mld_netaddr, "?CreateNetworkAddress@MLDPlay@@QAEHPAXK0PAK@Z", 4) { return 0; }
SHIM_AS(mld_join, "?JoinSession@MLDPlay@@QAEHPAUMLDPLAY_SESSIONDESC@@PAKPAGPAX@Z", 4) { return 0; }
SHIM_AS(mld_create, "?CreateSession@MLDPlay@@QAEHPAKPAG1PAXK@Z", 5) { return 0; }
SHIM_AS(mld_setup, "?SetupConnection@MLDPlay@@QAEHPAXPAU_GUID@@0@Z", 3) { return 0; }
SHIM_AS(mld_destroy, "?DestroySession@MLDPlay@@QAEHXZ", 0) { return 1; }
SHIM_AS(mld_playerinfo, "?GetPlayerInfo@MLDPlay@@QAEHPAUMLDPLAY_PLAYERINFO@@@Z", 1) { return 0; }
SHIM_AS(mld_send, "?SendData@MLDPlay@@QAEHKPAXKKPAK@Z", 5) { return 0; }
SHIM_AS(mld_chat, "?SendChat@MLDPlay@@QAEHKPAGKPAK@Z", 4) { return 0; }
SHIM_AS(mld_ms, "?GetCurrentMs@MLDPlay@@QAEKXZ", 0) { return now_ms(); }
SHIM_AS(mld_enummed, "?EnumerateNetworkMediums@MLDPlay@@QAEKP6GXPAGPAX@Z1@Z", 2) { return 0; }

// ---------------- mmio (RIFF) ----------------
static uint32_t fourcc_rd(Handle *x, uint32_t *v) { return read(x->fd, v, 4) == 4; }
SHIM(mmioDescend, 4) {
    Handle *x = handle_get(ARG(0));
    uint32_t ck = ARG(1), parent = ARG(2), fl = ARG(3);
    if (!x) return 265;   // MMIOERR_CHUNKNOTFOUND
    uint32_t want = rd32(ck), wanttype = rd32(ck + 8);
    off_t end = parent ? (off_t)(rd32(parent + 12) + rd32(parent + 4)) : (off_t)1 << 40;
    for (;;) {
        off_t pos = lseek(x->fd, 0, SEEK_CUR);
        if (pos + 8 > end) return 265;
        uint32_t id, size, type = 0;
        if (!fourcc_rd(x, &id) || !fourcc_rd(x, &size)) return 265;
        int list = id == 0x46464952 /*RIFF*/ || id == 0x5453494c /*LIST*/;
        if (list) fourcc_rd(x, &type);
        int ok = 1;
        if (fl & 0x10) ok = id == want;
        else if (fl & 0x20) ok = id == 0x46464952 && (!wanttype || type == wanttype);
        else if (fl & 0x40) ok = id == 0x5453494c && (!wanttype || type == wanttype);
        if (ok) {
            wr32(ck, id); wr32(ck + 4, size); wr32(ck + 8, type);
            wr32(ck + 12, (uint32_t)(pos + 8)); wr32(ck + 16, 0);
            if (!list) lseek(x->fd, pos + 8, SEEK_SET);
            return 0;
        }
        lseek(x->fd, pos + 8 + ((size + 1) & ~1u), SEEK_SET);
        if (!fl) return 265;
    }
}
SHIM(mmioAscend, 3) {
    Handle *x = handle_get(ARG(0));
    uint32_t ck = ARG(1);
    if (!x) return 0;
    lseek(x->fd, rd32(ck + 12) + ((rd32(ck + 4) + 1) & ~1u), SEEK_SET);
    return 0;
}
SHIM(mmioGetInfo, 3) { memset(PTR(ARG(1)), 0, 72); return 0; }
SHIM(mmioCreateChunk, 3) { return 0; }
SHIM(mmioWrite, 3) { Handle *x = handle_get(ARG(0)); if (!x) return 0xffffffff; return (uint32_t)write(x->fd, PTR(ARG(1)), ARG(2)); }

// ---------------- resources: QMixer doesn't ship the ones the game uses ----------------
SHIM(FindResourceA, 3) { return 0; }
SHIM(LoadResource, 2) { return 0; }
SHIM(LockResource, 1) { return 0; }
SHIM(SizeofResource, 2) { return 0; }
SHIM(OpenFile, 3) {
    // OF_EXIST (0x4000) is the typical use; opens only to check existence
    char p[1024]; path_map(gstr(ARG(0)), p, sizeof p);
    int fd = open(p, O_RDONLY);
    if (fd < 0) return 0xffffffff;
    if (ARG(2) & 0x4000) { close(fd); return 1; }
    uint32_t h = handle_new(H_FILE); handle_get(h)->fd = fd;
    return h;
}

// wsprintfA (cdecl, Win32-style varargs: %s %d %u %x %X %c %ld %lu %lx, width and '0')
static uint32_t gsprintf(char *out, size_t cap, const char *fmt, uint32_t args) {
    size_t o = 0;
    for (const char *f = fmt; *f && o + 1 < cap; f++) {
        if (*f != '%') { out[o++] = *f; continue; }
        char spec[16] = "%"; int k = 1;
        f++;
        while (*f && strchr("-0123456789#", *f) && k < 12) spec[k++] = *f++;
        if (*f == 'l' || *f == 'h') f++;
        if (!*f) break;
        char buf[512];
        uint32_t v = rd32(args);
        switch (*f) {
        case 's': spec[k++] = 's'; spec[k] = 0; snprintf(buf, sizeof buf, spec, v ? gstr(v) : "(null)"); args += 4; break;
        case 'd': case 'i': spec[k++] = 'd'; spec[k] = 0; snprintf(buf, sizeof buf, spec, (int32_t)v); args += 4; break;
        case 'u': case 'x': case 'X': spec[k++] = *f; spec[k] = 0; snprintf(buf, sizeof buf, spec, v); args += 4; break;
        case 'c': buf[0] = (char)v; buf[1] = 0; args += 4; break;
        case '%': strcpy(buf, "%"); break;
        default: buf[0] = 0; break;
        }
        for (char *c = buf; *c && o + 1 < cap; c++) out[o++] = *c;
    }
    out[o] = 0;
    return (uint32_t)o;
}
uint32_t gsprintf_va(char *out, size_t cap, const char *fmt, uint32_t args) { return gsprintf(out, cap, fmt, args); }
SHIM(wsprintfA, -1) {
    char buf[1025];
    uint32_t n = gsprintf(buf, sizeof buf, gstr(ARG(1)), R->esp + 12);
    memcpy(PTR(ARG(0)), buf, n + 1);
    return n;
}
