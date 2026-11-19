// Diagnostic: resource high-water marks, enabled with POPTB_STATS=1 (prints to stderr every 10 s
// and at exit). Sizes the WebAssembly port's guest memory window and thread stacks
// (docs/wasm/PLAN.md, B-2).
// - native stack: deepest C stack seen per thread, sampled in dispatch() (every indirect call and
//   every Win32/DirectX call), so slightly below the true maximum (leaf direct calls are missed)
// - guest stack: deepest guest esp per thread at the same points
// - guest heap (gmalloc bump pointer) and VirtualAlloc region high-water marks
// - GIL acquisitions (thread switches)
#include "rt.h"
#include <pthread.h>
#include <unistd.h>

int stats_on;
uint64_t stats_gil_locks;

typedef struct {
    char kind[16];
    uintptr_t top, min_sp;          // native stack
    uint32_t guest_hi, guest_min;   // guest stack
    uint64_t samples;
} TStat;
#define MAX_T 128
static TStat st[MAX_T];
static int nst;
static pthread_mutex_t st_mu = PTHREAD_MUTEX_INITIALIZER;
static __thread TStat *me;
static uint32_t t0;

void stats_thread_start(uint32_t guest_hi) {
    if (!stats_on) return;
    pthread_mutex_lock(&st_mu);
    if (nst < MAX_T) {
        me = &st[nst++];
        snprintf(me->kind, sizeof me->kind, "%s", nst == 1 ? "main" : "thread");
        me->top = me->min_sp = (uintptr_t)__builtin_frame_address(0);
        me->guest_hi = me->guest_min = guest_hi;
    }
    pthread_mutex_unlock(&st_mu);
}

void stats_thread_label(const char *kind) { if (me) snprintf(me->kind, sizeof me->kind, "%s", kind); }

void stats_sample(CPU *R) {
    TStat *s = me;
    if (!s) return;
    uintptr_t sp = (uintptr_t)__builtin_frame_address(0);
    if (sp < s->min_sp) s->min_sp = sp;
    // ignore esp values outside this thread's guest stack (e.g. a temporarily switched stack)
    if (R->esp < s->guest_min && s->guest_hi - R->esp < (64u << 20)) s->guest_min = R->esp;
    s->samples++;
}

void stats_report(void) {
    if (!stats_on) return;
    extern uint32_t brk_ptr, virt_ptr;
    uint32_t t = now_ms() - t0;
    fprintf(stderr, "[stats] t=%us heap_brk=0x%08x (%.1f MB above 0x%08x) virt=%.1f MB gil_locks=%llu threads=%d\n",
            t / 1000, brk_ptr, (brk_ptr - GUEST_HEAP_BASE) / 1048576.0, GUEST_HEAP_BASE,
            (virt_ptr - GUEST_VIRT_BASE) / 1048576.0, (unsigned long long)stats_gil_locks, nst);
    uintptr_t worst = 0;
    for (int i = 0; i < nst; i++) {
        TStat *s = &st[i];
        uintptr_t nat = s->top - s->min_sp;
        if (nat > worst) worst = nat;
        fprintf(stderr, "[stats]   thread %2d %-8s native stack %6.1f KB  guest stack %6.1f KB  samples %llu\n",
                i, s->kind, nat / 1024.0, (s->guest_hi - s->guest_min) / 1024.0, (unsigned long long)s->samples);
    }
    fprintf(stderr, "[stats]   max native stack %.1f KB\n", worst / 1024.0);
}

static void *reporter(void *arg) {
    (void)arg;
    for (;;) { sleep(10); stats_report(); }
    return NULL;
}

void stats_init(void) {
    const char *e = getenv("POPTB_STATS");
    if (!e || !*e || *e == '0') return;
    stats_on = 1;
    t0 = now_ms();
    atexit(stats_report);
    pthread_t th;
    pthread_create(&th, NULL, reporter, NULL);
    pthread_detach(th);
}
