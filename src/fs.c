#include "fs.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static char g_root[1024] = ".";

void fs_set_root(const char *root) { snprintf(g_root, sizeof g_root, "%s", root); }
const char *fs_root(void) { return g_root; }

// Resolves each path component case-insensitively.
static int resolve_ci(const char *rel, char *out, size_t outsz) {
    char work[1024];
    snprintf(work, sizeof work, "%s", rel);
    snprintf(out, outsz, "%s", g_root);
    char *save = NULL;
    for (char *part = strtok_r(work, "/\\", &save); part; part = strtok_r(NULL, "/\\", &save)) {
        DIR *d = opendir(out);
        if (!d) return 0;
        struct dirent *e;
        int found = 0;
        while ((e = readdir(d))) {
            if (strcasecmp(e->d_name, part) == 0) {
                size_t len = strlen(out);
                snprintf(out + len, outsz - len, "/%s", e->d_name);
                found = 1;
                break;
            }
        }
        closedir(d);
        if (!found) return 0;
    }
    return 1;
}

uint8_t *fs_load(const char *relpath, size_t *out_size) {
    char path[2048];
    if (!resolve_ci(relpath, path, sizeof path)) {
        fprintf(stderr, "fs: not found: %s\n", relpath);
        return NULL;
    }
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *buf = malloc((size_t)n + 1);
    if (buf && fread(buf, 1, (size_t)n, f) != (size_t)n) {
        free(buf);
        buf = NULL;
    }
    fclose(f);
    if (buf && out_size) *out_size = (size_t)n;
    return buf;
}
