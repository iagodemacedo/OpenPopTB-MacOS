// Emulated x86 CPU state and helpers used by the recompiled code (gen/*.c).
#pragma once
#include <stdint.h>
#include <string.h>
#include <math.h>

typedef struct CPU {
    uint32_t eax, ecx, edx, ebx, esp, ebp, esi, edi;
    // lazy flags: the last operation that wrote flags
    uint32_t cc_op, cc_a, cc_b, cc_res, cc_cf, cc_of;
    uint32_t df;        // direction flag (0/1)
    uint32_t fs_base;   // address of the thread's TIB
    // x87
    double st[8];
    uint32_t ftop;
    uint16_t fsw, fcw, ftag;
    void *thread;       // native thread data (win32 shim)
} CPU;

extern uint8_t *MEM;    // base of the guest's 4 GB address space

typedef uint32_t __attribute__((aligned(1), may_alias)) u32u;
typedef uint16_t __attribute__((aligned(1), may_alias)) u16u;
typedef uint64_t __attribute__((aligned(1), may_alias)) u64u;
typedef float __attribute__((aligned(1), may_alias)) f32u;
typedef double __attribute__((aligned(1), may_alias)) f64u;

#define PTR(a) (MEM + (uint32_t)(a))
static inline uint32_t rd8(uint32_t a) { return MEM[a]; }
static inline uint32_t rd16(uint32_t a) { return *(u16u *)(MEM + a); }
static inline uint32_t rd32(uint32_t a) { return *(u32u *)(MEM + a); }
static inline uint64_t rd64(uint32_t a) { return *(u64u *)(MEM + a); }
static inline void wr8(uint32_t a, uint32_t v) { MEM[a] = (uint8_t)v; }
static inline void wr16(uint32_t a, uint32_t v) { *(u16u *)(MEM + a) = (uint16_t)v; }
static inline void wr32(uint32_t a, uint32_t v) { *(u32u *)(MEM + a) = v; }
static inline void wr64(uint32_t a, uint64_t v) { *(u64u *)(MEM + a) = v; }
static inline float rdf32(uint32_t a) { return *(f32u *)(MEM + a); }
static inline double rdf64(uint32_t a) { return *(f64u *)(MEM + a); }
static inline void wrf32(uint32_t a, float v) { *(f32u *)(MEM + a) = v; }
static inline void wrf64(uint32_t a, double v) { *(f64u *)(MEM + a) = v; }
double rdf80(uint32_t a);
void wrf80(uint32_t a, double v);

#define PUSH(v) do { uint32_t _v = (v); R->esp -= 4; wr32(R->esp, _v); } while (0)
#define POP() (R->esp += 4, rd32(R->esp - 4))

// ---- flags ----
enum { CC_ADD, CC_SUB, CC_LOGIC, CC_INC, CC_DEC, CC_SHL, CC_SHR, CC_SAR, CC_ADC, CC_SBB, CC_NEG, CC_MUL, CC_EFL };
#define CCOP(k, sz) ((k) * 4 + ((sz) == 1 ? 0 : (sz) == 2 ? 1 : 2))

enum { F_CF = 1, F_PF = 4, F_AF = 16, F_ZF = 64, F_SF = 128, F_DF = 1024, F_OF = 2048 };

static inline uint32_t parity8(uint32_t v) {
    v &= 0xff; v ^= v >> 4; v ^= v >> 2; v ^= v >> 1;
    return (~v) & 1;
}

// computes EFLAGS (CF PF ZF SF OF) for an operation
static inline __attribute__((always_inline)) uint32_t eflags_of(uint32_t op, uint32_t a, uint32_t b, uint32_t r, uint32_t cfin, uint32_t ofin) {
    uint32_t k = op >> 2, s = op & 3;
    uint32_t mask = s == 0 ? 0xff : s == 1 ? 0xffff : 0xffffffff;
    uint32_t sign = s == 0 ? 0x80 : s == 1 ? 0x8000 : 0x80000000;
    if (k == CC_EFL) return a;
    a &= mask; b &= mask; r &= mask;
    uint32_t cf = 0, of = 0;
    switch (k) {
    case CC_ADD: cf = r < a; of = ((a ^ r) & (b ^ r) & sign) != 0; break;
    case CC_ADC: cf = cfin ? r <= a : r < a; of = ((a ^ r) & (b ^ r) & sign) != 0; break;
    case CC_SUB: cf = a < b; of = ((a ^ b) & (a ^ r) & sign) != 0; break;
    case CC_SBB: cf = cfin ? a <= b : a < b; of = ((a ^ b) & (a ^ r) & sign) != 0; break;
    case CC_LOGIC: break;
    case CC_INC: cf = cfin; of = r == sign; break;
    case CC_DEC: cf = cfin; of = r == sign - 1; break;
    case CC_NEG: cf = r != 0; of = r == sign; break;
    case CC_SHL: case CC_SHR: case CC_SAR: case CC_MUL: cf = cfin; of = ofin; break;
    }
    return cf | (parity8(r) << 2) | ((r == 0) << 6) | (((r & sign) != 0) << 7) | (of << 11);
}

static inline uint32_t get_flags(CPU *R) {
    if ((R->cc_op >> 2) == CC_EFL) return R->cc_a;
    return eflags_of(R->cc_op, R->cc_a, R->cc_b, R->cc_res, R->cc_cf, R->cc_of);
}
static inline void set_flags(CPU *R, uint32_t f) { R->cc_op = CCOP(CC_EFL, 4); R->cc_a = f; }

// conditions from materialized EFLAGS
static inline int cc_test(int cc, uint32_t f) {
    int cf = f & 1, pf = (f >> 2) & 1, zf = (f >> 6) & 1, sf = (f >> 7) & 1, of = (f >> 11) & 1;
    switch (cc) {
    case 0: return of; case 1: return !of;
    case 2: return cf; case 3: return !cf;
    case 4: return zf; case 5: return !zf;
    case 6: return cf | zf; case 7: return !(cf | zf);
    case 8: return sf; case 9: return !sf;
    case 10: return pf; case 11: return !pf;
    case 12: return sf != of; case 13: return sf == of;
    case 14: return zf | (sf != of); default: return !(zf | (sf != of));
    }
}
static inline int cond(CPU *R, int cc) {
    // shortcuts for 32-bit SUB/LOGIC (the most common case outside the fused path)
    if (R->cc_op == CCOP(CC_SUB, 4)) {
        uint32_t a = R->cc_a, b = R->cc_b;
        switch (cc) {
        case 2: return a < b; case 3: return a >= b; case 4: return a == b; case 5: return a != b;
        case 6: return a <= b; case 7: return a > b;
        case 12: return (int32_t)a < (int32_t)b; case 13: return (int32_t)a >= (int32_t)b;
        case 14: return (int32_t)a <= (int32_t)b; case 15: return (int32_t)a > (int32_t)b;
        }
    }
    return cc_test(cc, get_flags(R));
}
static inline uint32_t get_cf(CPU *R) { return get_flags(R) & 1; }

// ---- x87 ----
#define ST(i) R->st[(R->ftop + (i)) & 7]
// x87 precision control (FCW bits 8-9): 24 bits rounds to float; 53/64 bits use double
#define FPR(x) (__builtin_expect((R->fcw & 0x300) == 0, 0) ? (double)(float)(x) : (double)(x))
static inline void fpush(CPU *R, double v) { R->ftop = (R->ftop - 1) & 7; R->st[R->ftop] = v; }
static inline double fpop(CPU *R) { double v = R->st[R->ftop]; R->ftop = (R->ftop + 1) & 7; return v; }
static inline void fcom_set(CPU *R, double a, double b) {
    uint16_t c = 0;
    if (isnan(a) || isnan(b)) c = 0x4500;
    else if (a < b) c = 0x0100;
    else if (a == b) c = 0x4000;
    R->fsw = (R->fsw & ~0x4700) | c;
}
static inline uint32_t fstsw(CPU *R) { return (R->fsw & ~0x3800) | ((R->ftop & 7) << 11); }
double frnd(CPU *R, double v);   // rounds according to the control word
int64_t fist(CPU *R, double v, int bits);
void fxam(CPU *R);
void fnsave(CPU *R, uint32_t a);
void frstor(CPU *R, uint32_t a);

// ---- division ----
void div_trap(CPU *R, uint32_t eip);

// ---- control ----
extern volatile int gil_contended;
void gil_yield(void);
#define YIELD() do { if (__builtin_expect(gil_contended, 0)) gil_yield(); } while (0)
typedef void (*gfn)(CPU *);
void dispatch(CPU *R, uint32_t target);
void rt_trap(CPU *R, uint32_t eip, const char *what);
void rt_cpuid(CPU *R);
uint64_t rt_rdtsc(void);

// string ops
void rep_movs(CPU *R, int sz, int rep);
void rep_stos(CPU *R, int sz, int rep);
void rep_scas(CPU *R, int sz, int rep);   // rep: 0 none, 1 repe, 2 repne
void rep_cmps(CPU *R, int sz, int rep);
void do_lods(CPU *R, int sz);
