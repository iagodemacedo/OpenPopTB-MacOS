// KERNEL32: memory, threads, synchronization, files, locale.
#include "handles.h"
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <fnmatch.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <errno.h>
#include <ctype.h>
#include <sched.h>

// ---------------- handles ----------------
#define MAXH 4096
static Handle htab[MAXH];
static pthread_mutex_t wait_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t wait_cv = PTHREAD_COND_INITIALIZER;

uint32_t handle_new(int type) {
    for (int i = 1; i < MAXH; i++) if (htab[i].type == H_NONE) {
        memset(&htab[i], 0, sizeof htab[i]);
        htab[i].type = type; htab[i].fd = -1;
        return 0x1000 + i * 4;
    }
    rt_fatal("out of handles");
}
Handle *handle_get(uint32_t h) {
    if (h < 0x1000 || h >= 0x1000 + MAXH * 4 || (h & 3)) return NULL;
    Handle *x = &htab[(h - 0x1000) / 4];
    return x->type ? x : NULL;
}
void handle_close(uint32_t h) {
    Handle *x = handle_get(h);
    if (!x) return;
    if (x->type == H_FILE && x->fd >= 0) close(x->fd);
    free(x->path);
    if (x->type == H_THREAD && !x->done) return;   // thread still alive: keep it
    x->type = H_NONE;
}
void handles_signal(void) {
    pthread_mutex_lock(&wait_mu);
    pthread_cond_broadcast(&wait_cv);
    pthread_mutex_unlock(&wait_mu);
}

// ---------------- threads ----------------
static __thread NThread *cur_thread;
static uint32_t next_tid = 0x100;

NThread *nthread(CPU *R) { return (NThread *)R->thread; }
CPU *cpu_current(void) { return &cur_thread->cpu; }
void set_last_error(CPU *R, uint32_t e) { nthread(R)->last_error = e; }

uint32_t thread_setup_tib(NThread *t) {
    uint32_t tib = gmalloc(0x1000);
    wr32(tib + 0x00, 0xffffffff);       // empty SEH chain
    wr32(tib + 0x04, t->stack_hi);
    wr32(tib + 0x08, t->stack_lo);
    wr32(tib + 0x18, tib);
    wr32(tib + 0x20, 0x42);             // pid
    wr32(tib + 0x24, t->tid);
    wr32(tib + 0x2c, tib + 0x800);      // TLS array
    wr32(tib + 0x30, tib + 0xc00);      // fake PEB
    t->tib = tib;
    t->cpu.fs_base = tib;
    return tib;
}

CPU *cpu_new_thread(void) {
    NThread *t = calloc(1, sizeof *t);
    t->tid = __atomic_fetch_add(&next_tid, 4, __ATOMIC_RELAXED);
    uint32_t ss = 1u << 20;
    t->stack_lo = gmalloc(ss);
    t->stack_hi = t->stack_lo + ss;
    t->cpu.esp = t->stack_hi - 64;
    t->cpu.fcw = 0x27f;     // MSVC default: 53-bit precision, round to nearest
    t->cpu.thread = t;
    t->cpu.cc_op = CCOP(CC_EFL, 4);
    thread_setup_tib(t);
    cur_thread = t;
    return &t->cpu;
}

typedef struct { uint32_t start, param, h; NThread *t; } ThreadStart;

static void *thread_main(void *arg) {
    ThreadStart ts = *(ThreadStart *)arg;
    free(arg);
    gil_lock();
    CPU *R = cpu_new_thread();
    nthread(R)->handle = ts.h;
    Handle *hh = handle_get(ts.h);
    while (hh && hh->suspended) { gil_unlock(); usleep(1000); gil_lock(); }
    uint32_t rc = guest_call(R, ts.start, 1, ts.param);
    hh = handle_get(ts.h);
    if (hh) { hh->done = 1; hh->exit_code = rc; }
    handles_signal();
    gil_unlock();
    return NULL;
}

SHIM(CreateThread, 6) {
    uint32_t start = ARG(2), param = ARG(3), flags = ARG(4), ptid = ARG(5);
    uint32_t h = handle_new(H_THREAD);
    Handle *x = handle_get(h);
    x->suspended = (flags & 4) != 0;
    ThreadStart *ts = malloc(sizeof *ts);
    ts->start = start; ts->param = param; ts->h = h;
    pthread_attr_t at; pthread_attr_init(&at);
    pthread_attr_setstacksize(&at, 64 << 20);
    pthread_create(&x->th, &at, thread_main, ts);
    pthread_attr_destroy(&at);
    if (ptid) wr32(ptid, h);
    LOG("CreateThread start=%x param=%x -> %x", start, param, h);
    return h;
}
SHIM(ResumeThread, 1) { Handle *x = handle_get(ARG(0)); if (x) x->suspended = 0; return 1; }
SHIM(ExitThread, 1) {
    NThread *t = nthread(R);
    Handle *x = handle_get(t->handle);
    if (x) { x->done = 1; x->exit_code = ARG(0); }
    handles_signal();
    gil_unlock();
    pthread_exit(NULL);
}
SHIM(GetExitCodeThread, 2) {
    Handle *x = handle_get(ARG(0));
    if (!x) return 0;
    wr32(ARG(1), x->done ? x->exit_code : 259 /*STILL_ACTIVE*/);
    return 1;
}
SHIM(SetThreadPriority, 2) { return 1; }
SHIM(GetThreadPriority, 1) { return 0; }
SHIM(GetCurrentThread, 0) { return 0xfffffffe; }
SHIM(GetCurrentThreadId, 0) { return nthread(R)->tid; }
SHIM(GetCurrentProcess, 0) { return 0xffffffff; }
SHIM(GetCurrentProcessId, 0) { return 0x42; }
SHIM(TerminateProcess, 2) { fprintf(stderr, "TerminateProcess(%u)\n", ARG(1)); exit((int)ARG(1)); }
SHIM(ExitProcess, 1) { fprintf(stderr, "ExitProcess(%u)\n", ARG(0)); fflush(stderr); _exit((int)ARG(0)); }

// TLS
SHIM(SetCommTimeouts, 2) { return 0; }
SHIM(GetCommTimeouts, 2) { return 0; }
SHIM(GetOverlappedResult, 4) { return 0; }
SHIM(TlsAlloc, 0) {
    static uint32_t used;
    for (int i = 0; i < 64; i++) if (!(used & (1ull << i)) && i < 32) { used |= 1u << i; return i; }
    return 0xffffffff;
}
SHIM(TlsFree, 1) { return 1; }
SHIM(TlsGetValue, 1) { uint32_t i = ARG(0); set_last_error(R, 0); return i < 64 ? nthread(R)->tls[i] : 0; }
SHIM(TlsSetValue, 2) { uint32_t i = ARG(0); if (i >= 64) return 0; nthread(R)->tls[i] = ARG(1); return 1; }

SHIM(GetLastError, 0) { return nthread(R)->last_error; }
SHIM(SetLastError, 1) { nthread(R)->last_error = ARG(0); return 0; }

// ---------------- synchronization ----------------
SHIM(InitializeCriticalSection, 1) { memset(PTR(ARG(0)), 0, 24); wr32(ARG(0) + 4, 0xffffffff); return 0; }
SHIM(DeleteCriticalSection, 1) { return 0; }
SHIM(EnterCriticalSection, 1) {
    uint32_t cs = ARG(0), me = nthread(R)->tid;
    for (;;) {
        uint32_t owner = rd32(cs + 12);
        if (owner == 0 || owner == me) break;
        gil_unlock(); sched_yield(); usleep(100); gil_lock();
    }
    wr32(cs + 12, me);
    wr32(cs + 8, rd32(cs + 8) + 1);
    return 0;
}
SHIM(LeaveCriticalSection, 1) {
    uint32_t cs = ARG(0);
    uint32_t n = rd32(cs + 8);
    if (n) n--;
    wr32(cs + 8, n);
    if (!n) wr32(cs + 12, 0);
    return 0;
}
SHIM(InterlockedIncrement, 1) { uint32_t v = rd32(ARG(0)) + 1; wr32(ARG(0), v); return v; }
SHIM(InterlockedDecrement, 1) { uint32_t v = rd32(ARG(0)) - 1; wr32(ARG(0), v); return v; }
SHIM(InterlockedExchange, 2) { uint32_t v = rd32(ARG(0)); wr32(ARG(0), ARG(1)); return v; }

SHIM(CreateEventA, 4) {
    uint32_t h = handle_new(H_EVENT);
    Handle *x = handle_get(h);
    x->manual = ARG(1) != 0; x->signaled = ARG(2) != 0;
    return h;
}
SHIM(SetEvent, 1) { Handle *x = handle_get(ARG(0)); if (x) x->signaled = 1; handles_signal(); return 1; }
SHIM(ResetEvent, 1) { Handle *x = handle_get(ARG(0)); if (x) x->signaled = 0; return 1; }
SHIM(PulseEvent, 1) { handles_signal(); return 1; }
SHIM(CreateSemaphoreA, 4) {
    uint32_t h = handle_new(H_SEM);
    Handle *x = handle_get(h);
    x->count = (int)ARG(1); x->max = (int)ARG(2);
    return h;
}
SHIM(ReleaseSemaphore, 3) {
    Handle *x = handle_get(ARG(0));
    if (!x) return 0;
    if (ARG(2)) wr32(ARG(2), (uint32_t)x->count);
    x->count += (int)ARG(1);
    handles_signal();
    return 1;
}
SHIM(CreateMutexA, 3) {
    uint32_t h = handle_new(H_MUTEX);
    if (ARG(1)) handle_get(h)->owner = nthread(R)->tid;
    set_last_error(R, 0);
    return h;
}
SHIM(ReleaseMutex, 1) { Handle *x = handle_get(ARG(0)); if (x) x->owner = 0; handles_signal(); return 1; }

static int try_acquire(CPU *R, uint32_t h) {
    Handle *x = handle_get(h);
    if (!x) return 1;
    switch (x->type) {
    case H_EVENT: if (x->signaled) { if (!x->manual) x->signaled = 0; return 1; } return 0;
    case H_SEM: if (x->count > 0) { x->count--; return 1; } return 0;
    case H_MUTEX: if (!x->owner || x->owner == nthread(R)->tid) { x->owner = nthread(R)->tid; return 1; } return 0;
    case H_THREAD: return x->done;
    default: return 1;
    }
}

static uint32_t wait_objs(CPU *R, uint32_t n, uint32_t harr, int all, uint32_t ms) {
    uint32_t t0 = now_ms();
    for (;;) {
        if (all) {
            int ok = 1;
            for (uint32_t i = 0; i < n; i++) { Handle *x = handle_get(rd32(harr + 4 * i)); if (x && x->type == H_EVENT && !x->signaled) ok = 0; if (x && x->type == H_THREAD && !x->done) ok = 0; if (x && x->type == H_SEM && x->count <= 0) ok = 0; }
            if (ok) { for (uint32_t i = 0; i < n; i++) try_acquire(R, rd32(harr + 4 * i)); return 0; }
        } else {
            for (uint32_t i = 0; i < n; i++) if (try_acquire(R, rd32(harr + 4 * i))) return i;
        }
        if (ms != 0xffffffff && now_ms() - t0 >= ms) return 0x102;  // WAIT_TIMEOUT
        gil_unlock();
        pthread_mutex_lock(&wait_mu);
        struct timeval tv; gettimeofday(&tv, NULL);
        struct timespec ts = {tv.tv_sec, tv.tv_usec * 1000 + 2000000};
        if (ts.tv_nsec >= 1000000000) { ts.tv_sec++; ts.tv_nsec -= 1000000000; }
        pthread_cond_timedwait(&wait_cv, &wait_mu, &ts);
        pthread_mutex_unlock(&wait_mu);
        gil_lock();
    }
}
SHIM(WaitForSingleObject, 2) {
    uint32_t h = R->esp + 4;
    return wait_objs(R, 1, h, 0, ARG(1));
}
SHIM(WaitForMultipleObjects, 4) { return wait_objs(R, ARG(0), ARG(1), ARG(2), ARG(3)); }
SHIM(Sleep, 1) {
    uint32_t ms = ARG(0);
    gil_unlock();
    if (ms) usleep(ms * 1000); else sched_yield();
    gil_lock();
    return 0;
}

SHIM(CloseHandle, 1) { handle_close(ARG(0)); return 1; }

// ---------------- memory ----------------
SHIM(HeapCreate, 3) { return handle_new(H_HEAP); }
SHIM(HeapDestroy, 1) { return 1; }
SHIM(HeapAlloc, 3) { return gmalloc(ARG(2)); }
SHIM(HeapFree, 3) { gfree(ARG(2)); return 1; }
SHIM(HeapSize, 3) { return gsize(ARG(2)); }
SHIM(HeapReAlloc, 4) {
    uint32_t p = ARG(2), sz = ARG(3);
    if (ARG(1) & 0x10) { if (sz <= rd32(p - 16)) { wr32(p - 16 + 12, sz); return p; } return 0; }
    uint32_t old = gsize(p), n = grealloc(p, sz);
    if (n && sz > old) memset(PTR(n + old), 0, sz - old);
    return n;
}
SHIM(GetProcessHeap, 0) { return 0x1004; }
SHIM(VirtualAlloc, 4) { return vmalloc(ARG(0), ARG(1)); }
SHIM(VirtualFree, 3) { return 1; }
SHIM(GlobalAlloc, 2) { return gmalloc(ARG(1)); }
SHIM(GlobalReAlloc, 3) { return grealloc(ARG(0), ARG(1)); }
SHIM(GlobalFree, 1) { gfree(ARG(0)); return 0; }
SHIM(GlobalLock, 1) { return ARG(0); }
SHIM(GlobalUnlock, 1) { return 1; }
SHIM(GlobalHandle, 1) { return ARG(0); }
SHIM(GlobalSize, 1) { return gsize(ARG(0)); }
SHIM(LocalAlloc, 2) { return gmalloc(ARG(1)); }
SHIM(LocalFree, 1) { gfree(ARG(0)); return 0; }
SHIM(IsBadReadPtr, 2) { return ARG(0) < 0x10000; }
SHIM(IsBadWritePtr, 2) { return ARG(0) < 0x10000; }
SHIM(IsBadCodePtr, 1) { return ARG(0) < 0x10000; }
SHIM(GetSystemInfo, 1) {
    uint32_t p = ARG(0);
    memset(PTR(p), 0, 36);
    wr32(p + 4, 4096); wr32(p + 8, 0x10000); wr32(p + 12, 0x7ffeffff);
    wr32(p + 16, 1); wr32(p + 20, 1); wr32(p + 24, 586); wr32(p + 28, 0x10000);
    return 0;
}

// ---------------- process / environment ----------------
static uint32_t cmdline_ptr;
extern char g_cmdline[512];
SHIM(GetCommandLineA, 0) { if (!cmdline_ptr) cmdline_ptr = gstrdup(g_cmdline); return cmdline_ptr; }
SHIM(GetVersion, 0) { return 0xC0000A04; }   // Windows 98
SHIM(GetVersionExA, 1) {
    uint32_t p = ARG(0);
    wr32(p + 4, 4); wr32(p + 8, 10); wr32(p + 12, 0x040A08AE); wr32(p + 16, 1);
    strcpy(gstr(p + 20), " A ");
    return 1;
}
SHIM(GetStartupInfoA, 1) { memset(PTR(ARG(0)), 0, 68); wr32(ARG(0), 68); return 0; }
uint32_t module_handle(const char *name);
uint32_t module_export(uint32_t base, const char *name);
SHIM(GetModuleHandleA, 1) {
    uint32_t n = ARG(0);
    if (!n) return GUEST_IMAGE_BASE;
    uint32_t h = module_handle(gstr(n));
    LOG("GetModuleHandleA(%s) -> %x", gstr(n), h ? h : 0x7e000000);
    return h ? h : 0x7e000000;   // fake system DLL: GetProcAddress resolves by name
}
SHIM(GetModuleFileNameA, 3) {
    extern const char *g_exe_name;
    char exe[64]; snprintf(exe, sizeof exe, "C:\\Populous\\%s", g_exe_name);
    const char *s = ARG(0) == 0x18000000 ? "C:\\Populous\\QMixer.dll" : exe;
    uint32_t n = ARG(2);
    strncpy(gstr(ARG(1)), s, n);
    if (n) MEM[ARG(1) + n - 1] = 0;
    return (uint32_t)strlen(gstr(ARG(1)));
}
SHIM(LoadLibraryA, 1) {
    uint32_t h = module_handle(gstr(ARG(0)));
    LOG("LoadLibraryA(%s) -> %x", gstr(ARG(0)), h ? h : 0x7e000000);
    return h ? h : 0x7e000000;
}
SHIM(FreeLibrary, 1) { return 1; }

static void unknown_proc(CPU *R) {
    rt_fatal("call to unimplemented native function (GetProcAddress)");
}
SHIM(GetProcAddress, 2) {
    uint32_t n = ARG(1);
    if (n < 0x10000) { WARN("GetProcAddress by ordinal %u", n); return 0; }
    const char *name = gstr(n);
    uint32_t a = ARG(0) != 0x7e000000 ? module_export(ARG(0), name) : 0;
    if (!a) a = shim_addr(name);
    LOG("GetProcAddress(%s) -> %x", name, a);
    if (!a) { WARN("GetProcAddress(%s): not implemented", name); return 0; }
    (void)unknown_proc;
    return a;
}
SHIM(GetEnvironmentStrings, 0) { static uint32_t p; if (!p) p = gmalloc(4); return p; }
SHIM(GetEnvironmentStringsW, 0) { static uint32_t p; if (!p) p = gmalloc(4); return p; }
SHIM(FreeEnvironmentStringsA, 1) { return 1; }
SHIM(FreeEnvironmentStringsW, 1) { return 1; }
SHIM(SetEnvironmentVariableA, 2) { return 1; }
SHIM(GetEnvironmentVariableA, 3) { return 0; }
SHIM(SetHandleCount, 1) { return ARG(0); }
SHIM(SetUnhandledExceptionFilter, 1) { return 0; }
SHIM(UnhandledExceptionFilter, 1) { return 0; }
SHIM(SetErrorMode, 1) { return 0; }
SHIM(RaiseException, 4) {
    rt_fatal("RaiseException code=%08x flags=%x nargs=%u", ARG(0), ARG(1), ARG(2));
}
SHIM(RtlUnwind, 4) { rt_fatal("RtlUnwind called (C++/SEH exceptions not supported yet)"); }
SHIM(OutputDebugStringA, 1) { LOG("dbg: %s", gstr(ARG(0))); return 0; }
SHIM(GetStdHandle, 1) { return 0xf000 + (ARG(0) & 0xff); }
SHIM(SetStdHandle, 2) { return 1; }
SHIM(GetFileType, 1) { return ARG(0) >= 0xf000 ? 2 : 1; }

// ---------------- time ----------------
uint32_t now_ms(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000ull + ts.tv_nsec / 1000000);
}
SHIM(GetTickCount, 0) { gil_yield(); return now_ms(); }
SHIM(QueryPerformanceCounter, 1) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    wr64(ARG(0), ts.tv_sec * 1000000ull + ts.tv_nsec / 1000);
    return 1;
}
SHIM(QueryPerformanceFrequency, 1) { wr64(ARG(0), 1000000); return 1; }
static void fill_systime(uint32_t p, int local) {
    time_t t = time(NULL); struct tm tm;
    if (local) localtime_r(&t, &tm); else gmtime_r(&t, &tm);
    struct timeval tv; gettimeofday(&tv, NULL);
    wr16(p, tm.tm_year + 1900); wr16(p + 2, tm.tm_mon + 1); wr16(p + 4, tm.tm_wday); wr16(p + 6, tm.tm_mday);
    wr16(p + 8, tm.tm_hour); wr16(p + 10, tm.tm_min); wr16(p + 12, tm.tm_sec); wr16(p + 14, tv.tv_usec / 1000);
}
SHIM(GetSystemTime, 1) { fill_systime(ARG(0), 0); return 0; }
SHIM(GetLocalTime, 1) { fill_systime(ARG(0), 1); return 0; }
SHIM(GetTimeZoneInformation, 1) { memset(PTR(ARG(0)), 0, 172); return 0; }

// ---------------- locale (Latin-1 1:1) ----------------
SHIM(GetACP, 0) { return 1252; }
SHIM(GetOEMCP, 0) { return 437; }
SHIM(GetCPInfo, 2) {
    uint32_t p = ARG(1);
    memset(PTR(p), 0, 20);
    wr32(p, 1); wr8(p + 4, '?');
    return 1;
}
SHIM(IsDBCSLeadByte, 1) { return 0; }
SHIM(MultiByteToWideChar, 6) {
    uint32_t src = ARG(2); int n = (int)ARG(3); uint32_t dst = ARG(4); int cap = (int)ARG(5);
    if (n < 0) n = (int)strlen(gstr(src)) + 1;
    if (!cap) return n;
    int k = n < cap ? n : cap;
    for (int i = 0; i < k; i++) wr16(dst + 2 * i, rd8(src + i));
    return k;
}
SHIM(WideCharToMultiByte, 8) {
    uint32_t src = ARG(2); int n = (int)ARG(3); uint32_t dst = ARG(4); int cap = (int)ARG(5);
    if (n < 0) { n = 0; while (rd16(src + 2 * n)) n++; n++; }
    if (!cap) return n;
    int k = n < cap ? n : cap;
    for (int i = 0; i < k; i++) { uint32_t c = rd16(src + 2 * i); wr8(dst + i, c < 256 ? c : '?'); }
    return k;
}
SHIM(GetStringTypeA, 5) {
    uint32_t src = ARG(2); int n = (int)ARG(3); uint32_t out = ARG(4);
    if (n < 0) n = (int)strlen(gstr(src)) + 1;
    for (int i = 0; i < n; i++) {
        int c = rd8(src + i); uint32_t t = 0;
        if (isupper(c)) t |= 1; if (islower(c)) t |= 2; if (isdigit(c)) t |= 4; if (isspace(c)) t |= 8;
        if (ispunct(c)) t |= 0x10; if (iscntrl(c)) t |= 0x20; if (c == ' ' || c == '\t') t |= 0x40;
        if (isxdigit(c)) t |= 0x80; if (isalpha(c)) t |= 0x100;
        wr16(out + 2 * i, t);
    }
    return 1;
}
SHIM(GetStringTypeW, 4) {
    uint32_t src = ARG(1); int n = (int)ARG(2); uint32_t out = ARG(3);
    if (n < 0) { n = 0; while (rd16(src + 2 * n)) n++; n++; }
    for (int i = 0; i < n; i++) {
        int c = rd16(src + 2 * i); uint32_t t = 0;
        if (c < 128) {
            if (isupper(c)) t |= 1; if (islower(c)) t |= 2; if (isdigit(c)) t |= 4; if (isspace(c)) t |= 8;
            if (ispunct(c)) t |= 0x10; if (iscntrl(c)) t |= 0x20; if (c == ' ' || c == '\t') t |= 0x40;
            if (isxdigit(c)) t |= 0x80; if (isalpha(c)) t |= 0x100;
        }
        wr16(out + 2 * i, t);
    }
    return 1;
}
SHIM(LCMapStringA, 6) {
    uint32_t flags = ARG(1), src = ARG(2); int n = (int)ARG(3); uint32_t dst = ARG(4); int cap = (int)ARG(5);
    if (n < 0) n = (int)strlen(gstr(src)) + 1;
    if (!cap) return n;
    for (int i = 0; i < n && i < cap; i++) {
        int c = rd8(src + i);
        if (flags & 0x100) c = tolower(c); else if (flags & 0x200) c = toupper(c);
        wr8(dst + i, c);
    }
    return n < cap ? n : cap;
}
SHIM(LCMapStringW, 6) {
    uint32_t flags = ARG(1), src = ARG(2); int n = (int)ARG(3); uint32_t dst = ARG(4); int cap = (int)ARG(5);
    if (n < 0) { n = 0; while (rd16(src + 2 * n)) n++; n++; }
    if (!cap) return n;
    for (int i = 0; i < n && i < cap; i++) {
        int c = rd16(src + 2 * i);
        if (c < 128) { if (flags & 0x100) c = tolower(c); else if (flags & 0x200) c = toupper(c); }
        wr16(dst + 2 * i, c);
    }
    return n < cap ? n : cap;
}
static int cmp_n(int a, int b) { return a < b ? 1 : a > b ? 3 : 2; }
SHIM(CompareStringA, 6) {
    uint32_t fl = ARG(1), s1 = ARG(2); int n1 = (int)ARG(3); uint32_t s2 = ARG(4); int n2 = (int)ARG(5);
    if (n1 < 0) n1 = (int)strlen(gstr(s1)); if (n2 < 0) n2 = (int)strlen(gstr(s2));
    for (int i = 0; i < n1 && i < n2; i++) {
        int a = rd8(s1 + i), b = rd8(s2 + i);
        if (fl & 1) { a = tolower(a); b = tolower(b); }
        if (a != b) return cmp_n(a, b);
    }
    return cmp_n(n1, n2);
}
SHIM(CompareStringW, 6) {
    uint32_t fl = ARG(1), s1 = ARG(2); int n1 = (int)ARG(3); uint32_t s2 = ARG(4); int n2 = (int)ARG(5);
    if (n1 < 0) { n1 = 0; while (rd16(s1 + 2 * n1)) n1++; }
    if (n2 < 0) { n2 = 0; while (rd16(s2 + 2 * n2)) n2++; }
    for (int i = 0; i < n1 && i < n2; i++) {
        int a = rd16(s1 + 2 * i), b = rd16(s2 + 2 * i);
        if ((fl & 1) && a < 128 && b < 128) { a = tolower(a); b = tolower(b); }
        if (a != b) return cmp_n(a, b);
    }
    return cmp_n(n1, n2);
}
SHIM(GetLocaleInfoA, 4) {
    uint32_t type = ARG(1), buf = ARG(2), cap = ARG(3);
    const char *v = "";
    switch (type & 0xffff) {
    case 0x0b: v = "437"; break;     // LOCALE_IDEFAULTCODEPAGE
    case 0x1004: v = "1252"; break;  // LOCALE_IDEFAULTANSICODEPAGE
    case 0x0e: v = "."; break; case 0x0f: v = ","; break;
    case 0x01: v = "0409"; break;
    default: v = "0"; break;
    }
    uint32_t n = (uint32_t)strlen(v) + 1;
    if (!cap) return n;
    if (n > cap) return 0;
    memcpy(PTR(buf), v, n);
    return n;
}
SHIM(GetLocaleInfoW, 4) {
    uint32_t type = ARG(1), buf = ARG(2), cap = ARG(3);
    const char *v = (type & 0xffff) == 0x0b ? "437" : (type & 0xffff) == 0x1004 ? "1252" : "0";
    uint32_t n = (uint32_t)strlen(v) + 1;
    if (!cap) return n;
    if (n > cap) return 0;
    for (uint32_t i = 0; i < n; i++) wr16(buf + 2 * i, v[i]);
    return n;
}
SHIM(lstrlenA, 1) { return ARG(0) ? (uint32_t)strlen(gstr(ARG(0))) : 0; }
SHIM(lstrcpyA, 2) { strcpy(gstr(ARG(0)), gstr(ARG(1))); return ARG(0); }
SHIM(lstrcatA, 2) { strcat(gstr(ARG(0)), gstr(ARG(1))); return ARG(0); }
SHIM(lstrcmpiA, 2) { return (uint32_t)strcasecmp(gstr(ARG(0)), gstr(ARG(1))); }
SHIM(lstrcmpA, 2) { return (uint32_t)strcmp(gstr(ARG(0)), gstr(ARG(1))); }
SHIM(MulDiv, 3) { return (uint32_t)(int32_t)((int64_t)(int32_t)ARG(0) * (int32_t)ARG(1) / (int32_t)ARG(2)); }
