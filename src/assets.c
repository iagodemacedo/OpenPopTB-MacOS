#include "assets.h"
#include "fs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int theme_load(Theme *t, char key) {
    memset(t, 0, sizeof *t);
    char name[64];
    size_t n = 0;

    snprintf(name, sizeof name, "data/pal0-%c.dat", key);
    uint8_t *p = fs_load(name, &n);
    if (!p || n != 1024) { free(p); return 0; }
    memcpy(t->pal, p, 1024);
    free(p);

    snprintf(name, sizeof name, "data/sky0-%c.dat", key);
    t->sky = fs_load(name, &n);
    snprintf(name, sizeof name, "data/bl320-%c.dat", key);
    t->blocks = fs_load(name, &n);
    snprintf(name, sizeof name, "data/fade0-%c.dat", key);
    t->fade = fs_load(name, &n);
    return t->sky && t->blocks && t->fade;
}

void theme_free(Theme *t) {
    free(t->sky);
    free(t->blocks);
    free(t->fade);
    memset(t, 0, sizeof *t);
}
