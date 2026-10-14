// First-launch preparation for the prebuilt app: find the game (folder or GOG installer), then
// recompile the player's own popTB.exe into a bundle that is cached and loaded with dlopen.
// The app itself ships no game-derived code; everything game-specific is produced here, locally.
#include "table.h"
#include <stdio.h>
#include <stdlib.h>
#include <SDL3/SDL.h>
#include <CommonCrypto/CommonDigest.h>
#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <sys/wait.h>
#include <unistd.h>
#include <mach-o/dyld.h>

#ifndef POPTB_GEN_ABI
#define POPTB_GEN_ABI "dev"
#endif

extern char **environ;

// ---- small helpers ----
static void app_support(char *out, size_t n) {
    const char *home = getenv("HOME");
    snprintf(out, n, "%s/Library/Application Support/OpenPopTB", home ? home : ".");
}
static int exists(const char *p) { struct stat st; return stat(p, &st) == 0; }
static void exe_dir(char *out, size_t n) {
    char exe[1024]; uint32_t sz = sizeof exe;
    out[0] = 0;
    if (_NSGetExecutablePath(exe, &sz) != 0) return;
    char real[1024];
    if (!realpath(exe, real)) return;
    char *sl = strrchr(real, '/'); if (sl) *sl = 0;
    snprintf(out, n, "%s", real);
}

static int sha256_file(const char *path, char hex[65]) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    CC_SHA256_CTX c; CC_SHA256_Init(&c);
    static uint8_t buf[1 << 16]; size_t k;
    while ((k = fread(buf, 1, sizeof buf, f)) > 0) CC_SHA256_Update(&c, buf, (CC_LONG)k);
    fclose(f);
    uint8_t d[32]; CC_SHA256_Final(d, &c);
    for (int i = 0; i < 32; i++) sprintf(hex + 2 * i, "%02x", d[i]);
    return 1;
}

// ---- progress window ----
static SDL_Window *pw;
static SDL_Renderer *pr;
static char p_title[128], p_step[160], p_detail[160];
static float p_frac = -1;   // < 0: indeterminate
static int p_quit;

static void ui_open(void) {
    if (pw) return;
    SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS);
    pw = SDL_CreateWindow("Populous: The Beginning", 720, 220, SDL_WINDOW_HIGH_PIXEL_DENSITY);
    pr = SDL_CreateRenderer(pw, NULL);
    SDL_SetRenderVSync(pr, 1);
}
static void ui_close(void) {
    if (pr) SDL_DestroyRenderer(pr);
    if (pw) SDL_DestroyWindow(pw);
    pr = NULL; pw = NULL;
}
static void ui_frame(void) {
    SDL_Event e;
    while (SDL_PollEvent(&e)) if (e.type == SDL_EVENT_QUIT || e.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) p_quit = 1;
    if (!pr) return;
    SDL_SetRenderScale(pr, 1, 1);
    SDL_SetRenderDrawColor(pr, 18, 22, 34, 255);
    SDL_RenderClear(pr);
    SDL_SetRenderScale(pr, 2, 2);
    SDL_SetRenderDrawColor(pr, 240, 220, 150, 255);
    SDL_RenderDebugText(pr, 16, 14, p_title);
    SDL_SetRenderDrawColor(pr, 220, 220, 220, 255);
    SDL_RenderDebugText(pr, 16, 36, p_step);
    SDL_SetRenderDrawColor(pr, 150, 150, 160, 255);
    SDL_RenderDebugText(pr, 16, 90, p_detail);
    SDL_SetRenderScale(pr, 1, 1);
    SDL_FRect bar = {32, 120, 656, 22};
    SDL_SetRenderDrawColor(pr, 60, 66, 84, 255);
    SDL_RenderFillRect(pr, &bar);
    SDL_SetRenderDrawColor(pr, 230, 180, 70, 255);
    SDL_FRect fill = bar;
    if (p_frac >= 0) fill.w = bar.w * (p_frac > 1 ? 1 : p_frac);
    else {   // indeterminate: a block sweeping across the bar
        float t = (float)(SDL_GetTicks() % 1600) / 1600.f;
        fill.w = bar.w * 0.2f; fill.x = bar.x + (bar.w - fill.w) * (t < 0.5f ? t * 2 : 2 - t * 2);
    }
    SDL_RenderFillRect(pr, &fill);
    SDL_RenderPresent(pr);
}

static void msg(SDL_MessageBoxFlags kind, const char *text) {
    SDL_ShowSimpleMessageBox(kind, "Populous: The Beginning", text, pw);
}
// returns the index of the button clicked (buttons left to right), -1 if closed
static int ask(const char *text, int n, const char **buttons) {
    SDL_MessageBoxButtonData b[4];
    for (int i = 0; i < n; i++) {
        b[i].buttonID = i; b[i].text = buttons[i];
        b[i].flags = i == 0 ? SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT : i == n - 1 ? SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT : 0;
    }
    SDL_MessageBoxData d = {SDL_MESSAGEBOX_INFORMATION, pw, "Populous: The Beginning", text, n, b, NULL};
    int r = -1;
    if (!SDL_ShowMessageBox(&d, &r)) return -1;
    return r;
}

// ---- child processes ----
static pid_t spawn(char *const argv[], const char *log) {
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
    if (log) {
        posix_spawn_file_actions_addopen(&fa, 1, log, O_WRONLY | O_CREAT | O_APPEND, 0644);
        posix_spawn_file_actions_adddup2(&fa, 1, 2);
    }
    pid_t pid;
    int e = posix_spawn(&pid, argv[0], &fa, NULL, argv, environ);
    posix_spawn_file_actions_destroy(&fa);
    if (e) { errno = e; return -1; }
    return pid;
}
// runs a command to completion while keeping the window alive; returns the exit status (-1 on error/quit)
static int run_ui(char *const argv[], const char *log) {
    if (log) { FILE *f = fopen(log, "a"); if (f) { fprintf(f, "\n$"); for (int i = 0; argv[i]; i++) fprintf(f, " %s", argv[i]); fputc('\n', f); fclose(f); } }
    pid_t pid = spawn(argv, log);
    if (pid < 0) return -1;
    for (;;) {
        int st;
        pid_t r = waitpid(pid, &st, WNOHANG);
        if (r == pid) return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
        if (r < 0) return -1;
        ui_frame();
        if (p_quit) { kill(pid, SIGTERM); waitpid(pid, &st, 0); return -1; }
        if (!pr) SDL_Delay(30);
    }
}
// quiet check: does the command run and succeed?
static int run_quiet(char *const argv[]) {
    pid_t pid = spawn(argv, "/dev/null");
    if (pid < 0) return 0;
    int st; waitpid(pid, &st, 0);
    return WIFEXITED(st) && WEXITSTATUS(st) == 0;
}
static int rm_rf(const char *p) {
    if (!exists(p)) return 1;
    char *argv[] = {"/bin/rm", "-rf", (char *)p, NULL};
    return run_quiet(argv);
}
// copies a file tree and drops the quarantine flag from the copy: tools and libraries from the
// download would otherwise be refused by Gatekeeper ("disallowed by system policy") even after the
// player approved the app (ditto --noqtn does not do it for plain copies)
static int copy_tree(const char *from, const char *to) {
    char *cp[] = {"/usr/bin/ditto", (char *)from, (char *)to, NULL};
    char *xa[] = {"/usr/bin/xattr", "-dr", "com.apple.quarantine", (char *)to, NULL};
    if (!run_quiet(cp)) return 0;
    run_quiet(xa);   // fails harmlessly when nothing is quarantined
    return 1;
}

// One preparation at a time across all instances (TB and UW share Game/, tools/ and cache/):
// a second app opened meanwhile waits here, then finds the work already done.
static int lock_fd = -1;
static void prep_lock(void) {
    char as[1024]; app_support(as, sizeof as);
    mkdir(as, 0755);
    char p[1100]; snprintf(p, sizeof p, "%s/prepare.lock", as);
    lock_fd = open(p, O_RDWR | O_CREAT, 0644);
    if (lock_fd >= 0) flock(lock_fd, LOCK_EX);
}
static void prep_unlock(void) {
    if (lock_fd >= 0) { flock(lock_fd, LOCK_UN); close(lock_fd); }
    lock_fd = -1;
}

// ---- 1. finding the game ----
static int has_exe(const char *dir, const char *exe) {
    char p[1200]; snprintf(p, sizeof p, "%s/%s", dir, exe);
    return exists(p);
}
// looks for the folder that contains exe, up to two levels below dir (installers use an app/ prefix)
static int find_exe_below(const char *dir, const char *exe, char *out, size_t n, int depth) {
    if (has_exe(dir, exe)) { snprintf(out, n, "%s", dir); return 1; }
    if (depth == 0) return 0;
    DIR *d = opendir(dir);
    if (!d) return 0;
    struct dirent *e; int ok = 0;
    while (!ok && (e = readdir(d))) {
        if (e->d_name[0] == '.' || e->d_type != DT_DIR) continue;
        char sub[1200]; snprintf(sub, sizeof sub, "%s/%s", dir, e->d_name);
        ok = find_exe_below(sub, exe, out, n, depth - 1);
    }
    closedir(d);
    return ok;
}

static int osascript_choose(const char *what, char *out, size_t n) {
    char cmd[512];
    snprintf(cmd, sizeof cmd, "osascript -e 'POSIX path of (%s)' 2>/dev/null", what);
    FILE *pp = popen(cmd, "r");
    if (!pp) return 0;
    out[0] = 0;
    if (fgets(out, (int)n, pp)) out[strcspn(out, "\r\n")] = 0;
    pclose(pp);
    size_t l = strlen(out); if (l > 1 && out[l - 1] == '/') out[l - 1] = 0;
    return out[0] != 0;
}

static int innoextract_path(char *out, size_t n) {
    char d[1024]; exe_dir(d, sizeof d);
    char p[1200]; snprintf(p, sizeof p, "%s/innoextract", d);
    if (exists(p)) {
        // bundled copy: run it from Application Support, without the quarantine flag (see copy_tree)
        char as[1024]; app_support(as, sizeof as);
        char tools[1100]; snprintf(tools, sizeof tools, "%s/tools", as);
        char dst[1200], fw[1200], dfw[1200];
        snprintf(dst, sizeof dst, "%s/MacOS/innoextract", tools);
        snprintf(fw, sizeof fw, "%s/../Frameworks", d);
        snprintf(dfw, sizeof dfw, "%s/Frameworks", tools);
        rm_rf(tools);
        if (copy_tree(p, dst) && copy_tree(fw, dfw)) { snprintf(out, n, "%s", dst); return 1; }
        return 0;
    }
    const char *sys[] = {"/opt/homebrew/bin/innoextract", "/usr/local/bin/innoextract"};
    for (int i = 0; i < 2; i++) if (exists(sys[i])) { snprintf(out, n, "%s", sys[i]); return 1; }
    return 0;
}

// moves the game's save folder (any case) from the old copy into the new one
static void keep_saves(const char *old_root, const char *new_root) {
    DIR *d = opendir(old_root);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_type != DT_DIR || strcasecmp(e->d_name, "save")) continue;
        char from[1300], to[1300];
        snprintf(from, sizeof from, "%s/%s", old_root, e->d_name);
        snprintf(to, sizeof to, "%s/%s", new_root, e->d_name);
        rm_rf(to);
        rename(from, to);
    }
    closedir(d);
}

static int extract_installer_locked(const char *setup, const char *exe, char *root, size_t n) {
    char tool[1200];
    if (!innoextract_path(tool, sizeof tool)) {
        msg(SDL_MESSAGEBOX_ERROR, "innoextract was not found, so the installer cannot be unpacked.\n"
            "Extract it yourself (brew install innoextract) and choose the folder instead.");
        return 0;
    }
    char as[1024]; app_support(as, sizeof as);
    char dest[1100], tmp[1100], log[1100];
    snprintf(dest, sizeof dest, "%s/Game", as);
    snprintf(tmp, sizeof tmp, "%s/Game.tmp", as);
    snprintf(log, sizeof log, "%s/extract.log", as);
    remove(log);
    // unpack next to the current copy and swap only on success: a failed or cancelled extraction
    // leaves the previous copy (and its saves) untouched
    rm_rf(tmp);
    mkdir(tmp, 0755);
    ui_open();
    snprintf(p_title, sizeof p_title, "Unpacking the GOG installer");
    snprintf(p_step, sizeof p_step, "This takes about a minute.");
    const char *bn = strrchr(setup, '/');
    snprintf(p_detail, sizeof p_detail, "%.70s", bn ? bn + 1 : setup);
    p_frac = -1;
    // --gog also reads the .bin parts next to the .exe; -p 0: no progress bar in the log
    char *argv[] = {tool, "--gog", "-p", "0", "-d", tmp, (char *)setup, NULL};
    int st = run_ui(argv, log);
    char found[1300];
    if (p_quit) { rm_rf(tmp); return 0; }
    if (st != 0 || !find_exe_below(tmp, exe, found, sizeof found, 2)) {
        rm_rf(tmp);
        char m[1400];
        snprintf(m, sizeof m, "Could not unpack the installer (%s not found after extraction).\nDetails: %s", exe, log);
        msg(SDL_MESSAGEBOX_ERROR, m);
        return 0;
    }
    char old_root[1300];
    if (find_exe_below(dest, exe, old_root, sizeof old_root, 2)) {
        // the game writes its saves into its own folder: carry them over
        keep_saves(old_root, found);
    }
    rm_rf(dest);
    if (rename(tmp, dest) != 0) { msg(SDL_MESSAGEBOX_ERROR, "Could not move the unpacked game into place."); return 0; }
    snprintf(root, n, "%s%s", dest, found + strlen(tmp));
    return 1;
}
static int extract_installer(const char *setup, const char *exe, char *root, size_t n) {
    prep_lock();
    int ok = extract_installer_locked(setup, exe, root, n);
    prep_unlock();
    return ok;
}

// --installer <setup.exe>: same as choosing the installer in the dialog
int prep_from_installer(const char *exe, const char *setup, char *root, size_t n) {
    SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS);
    int ok = extract_installer(setup, exe, root, n);
    ui_close();
    return ok;
}

// asks where the game is: an installed/extracted folder or the GOG offline installer
int prep_choose_game(const char *exe, char *root, size_t n) {
    SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS);
    for (;;) {
        const char *b[] = {"Choose game folder", "Choose GOG installer", "Quit"};
        char text[512];
        snprintf(text, sizeof text,
            "Where is your copy of Populous: The Beginning?\n\n"
            "- Game folder: a folder that contains %s (GOG Galaxy, CrossOver, a Windows copy, "
            "or an installer you already extracted).\n"
            "- GOG installer: the offline installer setup_populous_the_beginning_*.exe "
            "(keep any .bin files next to it).", exe);
        int r = ask(text, 3, b);
        char pick[1100];
        if (r == 0) {
            if (!osascript_choose("choose folder with prompt \"Choose the Populous: The Beginning folder\"", pick, sizeof pick)) continue;
            if (find_exe_below(pick, exe, root, n, 2)) return 1;
            char m[1300]; snprintf(m, sizeof m, "%s was not found in:\n%s", exe, pick);
            msg(SDL_MESSAGEBOX_WARNING, m);
        } else if (r == 1) {
            if (!osascript_choose("choose file with prompt \"Choose the GOG installer (setup_populous_the_beginning_*.exe)\" of type {\"exe\", \"com.microsoft.windows-executable\"}", pick, sizeof pick)) continue;
            int ok = extract_installer(pick, exe, root, n);
            ui_close();
            if (ok) return 1;
            if (p_quit) return 0;
        } else return 0;
    }
}

// ---- 2. recompiling the game ----
static const struct { const char *file, *sha; } known[] = {
    {"popTB.exe", "a46e7c6fa04e13f6cd5accebe32bb5f543f1cd0808bb96b0040f59763251109e"},
    {"popTBUW.exe", "d994142ce0ffb0e5a12ad432cd5def2020d3ca9fb6027a1469d9f7459071e760"},
};

static int have_dev_tools(void) {
    // xcode-select -p fails without a developer directory; plain `clang` would pop the installer
    char *a[] = {"/usr/bin/xcode-select", "-p", NULL};
    char *b[] = {"/usr/bin/xcrun", "--find", "clang", NULL};
    char *c[] = {"/usr/bin/python3", "-c", "", NULL};
    return run_quiet(a) && run_quiet(b) && run_quiet(c);
}

static int ncpu(void) {
    int n = 0; size_t sz = sizeof n;
    if (sysctlbyname("hw.perflevel0.physicalcpu", &n, &sz, NULL, 0) != 0 || n <= 0)
        if (sysctlbyname("hw.ncpu", &n, &sz, NULL, 0) != 0 || n <= 0) n = 4;
    return n;
}

static int cmp_str(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

// compiles every .c in src into obj/, several at a time, then links them into a bundle
static int compile_all(const char *res, const char *src, const char *obj, const char *out, const char *log) {
    DIR *d = opendir(src);
    if (!d) return 0;
    char *files[256]; int nf = 0;
    struct dirent *e;
    while ((e = readdir(d)) && nf < 256) {
        size_t l = strlen(e->d_name);
        if (l > 2 && !strcmp(e->d_name + l - 2, ".c")) files[nf++] = strdup(e->d_name);
    }
    closedir(d);
    qsort(files, nf, sizeof *files, cmp_str);
    mkdir(obj, 0755);
    char inc_rt[1100], inc_gen[1100];
    snprintf(inc_rt, sizeof inc_rt, "-I%s/include", res);
    snprintf(inc_gen, sizeof inc_gen, "-I%s", src);
    int jobs = ncpu(), next = 0, done = 0, running = 0, failed = 0;
    pid_t pids[64] = {0};
    if (jobs > 64) jobs = 64;
    while ((done < nf) && !failed && !p_quit) {
        while (running < jobs && next < nf) {
            char in[1300], o[1300];
            snprintf(in, sizeof in, "%s/%s", src, files[next]);
            snprintf(o, sizeof o, "%s/%s.o", obj, files[next]);
            // same flags as the from-source build (CMakeLists.txt, generated code)
            char *argv[] = {"/usr/bin/xcrun", "clang", "-c", "-arch", "arm64", "-mmacosx-version-min=12.0",
                "-std=gnu11", "-O1", "-w", "-fno-strict-aliasing", "-fwrapv", inc_rt, inc_gen, in, "-o", o, NULL};
            pid_t pid = spawn(argv, log);
            if (pid < 0) { failed = 1; break; }
            for (int k = 0; k < jobs; k++) if (!pids[k]) { pids[k] = pid; break; }
            running++; next++;
        }
        int st;
        pid_t r = waitpid(-1, &st, WNOHANG);
        if (r > 0) {
            for (int k = 0; k < jobs; k++) if (pids[k] == r) pids[k] = 0;
            running--; done++;
            if (!WIFEXITED(st) || WEXITSTATUS(st) != 0) failed = 1;
            p_frac = 0.1f + 0.85f * (float)done / (float)nf;
            snprintf(p_detail, sizeof p_detail, "%d of %d files", done, nf);
        } else {
            ui_frame();
            if (!pr) SDL_Delay(20);
        }
    }
    for (int k = 0; k < jobs; k++) if (pids[k]) { int st; kill(pids[k], SIGTERM); waitpid(pids[k], &st, 0); }
    if (failed || p_quit) { for (int i = 0; i < nf; i++) free(files[i]); return 0; }
    // link: symbols the generated code uses (MEM, dispatch, ...) resolve against this executable
    char exe[1024]; uint32_t sz = sizeof exe;
    _NSGetExecutablePath(exe, &sz);
    char loader[1100]; realpath(exe, loader);
    char *argv[300]; int a = 0;
    argv[a++] = "/usr/bin/xcrun"; argv[a++] = "clang"; argv[a++] = "-bundle"; argv[a++] = "-arch"; argv[a++] = "arm64";
    argv[a++] = "-mmacosx-version-min=12.0"; argv[a++] = "-bundle_loader"; argv[a++] = loader;
    argv[a++] = "-o"; argv[a++] = (char *)out;
    char *objs[256];
    for (int i = 0; i < nf; i++) {
        objs[i] = malloc(1300);
        snprintf(objs[i], 1300, "%s/%s.o", obj, files[i]);
        argv[a++] = objs[i];
    }
    argv[a] = NULL;
    snprintf(p_detail, sizeof p_detail, "Linking");
    int st = run_ui(argv, log);
    for (int i = 0; i < nf; i++) { free(objs[i]); free(files[i]); }
    return st == 0;
}

static int build_game(const char *root, const char *exe, const char *dir, const char *bundle) {
    char res[1024];
    const char *base = SDL_GetBasePath();   // Contents/Resources inside the .app
    snprintf(res, sizeof res, "%s", base ? base : ".");
    size_t rl = strlen(res); if (rl > 1 && res[rl - 1] == '/') res[rl - 1] = 0;
    char tools[1100], src[1100], obj[1100], log[1100], tmp[1100], gen[1200], img[1200], seeds[1200];
    snprintf(tools, sizeof tools, "%s/recomp", dir);
    snprintf(src, sizeof src, "%s/src", dir);
    snprintf(obj, sizeof obj, "%s/obj", dir);
    snprintf(log, sizeof log, "%s/build.log", dir);
    snprintf(tmp, sizeof tmp, "%s.tmp", bundle);
    rm_rf(dir);
    char as[1024]; app_support(as, sizeof as);
    char cache[1100]; snprintf(cache, sizeof cache, "%s/cache", as);
    mkdir(as, 0755); mkdir(cache, 0755); mkdir(dir, 0755);
    char rsrc[1100]; snprintf(rsrc, sizeof rsrc, "%s/recomp", res);
    if (!copy_tree(rsrc, tools)) { msg(SDL_MESSAGEBOX_ERROR, "The app is incomplete (Resources/recomp missing). Download it again."); return 0; }

    ui_open();
    snprintf(p_title, sizeof p_title, "Preparing the game (first launch)");
    snprintf(p_step, sizeof p_step, "Step 1 of 2: translating %s", exe);
    snprintf(p_detail, sizeof p_detail, "About 1 minute. Only needed once.");
    p_frac = -1;
    // the generator: the recompiler's Python, run by the Command Line Tools' python3, with capstone
    // and pefile shipped next to it (gen.py adds its own folder to sys.path)
    char spec[1300], qm[1200], wn[1200];
    snprintf(gen, sizeof gen, "%s/gen.py", tools);
    snprintf(img, sizeof img, "%s/%s", root, exe);
    snprintf(seeds, sizeof seeds, "%s/seeds/%s.tsv", tools, exe);
    if (exists(seeds)) snprintf(spec, sizeof spec, "%s:%s", img, seeds); else snprintf(spec, sizeof spec, "%s", img);
    snprintf(qm, sizeof qm, "%s/QMixer.dll", root);
    snprintf(wn, sizeof wn, "%s/WEANETR.dll", root);
    char *argv[8]; int a = 0;
    argv[a++] = "/usr/bin/python3"; argv[a++] = "-I"; argv[a++] = gen; argv[a++] = src; argv[a++] = spec;
    if (exists(qm)) argv[a++] = qm;
    if (exists(wn)) argv[a++] = wn;
    argv[a] = NULL;
    int st = run_ui(argv, log);
    if (p_quit) return 0;
    if (st != 0) goto fail;

    snprintf(p_step, sizeof p_step, "Step 2 of 2: compiling for this Mac");
    p_frac = 0.1f;
    if (!compile_all(res, src, obj, tmp, log)) { if (p_quit) return 0; goto fail; }
    if (rename(tmp, bundle) != 0) goto fail;
    rm_rf(obj); rm_rf(src); rm_rf(tools);
    p_frac = 1;
    ui_frame();
    return 1;
fail:;
    char m[1400];
    snprintf(m, sizeof m, "Preparing the game failed.\n\nThe log is in:\n%s\n\nPlease attach it to a bug report.", log);
    msg(SDL_MESSAGEBOX_ERROR, m);
    return 0;
}

// removes bundles built for other versions of this executable or of the app
static void prune_cache(const char *exe, const char *keep) {
    char as[1024]; app_support(as, sizeof as);
    char cache[1100]; snprintf(cache, sizeof cache, "%s/cache", as);
    DIR *d = opendir(cache);
    if (!d) return;
    size_t el = strlen(exe);
    struct dirent *e;
    while ((e = readdir(d))) {
        if (strncmp(e->d_name, exe, el) || e->d_name[el] != '-' || !strcmp(e->d_name, keep)) continue;
        char p[1200]; snprintf(p, sizeof p, "%s/%s", cache, e->d_name);
        rm_rf(p);
    }
    closedir(d);
}

// makes sure the recompiled code for root/exe exists and loads it (sets fn_table, import_table)
int prep_load_game(const char *root, const char *exe) {
    char h_exe[65], h_qm[65] = "-", h_wn[65] = "-", p[1200];
    snprintf(p, sizeof p, "%s/%s", root, exe);
    if (!sha256_file(p, h_exe)) return 0;
    snprintf(p, sizeof p, "%s/QMixer.dll", root); sha256_file(p, h_qm);
    snprintf(p, sizeof p, "%s/WEANETR.dll", root); sha256_file(p, h_wn);

    // cache key: every input of the generator plus the generator/runtime version
    char key_src[400], key[65];
    snprintf(key_src, sizeof key_src, "%s|%s|%s|%s", h_exe, h_qm, h_wn, POPTB_GEN_ABI);
    uint8_t d[32]; CC_SHA256(key_src, (CC_LONG)strlen(key_src), d);
    for (int i = 0; i < 32; i++) sprintf(key + 2 * i, "%02x", d[i]);
    char name[128]; snprintf(name, sizeof name, "%s-%.16s", exe, key);
    char as[1024]; app_support(as, sizeof as);
    char dir[1300], bundle[1400];
    snprintf(dir, sizeof dir, "%s/cache/%s", as, name);
    snprintf(bundle, sizeof bundle, "%s/game.bundle", dir);

    if (!exists(bundle)) prep_lock();   // re-checked below: another instance may have just built it
    if (!exists(bundle)) {
        SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS);
        for (size_t i = 0; i < sizeof known / sizeof *known; i++) {
            if (strcasecmp(known[i].file, exe) || !strcmp(known[i].sha, h_exe)) continue;
            const char *b[] = {"Continue", "Quit"};
            char m[600];
            snprintf(m, sizeof m, "Your %s is not the tested version (GOG 1.01).\n\n"
                "It may work, but the port has only been tested with the GOG files. Continue?", exe);
            if (ask(m, 2, b) != 0) { prep_unlock(); return 0; }
        }
        if (!have_dev_tools()) {
            const char *b[] = {"Install", "Quit"};
            int r = ask("To prepare the game the first time, this app needs Apple's Command Line Tools "
                "(free, from Apple), and they are missing or not usable.\n\nClick Install, wait until macOS "
                "finishes installing them, then open the app again.\n\nIf they are already installed, check "
                "in Terminal that `xcrun clang --version` and `python3 --version` work (with Xcode selected, "
                "its license must be accepted: sudo xcodebuild -license).", 2, b);
            if (r == 0) { char *argv[] = {"/usr/bin/xcode-select", "--install", NULL}; run_quiet(argv); }
            prep_unlock();
            return 0;
        }
        if (!build_game(root, exe, dir, bundle)) { ui_close(); prep_unlock(); return 0; }
        ui_close();
        prune_cache(exe, name);   // only after a rebuild, under the lock
    }
    prep_unlock();

    void *h = dlopen(bundle, RTLD_NOW | RTLD_LOCAL);
    if (!h) {
        // a damaged cache: drop it so the next launch rebuilds
        char m[1600]; snprintf(m, sizeof m, "Could not load the prepared game:\n%s\n\nOpen the app again to rebuild it.", dlerror());
        rm_rf(dir);
        msg(SDL_MESSAGEBOX_ERROR, m);
        return 0;
    }
    const int *fl = dlsym(h, "gen_fn_table_len"), *il = dlsym(h, "gen_import_table_len");
    fn_table = dlsym(h, "gen_fn_table");
    import_table = dlsym(h, "gen_import_table");
    if (!fn_table || !import_table || !fl || !il) { rm_rf(dir); msg(SDL_MESSAGEBOX_ERROR, "The prepared game is incomplete. Open the app again to rebuild it."); return 0; }
    fn_table_len = *fl; import_table_len = *il;
    return 1;
}
