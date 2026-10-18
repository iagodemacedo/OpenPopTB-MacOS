// State capture for differential replay (tools/replay.py): memory + registers on entry to and
// exit from a guest function. Enable with POPTB_SNAP_FUNC=<hex> [POPTB_SNAP_SKIP=n] POPTB_SNAP_DIR=<dir>
// and generate the code with POPTB_HOOK=<hex>.
#include "rt.h"

extern uint32_t brk_ptr, virt_ptr;
static uint32_t snap_func, snap_skip, calls, active_esp;
static const char *snap_dir;
static int done;
int trace_on;
static FILE *trace_f;
static uint64_t trace_n;

// per-instruction record: address + 8 registers (compared against the Unicorn trace in tools/replay.py)
static CPU *snap_cpu;
static int trace_all;   // POPTB_TRACE_ALL: records all threads, one file per thread
static FILE *thread_trace(CPU *R) {
    static CPU *cpus[64]; static FILE *fs[64]; static int n;
    for (int i = 0; i < n; i++) if (cpus[i] == R) return fs[i];
    if (n >= 64) return NULL;
    char p[1024]; snprintf(p, sizeof p, "%s/t%03d.trace", snap_dir, n);
    cpus[n] = R; fs[n] = fopen(p, "wb");
    return fs[n++];
}
// POPTB_TRACE_LAST=1 (with a trace-instrumented gen): keep the last executed addresses, printed on a crash
uint32_t trace_ring[16][9];
unsigned trace_ring_n; int trace_ring_on;
void trace_rec(CPU *R, uint32_t a) {
    if (trace_ring_on) {
        uint32_t *r = trace_ring[trace_ring_n++ & 15];
        r[0] = a; r[1] = R->eax; r[2] = R->ecx; r[3] = R->edx; r[4] = R->ebx; r[5] = R->esp; r[6] = R->ebp; r[7] = R->esi; r[8] = R->edi;
        return;
    }
    if (trace_all) {
        FILE *f = thread_trace(R);
        if (!f) return;
        uint32_t r[9] = {a, R->eax, R->ecx, R->edx, R->ebx, R->esp, R->ebp, R->esi, R->edi};
        fwrite(r, 4, 9, f);
        return;
    }
    if (R != snap_cpu) return;   // only the captured thread
    if (trace_n++ > 30000000) { trace_on = 0; return; }
    uint32_t r[9] = {a, R->eax, R->ecx, R->edx, R->ebx, R->esp, R->ebp, R->esi, R->edi};
    fwrite(r, 4, 9, trace_f);
}

static void dump(CPU *R, const char *tag) {
    char p[1024]; snprintf(p, sizeof p, "%s/%s.snap", snap_dir, tag);
    FILE *f = fopen(p, "wb");
    uint32_t regs[12] = {R->eax, R->ecx, R->edx, R->ebx, R->esp, R->ebp, R->esi, R->edi, get_flags(R) | (R->df << 10) | 2, R->fs_base, R->fcw, R->ftop};
    fwrite(regs, 4, 12, f);
    fwrite(R->st, 8, 8, f);
    uint32_t pe = rd32(GUEST_IMAGE_BASE + 0x3c), isz = rd32(GUEST_IMAGE_BASE + pe + 24 + 56);
    uint32_t regions[3][2] = {{GUEST_IMAGE_BASE, isz}, {GUEST_HEAP_BASE, brk_ptr - GUEST_HEAP_BASE}, {GUEST_VIRT_BASE, virt_ptr - GUEST_VIRT_BASE}};
    uint32_t n = 3; fwrite(&n, 4, 1, f);
    for (int i = 0; i < 3; i++) { fwrite(regions[i], 4, 2, f); fwrite(PTR(regions[i][0]), 1, regions[i][1], f); }
    fclose(f);
    fprintf(stderr, "[snap] %s: esp=%x heap=%u KB virt=%u KB\n", p, R->esp, (brk_ptr - GUEST_HEAP_BASE) >> 10, (virt_ptr - GUEST_VIRT_BASE) >> 10);
}

void hook_enter(CPU *R, uint32_t fn) {
    if (getenv("POPTB_HOOK_LOG")) {
        static uint32_t last_fn, rep;
        if (fn == last_fn && ++rep > 3) { if (rep == 4) fprintf(stderr, "[hook] ... repeats of %x\n", fn); }
        else { if (fn != last_fn) rep = 0; fprintf(stderr, "[hook %u] %x(%x, %x, %x, %x)\n", now_ms() % 100000, fn, rd32(R->esp + 4), rd32(R->esp + 8), rd32(R->esp + 12), rd32(R->esp + 16)); }
        last_fn = fn;
    }
    if (done || !snap_func) {
        if (!snap_func && getenv("POPTB_SNAP_FUNC")) {
            snap_func = (uint32_t)strtoul(getenv("POPTB_SNAP_FUNC"), NULL, 16);
            snap_skip = getenv("POPTB_SNAP_SKIP") ? (uint32_t)atoi(getenv("POPTB_SNAP_SKIP")) : 0;
            snap_dir = getenv("POPTB_SNAP_DIR") ? getenv("POPTB_SNAP_DIR") : ".";
        }
        if (done || !snap_func) return;
    }
    if (fn != snap_func || active_esp) return;
    if (calls++ < snap_skip) return;
    active_esp = R->esp;
    snap_cpu = R;
    dump(R, "enter");
    if (getenv("POPTB_TRACE_ALL")) { trace_all = 1; trace_on = 1; done = 1; active_esp = 0; fprintf(stderr, "[snap] tracing all threads\n"); return; }
    if (getenv("POPTB_SNAP_TRACE")) {
        char p[1024]; snprintf(p, sizeof p, "%s/c.trace", snap_dir);
        trace_f = fopen(p, "wb"); trace_on = 1;
    }
}
void hook_leave(CPU *R, uint32_t fn) {
    if (getenv("POPTB_HOOK_LOG")) fprintf(stderr, "[hook %u] %x -> eax=%x\n", now_ms() % 100000, fn, R->eax);
    if (fn != snap_func || !active_esp || R->esp <= active_esp) return;
    dump(R, "leave");
    done = 1; active_esp = 0;
    if (trace_f) { trace_on = 0; fclose(trace_f); trace_f = NULL; fprintf(stderr, "[snap] trace: %llu instructions\n", (unsigned long long)trace_n); }
}
