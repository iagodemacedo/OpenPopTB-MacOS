// Win32 files on top of POSIX. "C:\Populous\..." paths point at the game folder.
#include "handles.h"
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <fnmatch.h>
#include <sys/stat.h>
#include <errno.h>
#include <ctype.h>
#include <glob.h>

char g_root[1024];
char g_save[1024];
static char g_cwd[512] = "C:\\Populous";

// resolves component by component, case-insensitively
static void ci_resolve(char *p) {
    struct stat st;
    if (stat(p, &st) == 0) return;
    char out[2048] = "";
    char tmp[2048]; strncpy(tmp, p, sizeof tmp - 1); tmp[sizeof tmp - 1] = 0;
    char *save = NULL;
    int abs = tmp[0] == '/';
    if (abs) strcpy(out, "");
    for (char *c = strtok_r(tmp, "/", &save); c; c = strtok_r(NULL, "/", &save)) {
        char cand[2048];
        snprintf(cand, sizeof cand, "%s/%s", out, c);
        if (stat(cand, &st) != 0) {
            DIR *d = opendir(out[0] ? out : "/");
            if (d) {
                struct dirent *e;
                while ((e = readdir(d))) if (!strcasecmp(e->d_name, c)) { snprintf(cand, sizeof cand, "%s/%s", out, e->d_name); break; }
                closedir(d);
            }
        }
        strcpy(out, cand);
    }
    strcpy(p, out);
}

const char *path_map(const char *w, char *out, size_t n) {
    char full[1024];
    if (isalpha((unsigned char)w[0]) && w[1] == ':') snprintf(full, sizeof full, "%s", w);
    else if (w[0] == '\\' || w[0] == '/') snprintf(full, sizeof full, "C:%s", w);
    else snprintf(full, sizeof full, "%s\\%s", g_cwd, w);
    // normalizes separators and strips "C:" + the "\Populous" prefix
    for (char *c = full; *c; c++) if (*c == '/') *c = '\\';
    const char *rest = full + 2;
    if (!strncasecmp(rest, "\\Populous", 9) && (rest[9] == '\\' || rest[9] == 0)) rest += 9;
    else if (!strncasecmp(rest, "\\POP3.CD", 8) && (rest[8] == '\\' || rest[8] == 0)) rest += 8;
    char buf[1024];
    snprintf(buf, sizeof buf, "%s%s", g_root, rest);
    for (char *c = buf; *c; c++) if (*c == '\\') *c = '/';
    // collapses "//" and "/./"
    char *s = buf, *d = buf;
    while (*s) {
        if (s[0] == '/' && s[1] == '/') { s++; continue; }
        if (s[0] == '/' && s[1] == '.' && (s[2] == '/' || s[2] == 0)) { s += 2; continue; }
        *d++ = *s++;
    }
    *d = 0;
    ci_resolve(buf);
    snprintf(out, n, "%s", buf);
    return out;
}

SHIM(CreateFileA, 7) {
    const char *name = gstr(ARG(0));
    uint32_t access = ARG(1), disp = ARG(4);
    char p[1024]; path_map(name, p, sizeof p);
    int fl = 0;
    if ((access & 0xC0000000) == 0xC0000000) fl = O_RDWR;
    else if (access & 0x40000000) fl = O_WRONLY;
    else fl = O_RDONLY;
    switch (disp) {
    case 1: fl |= O_CREAT | O_EXCL; break;   // CREATE_NEW
    case 2: fl |= O_CREAT | O_TRUNC; break;  // CREATE_ALWAYS
    case 3: break;                           // OPEN_EXISTING
    case 4: fl |= O_CREAT; break;            // OPEN_ALWAYS
    case 5: fl |= O_TRUNC; break;            // TRUNCATE_EXISTING
    }
    int fd = open(p, fl, 0644);
    if (fd < 0) {
        LOG("CreateFileA(%s -> %s) failed: %s", name, p, strerror(errno));
        set_last_error(R, errno == ENOENT ? 2 : errno == EEXIST ? 80 : 5);
        return 0xffffffff;
    }
    struct stat st; fstat(fd, &st);
    if (S_ISDIR(st.st_mode)) { close(fd); set_last_error(R, 5); return 0xffffffff; }
    uint32_t h = handle_new(H_FILE);
    Handle *x = handle_get(h);
    x->fd = fd; x->path = strdup(p);
    LOG("CreateFileA(%s) -> %x", name, h);
    set_last_error(R, 0);
    return h;
}
SHIM(ReadFile, 5) {
    Handle *x = handle_get(ARG(0));
    uint32_t buf = ARG(1), n = ARG(2), pread_ = ARG(3);
    if (!x || x->fd < 0) { if (pread_) wr32(pread_, 0); return 0; }
    ssize_t r = read(x->fd, PTR(buf), n);
    if (pread_) wr32(pread_, r < 0 ? 0 : (uint32_t)r);
    return r >= 0;
}
SHIM(WriteFile, 5) {
    uint32_t h = ARG(0), buf = ARG(1), n = ARG(2), pw = ARG(3);
    if (h >= 0xf000) { fwrite(PTR(buf), 1, n, stderr); if (pw) wr32(pw, n); return 1; }
    Handle *x = handle_get(h);
    if (!x || x->fd < 0) return 0;
    ssize_t r = write(x->fd, PTR(buf), n);
    if (pw) wr32(pw, r < 0 ? 0 : (uint32_t)r);
    return r >= 0;
}
SHIM(SetFilePointer, 4) {
    Handle *x = handle_get(ARG(0));
    if (!x) return 0xffffffff;
    int64_t off = (int32_t)ARG(1);
    if (ARG(2)) off = (int64_t)(((uint64_t)rd32(ARG(2)) << 32) | ARG(1));
    int wh = ARG(3) == 0 ? SEEK_SET : ARG(3) == 1 ? SEEK_CUR : SEEK_END;
    off_t r = lseek(x->fd, off, wh);
    if (r < 0) return 0xffffffff;
    if (ARG(2)) wr32(ARG(2), (uint32_t)(r >> 32));
    return (uint32_t)r;
}
SHIM(GetFileSize, 2) {
    Handle *x = handle_get(ARG(0));
    if (!x) return 0xffffffff;
    struct stat st; fstat(x->fd, &st);
    if (ARG(1)) wr32(ARG(1), 0);
    return (uint32_t)st.st_size;
}
SHIM(FlushFileBuffers, 1) { return 1; }
SHIM(SetEndOfFile, 1) {
    Handle *x = handle_get(ARG(0));
    if (!x) return 0;
    off_t pos = lseek(x->fd, 0, SEEK_CUR);
    return ftruncate(x->fd, pos) == 0;
}
SHIM(DeleteFileA, 1) { char p[1024]; path_map(gstr(ARG(0)), p, sizeof p); return unlink(p) == 0; }
SHIM(MoveFileA, 2) {
    char a[1024], b[1024]; path_map(gstr(ARG(0)), a, sizeof a); path_map(gstr(ARG(1)), b, sizeof b);
    return rename(a, b) == 0;
}
SHIM(CopyFileA, 3) {
    char a[1024], b[1024]; path_map(gstr(ARG(0)), a, sizeof a); path_map(gstr(ARG(1)), b, sizeof b);
    if (ARG(2) && access(b, F_OK) == 0) return 0;
    FILE *fi = fopen(a, "rb"); if (!fi) return 0;
    FILE *fo = fopen(b, "wb"); if (!fo) { fclose(fi); return 0; }
    char buf[65536]; size_t k;
    while ((k = fread(buf, 1, sizeof buf, fi)) > 0) fwrite(buf, 1, k, fo);
    fclose(fi); fclose(fo);
    return 1;
}
SHIM(CreateDirectoryA, 2) { char p[1024]; path_map(gstr(ARG(0)), p, sizeof p); return mkdir(p, 0755) == 0; }
SHIM(RemoveDirectoryA, 1) { char p[1024]; path_map(gstr(ARG(0)), p, sizeof p); return rmdir(p) == 0; }
SHIM(GetFileAttributesA, 1) {
    char p[1024]; path_map(gstr(ARG(0)), p, sizeof p);
    struct stat st;
    if (stat(p, &st) != 0) { set_last_error(R, 2); LOG("GetFileAttributesA(%s): does not exist", gstr(ARG(0))); return 0xffffffff; }
    return S_ISDIR(st.st_mode) ? 0x10 : 0x80;
}
SHIM(SetFileAttributesA, 2) { return 1; }
SHIM(GetFullPathNameA, 4) {
    const char *n = gstr(ARG(0));
    char full[1024];
    if (isalpha((unsigned char)n[0]) && n[1] == ':') snprintf(full, sizeof full, "%s", n);
    else snprintf(full, sizeof full, "%s\\%s", g_cwd, n);
    uint32_t len = (uint32_t)strlen(full);
    if (ARG(1) <= len) return len + 1;
    strcpy(gstr(ARG(2)), full);
    if (ARG(3)) { char *s = strrchr(gstr(ARG(2)), '\\'); wr32(ARG(3), s ? ARG(2) + (uint32_t)(s - gstr(ARG(2))) + 1 : 0); }
    return len;
}
SHIM(GetCurrentDirectoryA, 2) {
    uint32_t len = (uint32_t)strlen(g_cwd);
    if (ARG(0) <= len) return len + 1;
    strcpy(gstr(ARG(1)), g_cwd);
    return len;
}
SHIM(SetCurrentDirectoryA, 1) {
    const char *n = gstr(ARG(0));
    if (isalpha((unsigned char)n[0]) && n[1] == ':') snprintf(g_cwd, sizeof g_cwd, "%s", n);
    else { char t[512]; snprintf(t, sizeof t, "%s\\%s", g_cwd, n); strcpy(g_cwd, t); }
    size_t l = strlen(g_cwd); if (l > 3 && g_cwd[l - 1] == '\\') g_cwd[l - 1] = 0;
    LOG("SetCurrentDirectoryA(%s)", g_cwd);
    return 1;
}
SHIM(GetDriveTypeA, 1) {
    const char *n = ARG(0) ? gstr(ARG(0)) : "C:\\";
    return (toupper((unsigned char)n[0]) == 'D') ? 5 : 3;   // D: = CD-ROM, everything else fixed disk
}
SHIM(GetLogicalDriveStringsA, 2) {
    static const char s[] = "C:\\\0D:\\\0";
    if (ARG(0) < sizeof s) return sizeof s;
    memcpy(PTR(ARG(1)), s, sizeof s);
    return sizeof s - 1;
}
SHIM(GetLogicalDrives, 0) { return 0xc; }
SHIM(GetVolumeInformationA, 8) {
    if (ARG(1)) strncpy(gstr(ARG(1)), "POPTB", ARG(2));
    if (ARG(3)) wr32(ARG(3), 0x12345678);
    if (ARG(4)) wr32(ARG(4), 255);
    if (ARG(5)) wr32(ARG(5), 0);
    if (ARG(6)) strncpy(gstr(ARG(6)), "CDFS", ARG(7));
    return 1;
}

// ---- FindFirstFile ----
typedef struct { glob_t g; size_t i; } Find;
static void fill_find(uint32_t p, const char *path) {
    memset(PTR(p), 0, 320);
    struct stat st; stat(path, &st);
    wr32(p, S_ISDIR(st.st_mode) ? 0x10 : 0x80);
    wr32(p + 32, (uint32_t)st.st_size);
    const char *b = strrchr(path, '/'); b = b ? b + 1 : path;
    strncpy(gstr(p + 44), b, 259);
    strncpy(gstr(p + 304), b, 13);
}
static int glob_ci(const char *pat, glob_t *g) {
    // case-insensitive pattern: lists the directory and filters with fnmatch(FNM_CASEFOLD)
    memset(g, 0, sizeof *g);
    char dir[1024]; snprintf(dir, sizeof dir, "%s", pat);
    char *sl = strrchr(dir, '/');
    const char *mask = sl ? sl + 1 : dir;
    if (sl) *sl = 0;
    char m2[256]; snprintf(m2, sizeof m2, "%s", mask);
    if (!strcmp(m2, "*.*")) strcpy(m2, "*");
    DIR *d = opendir(sl ? dir : ".");
    if (!d) return 1;
    struct dirent *e;
    size_t cap = 16; g->gl_pathv = calloc(cap, sizeof(char *)); g->gl_pathc = 0;
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        if (fnmatch(m2, e->d_name, FNM_CASEFOLD) != 0) continue;
        if (g->gl_pathc + 1 >= cap) { cap *= 2; g->gl_pathv = realloc(g->gl_pathv, cap * sizeof(char *)); }
        char full[2048]; snprintf(full, sizeof full, "%s/%s", dir, e->d_name);
        g->gl_pathv[g->gl_pathc++] = strdup(full);
    }
    closedir(d);
    return g->gl_pathc ? 0 : 1;
}
SHIM(FindFirstFileA, 2) {
    char p[1024]; path_map(gstr(ARG(0)), p, sizeof p);
    Find *f = calloc(1, sizeof *f);
    if (glob_ci(p, &f->g) != 0) { free(f->g.gl_pathv); free(f); set_last_error(R, 2); LOG("FindFirstFileA(%s): nothing", gstr(ARG(0))); return 0xffffffff; }
    uint32_t h = handle_new(H_FIND);
    handle_get(h)->find = f;
    fill_find(ARG(1), f->g.gl_pathv[f->i++]);
    return h;
}
SHIM(FindNextFileA, 2) {
    Handle *x = handle_get(ARG(0));
    if (!x || !x->find) return 0;
    Find *f = x->find;
    if (f->i >= f->g.gl_pathc) { set_last_error(R, 18); return 0; }
    fill_find(ARG(1), f->g.gl_pathv[f->i++]);
    return 1;
}
SHIM(FindClose, 1) {
    Handle *x = handle_get(ARG(0));
    if (x && x->find) {
        Find *f = x->find;
        for (size_t i = 0; i < f->g.gl_pathc; i++) free(f->g.gl_pathv[i]);
        free(f->g.gl_pathv); free(f);
        x->find = NULL;
    }
    handle_close(ARG(0));
    return 1;
}

// ---- file mapping (read-only: copies into guest memory) ----
SHIM(CreateFileMappingA, 6) {
    uint32_t h = handle_new(H_MAPPING);
    handle_get(h)->file = ARG(0);
    return h;
}
SHIM(MapViewOfFile, 5) {
    Handle *m = handle_get(ARG(0));
    if (!m) return 0;
    Handle *f = handle_get(m->file);
    if (!f) return 0;
    struct stat st; fstat(f->fd, &st);
    uint32_t off = ARG(3), n = ARG(4) ? ARG(4) : (uint32_t)st.st_size - off;
    uint32_t p = gmalloc(n);
    pread(f->fd, PTR(p), n, off);
    return p;
}
SHIM(UnmapViewOfFile, 1) { gfree(ARG(0)); return 1; }

// ---- "ini" files (QMixer) ----
SHIM(GetPrivateProfileIntA, 4) { return ARG(2); }
SHIM(GetPrivateProfileStringA, 6) {
    const char *def = ARG(2) ? gstr(ARG(2)) : "";
    strncpy(gstr(ARG(3)), def, ARG(4));
    return (uint32_t)strlen(gstr(ARG(3)));
}
SHIM(WritePrivateProfileStringA, 4) { return 1; }

// ---- registry ----
typedef struct { const char *name; int type; const char *s; uint32_t d; } RegVal;
static const RegVal regvals[] = {
    {"Language", 4, NULL, 9},
    {"BuildTypeCode", 4, NULL, 1},
    {"Version", 1, "1.01", 0},
    {"InstallDrive", 1, "C:", 0},
    {"InstallPath", 1, "C:\\Populous", 0},
    {"InstallDirectory", 1, "\\Populous", 0},
    {"CDDrive", 1, "C:", 0},
};
SHIM(RegOpenKeyExA, 5) { LOG("RegOpenKeyExA(%s)", ARG(1) ? gstr(ARG(1)) : ""); wr32(ARG(4), 0x2001); return 0; }
SHIM(RegOpenKeyA, 3) { LOG("RegOpenKeyA(%s)", ARG(1) ? gstr(ARG(1)) : ""); wr32(ARG(2), 0x2001); return 0; }
SHIM(RegCreateKeyExA, 9) { LOG("RegCreateKeyExA(%s)", ARG(1) ? gstr(ARG(1)) : ""); wr32(ARG(7), 0x2001); if (ARG(8)) wr32(ARG(8), 2); return 0; }
SHIM(RegCloseKey, 1) { return 0; }
SHIM(RegSetValueExA, 6) { LOG("RegSetValueExA(%s) type=%u len=%u", ARG(1) ? gstr(ARG(1)) : "", ARG(3), ARG(5)); return 0; }
SHIM(RegFlushKey, 1) { return 0; }
SHIM(RegQueryValueExA, 6) {
    const char *n = ARG(1) ? gstr(ARG(1)) : "";
    uint32_t ptype = ARG(3), data = ARG(4), plen = ARG(5);
    for (size_t i = 0; i < sizeof regvals / sizeof regvals[0]; i++) if (!strcasecmp(regvals[i].name, n)) {
        const RegVal *v = &regvals[i];
        if (ptype) wr32(ptype, v->type);
        uint32_t need = v->type == 1 ? (uint32_t)strlen(v->s) + 1 : 4;
        if (plen) {
            uint32_t cap = rd32(plen);
            wr32(plen, need);
            if (data) { if (cap < need) return 234; if (v->type == 1) memcpy(PTR(data), v->s, need); else wr32(data, v->d); }
        }
        LOG("RegQueryValueExA(%s) ok", n);
        return 0;
    }
    LOG("RegQueryValueExA(%s): missing", n);
    return 2;
}
