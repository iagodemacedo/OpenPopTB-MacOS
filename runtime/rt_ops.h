// Rotations (rare): update CF/OF only when the flags are live.
#pragma once
#include "cpu.h"

static inline uint32_t rt_rot_mask(int sz) { return sz == 1 ? 0xffu : sz == 2 ? 0xffffu : 0xffffffffu; }

static inline uint32_t rt_rol(CPU *R, uint32_t a, uint32_t n, int sz, int live) {
    int bits = sz * 8; uint32_t m = rt_rot_mask(sz);
    a &= m;
    if (!n) return a;
    uint32_t k = n % bits, r = k ? ((a << k) | (a >> (bits - k))) & m : a;
    if (live) {
        uint32_t cf = r & 1, of = ((r >> (bits - 1)) & 1) ^ cf;
        set_flags(R, (get_flags(R) & ~0x801u) | cf | (of << 11));
    }
    return r;
}
static inline uint32_t rt_ror(CPU *R, uint32_t a, uint32_t n, int sz, int live) {
    int bits = sz * 8; uint32_t m = rt_rot_mask(sz);
    a &= m;
    if (!n) return a;
    uint32_t k = n % bits, r = k ? ((a >> k) | (a << (bits - k))) & m : a;
    if (live) {
        uint32_t cf = (r >> (bits - 1)) & 1, of = cf ^ ((r >> (bits - 2)) & 1);
        set_flags(R, (get_flags(R) & ~0x801u) | cf | (of << 11));
    }
    return r;
}
static inline uint32_t rt_rcl(CPU *R, uint32_t a, uint32_t n, int sz, int live) {
    int bits = sz * 8; uint32_t m = rt_rot_mask(sz);
    uint32_t f = get_flags(R), cf = f & 1;
    a &= m;
    n %= (bits + 1);
    for (uint32_t i = 0; i < n; i++) {
        uint32_t out = (a >> (bits - 1)) & 1;
        a = ((a << 1) | cf) & m; cf = out;
    }
    if (n) set_flags(R, (f & ~0x801u) | cf | ((((a >> (bits - 1)) & 1) ^ cf) << 11));
    (void)live;
    return a;
}
static inline uint32_t rt_rcr(CPU *R, uint32_t a, uint32_t n, int sz, int live) {
    int bits = sz * 8; uint32_t m = rt_rot_mask(sz);
    uint32_t f = get_flags(R), cf = f & 1;
    a &= m;
    n %= (bits + 1);
    for (uint32_t i = 0; i < n; i++) {
        uint32_t out = a & 1;
        a = (a >> 1) | (cf << (bits - 1)); cf = out;
    }
    if (n) set_flags(R, (f & ~0x801u) | cf | ((((a >> (bits - 1)) ^ (a >> (bits - 2))) & 1) << 11));
    (void)live;
    return a;
}
