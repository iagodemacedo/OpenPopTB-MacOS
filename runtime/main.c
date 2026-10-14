// Native entry point: loads the original popTB.exe into guest memory and runs the recompiled code.
#include "handles.h"
#include "table.h"
#include <unistd.h>
#include <sys/stat.h>
#include <signal.h>
#include <ctype.h>
#include <sys/mman.h>
#include <execinfo.h>
#include <mach-o/dyld.h>
#include <SDL3/SDL.h>

static void on_fault(int sig, siginfo_t *si, void *uc) {
    (void)uc;
    uintptr_t a = (uintptr_t)si->si_addr;
    fprintf(stderr, "\n[fault] signal %d address %p", sig, si->si_addr);
    if (a >= (uintptr_t)MEM && a < (uintptr_t)MEM + (1ull << 32)) fprintf(stderr, " (guest 0x%08x)", (uint32_t)(a - (uintptr_t)MEM));
    fprintf(stderr, "\n");
    fprintf(stderr, "load 0x%lx\n", (unsigned long)_dyld_get_image_header(0));
    void *bt[64]; int n = backtrace(bt, 64);
    backtrace_symbols_fd(bt, n, 2);
    _exit(128 + sig);
}

#ifndef POPTB_EXE
#define POPTB_EXE "popTB.exe"
#endif
char g_cmdline[512] = POPTB_EXE;
const char *g_exe_name = POPTB_EXE;
const char *g_shot_path, *g_shot_dir; int g_shot_frame = 60, g_shot_every;
void plat_script(const char *spec);
uint32_t g_hwnd, g_wndproc;
const FnEntry *fn_table;
int fn_table_len;
const ImportEntry *import_table;
int import_table_len;
int prep_choose_game(const char *exe, char *root, size_t n);
int prep_from_installer(const char *exe, const char *setup, char *root, size_t n);
int prep_load_game(const char *root, const char *exe);

static uint8_t *read_file(const char *p, size_t *n) {
    FILE *f = fopen(p, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); *n = (size_t)ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *b = malloc(*n);
    if (fread(b, 1, *n, f) != *n) { fclose(f); free(b); return NULL; }
    fclose(f);
    return b;
}
static uint32_t le32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }

typedef struct { char name[64]; uint32_t base, size, entry; } Module;
static Module mods[8];
static int nmods;

// copies a PE into guest memory at its preferred base (all game images have a fixed base)
static Module *load_pe(const char *path) {
    size_t n; uint8_t *b = read_file(path, &n);
    if (!b) rt_fatal("could not read %s", path);
    uint32_t pe = le32(b + 0x3c);
    if (le32(b + pe) != 0x4550) rt_fatal("%s is not a PE", path);
    uint16_t nsec = le16(b + pe + 6), optsz = le16(b + pe + 20);
    const uint8_t *opt = b + pe + 24;
    uint32_t base = le32(opt + 28), hdrsz = le32(opt + 60), isz = le32(opt + 56);
    memcpy(PTR(base), b, hdrsz);
    const uint8_t *sec = opt + optsz;
    for (int i = 0; i < nsec; i++, sec += 40) {
        uint32_t vsz = le32(sec + 8), va = le32(sec + 12), rawsz = le32(sec + 16), raw = le32(sec + 20);
        uint32_t k = rawsz < vsz || !vsz ? rawsz : vsz;
        if (raw + k > n) k = (uint32_t)(n - raw);
        memcpy(PTR(base + va), b + raw, k);
    }
    Module *m = &mods[nmods++];
    const char *bn = strrchr(path, '/'); bn = bn ? bn + 1 : path;
    snprintf(m->name, sizeof m->name, "%s", bn);
    for (char *c = m->name; *c; c++) *c = (char)tolower((unsigned char)*c);
    m->base = base; m->size = isz; m->entry = base + le32(opt + 16);
    if (base >= GUEST_HEAP_BASE && base < GUEST_HEAP_END) {
        extern uint32_t reserved_lo[], reserved_hi[]; extern int nreserved;
        reserved_lo[nreserved] = base; reserved_hi[nreserved] = base + isz; nreserved++;
    }
    if (getenv("POPTB_PROTECT_CODE")) {
        // diagnostic: read-only code sections to catch self-modifying code
        sec = opt + optsz;
        for (int i = 0; i < nsec; i++, sec += 40) {
            uint32_t vsz = le32(sec + 8), va = le32(sec + 12), ch = le32(sec + 36);
            if (ch & 0x20000000) {
                // host pages are 16 KB: protect only those fully inside the section
                uint32_t a0 = (base + va + 0x3fff) & ~0x3fffu, a1 = (base + va + vsz) & ~0x3fffu;
                if (a1 > a0) mprotect(PTR(a0), a1 - a0, PROT_READ);
            }
        }
    }
    free(b);
    return m;
}

// looks up an export by name in the image already loaded in guest memory
static uint32_t pe_export(Module *m, const char *name) {
    uint32_t pe = rd32(m->base + 0x3c);
    uint32_t dir = rd32(m->base + pe + 24 + 96);
    if (!dir) return 0;
    uint32_t ed = m->base + dir;
    uint32_t nnames = rd32(ed + 24), funcs = m->base + rd32(ed + 28), names = m->base + rd32(ed + 32), ords = m->base + rd32(ed + 36);
    for (uint32_t i = 0; i < nnames; i++)
        if (!strcmp(gstr(m->base + rd32(names + 4 * i)), name)) return m->base + rd32(funcs + 4 * rd16(ords + 2 * i));
    return 0;
}
static Module *module_by_name(const char *dll) {
    for (int i = 0; i < nmods; i++) if (!strcasecmp(mods[i].name, dll)) return &mods[i];
    return NULL;
}
uint32_t module_handle(const char *name) {
    if (!name) return GUEST_IMAGE_BASE;
    char low[128]; snprintf(low, sizeof low, "%s", name);
    for (char *c = low; *c; c++) *c = (char)tolower((unsigned char)*c);
    if (!strstr(low, ".")) strncat(low, ".dll", sizeof low - strlen(low) - 1);
    const char *bn = strrchr(low, '\\'); bn = bn ? bn + 1 : low;
    Module *m = module_by_name(bn);
    return m ? m->base : 0;
}
uint32_t module_export(uint32_t base, const char *name) {
    for (int i = 0; i < nmods; i++) if (mods[i].base == base) return pe_export(&mods[i], name);
    return 0;
}

static void bind_imports(void) {
    // imports into recompiled DLLs point at their code; the rest become native thunks
    int missing = 0;
    for (int i = 0; i < import_table_len; i++) {
        const ImportEntry *e = &import_table[i];
        uint32_t a = 0;
        Module *m = module_by_name(e->dll);
        // DirectPlay lobbies don't exist on macOS: "not lobbied" comes from the shim (0x20, as on Windows)
        int override = strstr(e->name, "AreWeLobbied") != NULL;
        if (m && !override) a = pe_export(m, e->name);
        if (!a) a = shim_addr(e->name);
        if (!a) {
            if (strstr(e->name, "@@3")) a = gmalloc(256);
            else { a = thunk_new(NULL, e->name); missing++; LOG("import without shim: %s!%s", e->dll, e->name); }
        }
        wr32(e->iat, a);
    }
    fprintf(stderr, "images: %d, imports: %d, unimplemented: %d\n", nmods, import_table_len, missing);
}

static int has_game(const char *dir) {
    char p[1200]; struct stat st;
    snprintf(p, sizeof p, "%s/%s", dir, POPTB_EXE);
    return stat(p, &st) == 0;
}
static void config_path(char *out, size_t n) {
    const char *home = getenv("HOME");
    snprintf(out, n, "%s/Library/Application Support/OpenPopTB", home ? home : ".");
}
// order: --data/POPTB_DATA, saved path, folder next to the .app, current directory, ask the user
// (folder or GOG installer)
static int find_data_dir(const char *given) {
    char cand[1100] = "";
    char cfg[1024]; config_path(cfg, sizeof cfg);
    char cf[1100]; snprintf(cf, sizeof cf, "%s/data_path.txt", cfg);
    FILE *f;
    if (given && has_game(given)) { realpath(given, g_root); goto ok; }
    f = fopen(cf, "r");
    if (f) {
        if (fgets(cand, sizeof cand, f)) { cand[strcspn(cand, "\r\n")] = 0; }
        fclose(f);
        if (cand[0] && has_game(cand)) { snprintf(g_root, sizeof g_root, "%s", cand); return 1; }
    }
    char exe[1024]; uint32_t sz = sizeof exe;
    if (_NSGetExecutablePath(exe, &sz) == 0) {
        char real[1024]; realpath(exe, real);
        // .../X.app/Contents/MacOS/bin -> .../ (folder containing the .app)
        char *p = real;
        for (int up = 0; up < 4; up++) { char *sl = strrchr(p, '/'); if (sl) *sl = 0; }
        if (has_game(real)) { snprintf(g_root, sizeof g_root, "%s", real); goto ok; }
    }
    if (has_game(".")) { realpath(".", g_root); goto ok; }
    // ask: game folder or GOG installer (runtime/prepare.c)
    if (!prep_choose_game(POPTB_EXE, cand, sizeof cand)) return 0;
    realpath(cand, g_root);
ok:
    for (char *c = cfg + 1; *c; c++) if (*c == '/') { *c = 0; mkdir(cfg, 0755); *c = '/'; }   // mkdir -p
    mkdir(cfg, 0755);
    f = fopen(cf, "w");
    if (f) { fprintf(f, "%s\n", g_root); fclose(f); }
    return 1;
}

int main(int argc, char **argv) {
    const char *data = NULL, *script_spec = NULL, *installer = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--data") && i + 1 < argc) data = argv[++i];
        else if (!strcmp(argv[i], "--installer") && i + 1 < argc) installer = argv[++i];
        else if (!strcmp(argv[i], "-v")) rt_verbose = 1;
        else if (!strcmp(argv[i], "-vv")) rt_verbose = 2;
        else if (!strcmp(argv[i], "--shot") && i + 1 < argc) g_shot_path = argv[++i];
        else if (!strcmp(argv[i], "--shot-frame") && i + 1 < argc) g_shot_frame = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--shot-dir") && i + 1 < argc) g_shot_dir = argv[++i];
        else if (!strcmp(argv[i], "--shot-every") && i + 1 < argc) g_shot_every = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--script") && i + 1 < argc) script_spec = argv[++i];
        else { strncat(g_cmdline, " ", sizeof g_cmdline - strlen(g_cmdline) - 1); strncat(g_cmdline, argv[i], sizeof g_cmdline - strlen(g_cmdline) - 1); }
    }
    if (!data) data = getenv("POPTB_DATA");
    // unpacks the GOG installer into Application Support and uses that copy from then on
    static char unpacked[1100];
    if (installer && !data) {
        if (!prep_from_installer(POPTB_EXE, installer, unpacked, sizeof unpacked)) return 1;
        data = unpacked;
    }
    if (!find_data_dir(data)) {
        fprintf(stderr, "popTB.exe not found (use --data <game folder>)\n");
        return 1;
    }
#ifdef POPTB_PREBUILT
    // prebuilt app: the game code is recompiled from the player's copy on first launch, then cached
    if (!prep_load_game(g_root, POPTB_EXE)) return 1;
#else
    fn_table = gen_fn_table; fn_table_len = gen_fn_table_len;
    import_table = gen_import_table; import_table_len = gen_import_table_len;
#endif
    char exe[1100]; snprintf(exe, sizeof exe, "%s/%s", g_root, POPTB_EXE);
    struct stat st;
    chdir(g_root);
    struct sigaction sa = {0};
    sa.sa_sigaction = on_fault; sa.sa_flags = SA_SIGINFO;
    sigaction(SIGSEGV, &sa, NULL); sigaction(SIGBUS, &sa, NULL); sigaction(SIGFPE, &sa, NULL);
    mem_init();
    plat_init();
    if (script_spec) plat_script(script_spec);
    Module *game = load_pe(exe);
    char dll[1100]; snprintf(dll, sizeof dll, "%s/QMixer.dll", g_root);
    struct stat st2;
    if (stat(dll, &st2) == 0) {
        load_pe(dll);
        // diagnostic: QMixer internal log level (writes to C:\dump.txt = <game>/dump.txt)
        if (getenv("POPTB_QMIX_DEBUG")) wr32(0x1802dfd4, (uint32_t)atoi(getenv("POPTB_QMIX_DEBUG")));
    }
    snprintf(dll, sizeof dll, "%s/WEANETR.dll", g_root);
    // multiplayer is a work in progress (docs/multiplayer.md): only loads weanetr with POPTB_NET=1
    if (stat(dll, &st2) == 0 && getenv("POPTB_NET")) load_pe(dll);
    bind_imports();
    gil_lock();
    CPU *R = cpu_new_thread();
    for (int i = 0; i < nmods; i++) if (&mods[i] != game) {
        uint32_t ok = guest_call(R, mods[i].entry, 3, mods[i].base, 1, 0);   // DllMain(DLL_PROCESS_ATTACH)
        fprintf(stderr, "DllMain(%s) -> %u\n", mods[i].name, ok);
    }
    fprintf(stderr, "entry 0x%x\n", game->entry);
    guest_call(R, game->entry, 0);
    fprintf(stderr, "entry returned eax=%x\n", R->eax);
    return (int)R->eax;
}
