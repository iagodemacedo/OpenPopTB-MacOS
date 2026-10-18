// Indirect call dispatch, native thunks, FPU, string operations, GIL.
#include "rt.h"
#include "table.h"
#include <stdarg.h>
#include <pthread.h>
#include <sched.h>
#include <mach/mach_time.h>
#include <execinfo.h>
#include <unistd.h>
#include <mach-o/dyld.h>

int rt_verbose = 0;

void rt_fatal(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    fprintf(stderr, "[fatal] ");
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
    fprintf(stderr, "load 0x%lx\n", (unsigned long)_dyld_get_image_header(0));
    void *bt[48]; int n = backtrace(bt, 48);
    backtrace_symbols_fd(bt, n < 14 ? n : 14, 2);
    fflush(stderr);
    _exit(3);
}

// ---- thunks ----
static native_fn thunks[GUEST_THUNK_MAX];
static const char *thunk_names[GUEST_THUNK_MAX];
static int nthunks;
static pthread_mutex_t thunk_mu = PTHREAD_MUTEX_INITIALIZER;

uint32_t thunk_new(native_fn fn, const char *name) {
    pthread_mutex_lock(&thunk_mu);
    if (nthunks >= GUEST_THUNK_MAX) rt_fatal("out of thunks");
    int i = nthunks++;
    thunks[i] = fn; thunk_names[i] = name;
    pthread_mutex_unlock(&thunk_mu);
    return GUEST_THUNK_BASE + (uint32_t)i * 4;
}
const char *thunk_name(uint32_t a) {
    if (a < GUEST_THUNK_BASE) return NULL;
    uint32_t i = (a - GUEST_THUNK_BASE) / 4;
    return i < (uint32_t)nthunks ? thunk_names[i] : NULL;
}

typedef struct { const char *name; native_fn fn; uint32_t addr; } Shim;
static Shim shims[4096];
static int nshims;
void shim_register(const char *name, native_fn fn) {
    if (nshims >= 4096) abort();
    shims[nshims].name = name; shims[nshims].fn = fn; shims[nshims].addr = 0;
    nshims++;
}
native_fn shim_lookup(const char *name) {
    for (int i = 0; i < nshims; i++) if (!strcmp(shims[i].name, name)) return shims[i].fn;
    return NULL;
}
uint32_t shim_addr(const char *name) {
    for (int i = 0; i < nshims; i++) if (!strcmp(shims[i].name, name)) {
        if (!shims[i].addr) shims[i].addr = thunk_new(shims[i].fn, shims[i].name);
        return shims[i].addr;
    }
    return 0;
}

// ---- dispatch ----
static gfn fn_find(uint32_t a) {
    int lo = 0, hi = fn_table_len - 1;
    while (lo <= hi) {
        int mid = (lo + hi) >> 1;
        uint32_t v = fn_table[mid].addr;
        if (v == a) return fn_table[mid].fn;
        if (v < a) lo = mid + 1; else hi = mid - 1;
    }
    return NULL;
}

void dispatch(CPU *R, uint32_t t) {
    if (t >= GUEST_THUNK_BASE && t < GUEST_THUNK_BASE + GUEST_THUNK_MAX * 4) {
        uint32_t i = (t - GUEST_THUNK_BASE) / 4;
        native_fn f = thunks[i];
        if (rt_verbose >= 3 && i < (uint32_t)nthunks) LOG("call %s ret=0x%08x", thunk_names[i], rd32(R->esp));
        if (f) { f(R); return; }
        if (i < (uint32_t)nthunks) rt_fatal("unimplemented Win32 function: %s (ret=0x%08x)", thunk_names[i], rd32(R->esp));
    }
    gfn g = fn_find(t);
    if (g) { g(R); return; }
    rt_fatal("dispatch: unknown target 0x%08x (ret=0x%08x esp=0x%08x)", t, rd32(R->esp), R->esp);
}

void rt_trap(CPU *R, uint32_t eip, const char *what) {
    rt_fatal("trap at 0x%08x: %s (eax=%x ecx=%x edx=%x esp=%x)", eip, what, R->eax, R->ecx, R->edx, R->esp);
}
void div_trap(CPU *R, uint32_t eip) { rt_fatal("division by zero at 0x%08x (esp=%x)", eip, R->esp); }

void rt_cpuid(CPU *R) {
    // generic Pentium without MMX/SSE: the game only uses it for detection and QMixer picks the x86 mixer
    uint32_t leaf = R->eax;
    if (leaf == 0) { R->eax = 1; R->ebx = 0x756e6547; R->edx = 0x49656e69; R->ecx = 0x6c65746e; }
    else { R->eax = 0x543; R->ebx = 0; R->ecx = 0; R->edx = 0x000001bf; }   // no MMX: forces pure x86 paths
}
uint64_t rt_rdtsc(void) {
    static mach_timebase_info_data_t tb;
    if (!tb.denom) mach_timebase_info(&tb);
    // simulates a 1 GHz clock (the game measures the CPU via rdtsc to pick the detail level)
    return mach_absolute_time() * tb.numer / tb.denom;
}

// ---- native -> guest call ----
uint32_t guest_call(CPU *R, uint32_t fn, int nargs, ...) {
    uint32_t args[16];
    va_list ap; va_start(ap, nargs);
    for (int i = 0; i < nargs; i++) args[i] = va_arg(ap, uint32_t);
    va_end(ap);
    uint32_t save_esp = R->esp;
    uint32_t sv[7] = {R->ecx, R->edx, R->ebx, R->ebp, R->esi, R->edi, R->df};
    for (int i = nargs - 1; i >= 0; i--) PUSH(args[i]);
    PUSH(MAGIC_RET);
    dispatch(R, fn);
    uint32_t r = R->eax;
    R->esp = save_esp;   // works for both stdcall and cdecl
    R->ecx = sv[0]; R->edx = sv[1]; R->ebx = sv[2]; R->ebp = sv[3]; R->esi = sv[4]; R->edi = sv[5]; R->df = sv[6];
    return r;
}

// ---- strings ----
static inline uint32_t rdn(uint32_t a, int sz) { return sz == 1 ? rd8(a) : sz == 2 ? rd16(a) : rd32(a); }
static inline void wrn(uint32_t a, int sz, uint32_t v) { if (sz == 1) wr8(a, v); else if (sz == 2) wr16(a, v); else wr32(a, v); }

void rep_movs(CPU *R, int sz, int rep) {
    int d = R->df ? -sz : sz;
    if (!rep) { wrn(R->edi, sz, rdn(R->esi, sz)); R->esi += d; R->edi += d; return; }
    uint32_t n = R->ecx;
    if (!R->df && n) {
        uint32_t bytes = n * sz;
        // memmove won't do: overlap with src < dst replicates the pattern (x86 behavior)
        if (R->edi <= R->esi || R->edi >= R->esi + bytes) memmove(PTR(R->edi), PTR(R->esi), bytes);
        else for (uint32_t i = 0; i < bytes; i++) MEM[(uint32_t)(R->edi + i)] = MEM[(uint32_t)(R->esi + i)];
        R->esi += bytes; R->edi += bytes; R->ecx = 0;
        return;
    }
    while (R->ecx) { wrn(R->edi, sz, rdn(R->esi, sz)); R->esi += d; R->edi += d; R->ecx--; }
}
void rep_stos(CPU *R, int sz, int rep) {
    int d = R->df ? -sz : sz;
    uint32_t v = sz == 1 ? (R->eax & 0xff) : sz == 2 ? (R->eax & 0xffff) : R->eax;
    if (!rep) { wrn(R->edi, sz, v); R->edi += d; return; }
    if (!R->df && sz == 1) { memset(PTR(R->edi), (int)v, R->ecx); R->edi += R->ecx; R->ecx = 0; return; }
    if (!R->df && sz == 4) {
        uint32_t n = R->ecx, a = R->edi;
        for (uint32_t i = 0; i < n; i++) wr32(a + i * 4, v);
        R->edi += n * 4; R->ecx = 0; return;
    }
    while (R->ecx) { wrn(R->edi, sz, v); R->edi += d; R->ecx--; }
}
static void set_sub_flags(CPU *R, int sz, uint32_t a, uint32_t b) {
    uint32_t m = sz == 1 ? 0xff : sz == 2 ? 0xffff : 0xffffffff;
    R->cc_op = CCOP(CC_SUB, sz); R->cc_a = a; R->cc_b = b; R->cc_res = (a - b) & m;
}
void rep_scas(CPU *R, int sz, int rep) {
    int d = R->df ? -sz : sz;
    uint32_t m = sz == 1 ? 0xff : sz == 2 ? 0xffff : 0xffffffff;
    uint32_t v = R->eax & m;
    if (!rep) { set_sub_flags(R, sz, v, rdn(R->edi, sz)); R->edi += d; return; }
    while (R->ecx) {
        uint32_t x = rdn(R->edi, sz);
        R->edi += d; R->ecx--;
        set_sub_flags(R, sz, v, x);
        if (rep == 1 && v != x) break;
        if (rep == 2 && v == x) break;
    }
}
void rep_cmps(CPU *R, int sz, int rep) {
    int d = R->df ? -sz : sz;
    if (!rep) { set_sub_flags(R, sz, rdn(R->esi, sz), rdn(R->edi, sz)); R->esi += d; R->edi += d; return; }
    while (R->ecx) {
        uint32_t x = rdn(R->esi, sz), y = rdn(R->edi, sz);
        R->esi += d; R->edi += d; R->ecx--;
        set_sub_flags(R, sz, x, y);
        if (rep == 1 && x != y) break;
        if (rep == 2 && x == y) break;
    }
}
void do_lods(CPU *R, int sz) {
    int d = R->df ? -sz : sz;
    uint32_t v = rdn(R->esi, sz);
    if (sz == 1) R->eax = (R->eax & ~0xffu) | v;
    else if (sz == 2) R->eax = (R->eax & ~0xffffu) | v;
    else R->eax = v;
    R->esi += d;
}

// ---- GIL (fair ticket lock) ----
static pthread_mutex_t gil_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t gil_cv = PTHREAD_COND_INITIALIZER;
static uint64_t gil_next, gil_serving;
static int gil_waiters;
volatile int gil_contended;

// time slice: the GIL holder only yields after holding it for at least GIL_QUANTUM
static uint64_t gil_t0, gil_quantum;
void gil_lock(void) {
    uint64_t w0 = rt_verbose ? mach_absolute_time() : 0;
    pthread_mutex_lock(&gil_mu);
    uint64_t t = gil_next++;
    gil_waiters++;
    gil_contended = 1;
    while (gil_serving != t) pthread_cond_wait(&gil_cv, &gil_mu);
    gil_waiters--;
    gil_contended = gil_waiters > 0;
    pthread_mutex_unlock(&gil_mu);
    gil_t0 = mach_absolute_time();
    if (rt_verbose) {   // report long waits for the GIL
        static mach_timebase_info_data_t tb; if (!tb.denom) mach_timebase_info(&tb);
        uint64_t ms = (gil_t0 - w0) * tb.numer / tb.denom / 1000000;
        static int n; if (ms >= 100 && n++ < 50) LOG("[%u] GIL wait %llu ms (thread %p)", now_ms() % 100000, (unsigned long long)ms, (void *)pthread_self());
    }
}
void gil_unlock(void) {
    pthread_mutex_lock(&gil_mu);
    gil_serving++;
    pthread_cond_broadcast(&gil_cv);
    pthread_mutex_unlock(&gil_mu);
}
void gil_yield(void) {
    if (!gil_contended) return;
    if (!gil_quantum) {
        mach_timebase_info_data_t tb; mach_timebase_info(&tb);
        gil_quantum = 2000000ull * tb.denom / tb.numer;   // 2 ms
    }
    if (mach_absolute_time() - gil_t0 < gil_quantum) return;
    gil_unlock();
    gil_lock();
}
