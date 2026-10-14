// Fake COM objects: vtable in guest memory pointing at native thunks.
#include "rt.h"

static void *natives[65536];
static int nnat = 1;

uint32_t com_vtbl(const char *iface, const char **methods, int n, native_fn *impls) {
    uint32_t vt = gmalloc(4 * n);
    for (int i = 0; i < n; i++) {
        char *nm = malloc(strlen(iface) + strlen(methods[i]) + 3);
        sprintf(nm, "%s::%s", iface, methods[i]);
        wr32(vt + 4 * i, thunk_new(impls[i], nm));
    }
    return vt;
}

uint32_t com_new(const char *iface, uint32_t vtbl, uint32_t extra, void *native) {
    (void)iface;
    uint32_t o = gmalloc(16 + extra);
    wr32(o, vtbl);
    wr32(o + 4, 1);
    if (nnat >= 65536) rt_fatal("out of COM objects");
    natives[nnat] = native;
    wr32(o + 8, (uint32_t)nnat++);
    return o;
}

void *com_native(uint32_t o) {
    if (!o) return NULL;
    uint32_t i = rd32(o + 8);
    return i < 65536 ? natives[i] : NULL;
}
