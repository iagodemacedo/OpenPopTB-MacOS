// The guest's 4 GB address space and the heap allocator inside it.
#include "rt.h"
#include <sys/mman.h>
#include <pthread.h>

uint8_t *MEM;
static pthread_mutex_t heap_mu = PTHREAD_MUTEX_INITIALIZER;

void mem_init(void) {
    void *p = mmap(NULL, 1ull << 32, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON | MAP_NORESERVE, -1, 0);
    if (p == MAP_FAILED) rt_fatal("mmap 4GB failed");
    MEM = p;
    // zero page inaccessible to catch guest null pointers
    mprotect(MEM, 0x10000, PROT_NONE);
}

// Allocator: blocks with a 16-byte header {size, magic, next_free, pad}.
// Free lists per size class (multiples of 16 up to 4 KB, powers of 2 above).
#define HDR 16u
#define MAGIC_USED 0xA110C8EDu
#define MAGIC_FREE 0xF4EEB10Cu
#define NCLS 64
uint32_t brk_ptr = GUEST_HEAP_BASE;
uint32_t reserved_lo[8], reserved_hi[8];
int nreserved;
static uint32_t freelist[NCLS];

static int cls_of(uint32_t sz) {   // sz already rounded to 16
    if (sz <= 4096) return (int)(sz / 16) - 1;    // 0..255 -> but we limit it below
    int c = 32 - __builtin_clz(sz - 1);           // ceil log2
    return 40 + (c - 12);
}
static uint32_t round_sz(uint32_t sz) {
    sz = (sz + 15) & ~15u;
    if (sz == 0) sz = 16;
    if (sz > 4096) { uint32_t p = 1u << (32 - __builtin_clz(sz - 1)); sz = p; }
    return sz;
}
static int idx_of(uint32_t sz) {
    if (sz <= 512) return (int)(sz / 16) - 1;      // 0..31
    if (sz <= 4096) return 32 + (int)((sz - 513) / 512);  // 32..39
    return cls_of(sz);
}

uint32_t gmalloc(uint32_t size) {
    pthread_mutex_lock(&heap_mu);
    uint32_t sz = size <= 512 ? ((size + 15) & ~15u) : size <= 4096 ? ((size + 511) & ~511u) : round_sz(size);
    if (sz == 0) sz = 16;
    int i = idx_of(sz);
    uint32_t p = 0;
    if (i < NCLS && freelist[i]) {
        uint32_t h = freelist[i];
        freelist[i] = rd32(h + 8);
        p = h + HDR;
        sz = rd32(h);
    } else {
        // skips ranges taken by DLLs loaded in the heap space (e.g. QMixer at 0x18000000)
        extern uint32_t reserved_lo[], reserved_hi[]; extern int nreserved;
        for (int k = 0; k < nreserved; k++)
            if (brk_ptr + HDR + sz > reserved_lo[k] && brk_ptr < reserved_hi[k]) brk_ptr = reserved_hi[k];
        uint32_t h = brk_ptr;
        if ((uint64_t)h + HDR + sz > GUEST_HEAP_END) { pthread_mutex_unlock(&heap_mu); WARN("guest heap exhausted (%u)", size); return 0; }
        brk_ptr += HDR + sz;
        wr32(h, sz);
        p = h + HDR;
    }
    wr32(p - HDR + 4, MAGIC_USED);
    wr32(p - HDR + 12, size);
    pthread_mutex_unlock(&heap_mu);
    memset(PTR(p), 0, sz);
    return p;
}

void gfree(uint32_t p) {
    if (!p) return;
    pthread_mutex_lock(&heap_mu);
    uint32_t h = p - HDR;
    if (rd32(h + 4) != MAGIC_USED) { pthread_mutex_unlock(&heap_mu); WARN("invalid free 0x%x", p); return; }
    uint32_t sz = rd32(h);
    int i = idx_of(sz);
    wr32(h + 4, MAGIC_FREE);
    wr32(h + 8, freelist[i]);
    freelist[i] = h;
    pthread_mutex_unlock(&heap_mu);
}

uint32_t gsize(uint32_t p) { return p ? rd32(p - HDR + 12) : 0; }

uint32_t grealloc(uint32_t p, uint32_t size) {
    if (!p) return gmalloc(size);
    uint32_t cap = rd32(p - HDR);
    if (size <= cap) { wr32(p - HDR + 12, size); return p; }
    uint32_t n = gmalloc(size);
    if (!n) return 0;
    memcpy(PTR(n), PTR(p), rd32(p - HDR + 12));
    gfree(p);
    return n;
}

uint32_t virt_ptr = GUEST_VIRT_BASE;
uint32_t vmalloc(uint32_t addr, uint32_t size) {
    pthread_mutex_lock(&heap_mu);
    size = (size + 0xffff) & ~0xffffu;
    uint32_t r;
    if (addr && addr >= GUEST_VIRT_BASE && addr < virt_ptr) r = addr;   // commit in an already reserved region
    else {
        if ((uint64_t)virt_ptr + size > GUEST_VIRT_END) { pthread_mutex_unlock(&heap_mu); return 0; }
        r = virt_ptr; virt_ptr += size;
    }
    pthread_mutex_unlock(&heap_mu);
    return r;
}

uint32_t gstrdup(const char *s) {
    uint32_t n = (uint32_t)strlen(s) + 1, p = gmalloc(n);
    memcpy(PTR(p), s, n);
    return p;
}
