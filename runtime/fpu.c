// x87 helpers used by the generated code (80-bit format, rounding, fxam, fnsave).
#include "cpu.h"

// ---- x87 ----
double rdf80(uint32_t a) {
    uint64_t man = rd64(a);
    uint32_t se = rd16(a + 8);
    int sign = se >> 15, exp = se & 0x7fff;
    if (exp == 0 && man == 0) return sign ? -0.0 : 0.0;
    if (exp == 0x7fff) return (man << 1) ? NAN : (sign ? -INFINITY : INFINITY);
    double v = ldexp((double)man, exp - 16383 - 63);
    return sign ? -v : v;
}
void wrf80(uint32_t a, double v) {
    uint64_t man = 0; int exp = 0, sign = signbit(v) ? 1 : 0;
    if (v == 0) { exp = 0; man = 0; }
    else if (isinf(v)) { exp = 0x7fff; man = 0x8000000000000000ull; }
    else if (isnan(v)) { exp = 0x7fff; man = 0xC000000000000000ull; }
    else {
        int e; double f = frexp(fabs(v), &e);   // f in [0.5,1)
        man = (uint64_t)ldexp(f, 64);
        exp = e - 1 + 16383;
    }
    wr64(a, man);
    wr16(a + 8, (uint32_t)((sign << 15) | exp));
}
double frnd(CPU *R, double v) {
    switch ((R->fcw >> 10) & 3) {
    case 0: return nearbyint(v);
    case 1: return floor(v);
    case 2: return ceil(v);
    default: return trunc(v);
    }
}
int64_t fist(CPU *R, double v, int bits) {
    double r = frnd(R, v);
    if (bits == 16) { if (!(r >= -32768.0 && r <= 32767.0)) return 0x8000; return (int64_t)r; }
    if (bits == 32) { if (!(r >= -2147483648.0 && r <= 2147483647.0)) return 0x80000000ll; return (int64_t)r; }
    if (!(r >= -9.2233720368547758e18 && r < 9.2233720368547758e18)) return INT64_MIN;
    return (int64_t)r;
}
void fxam(CPU *R) {
    double v = ST(0);
    uint16_t c;   // C3 C2 C0
    if (isnan(v)) c = 0x0100;
    else if (isinf(v)) c = 0x0500;
    else if (v == 0) c = 0x4000;
    else if (fpclassify(v) == FP_SUBNORMAL) c = 0x4400;
    else c = 0x0400;
    if (signbit(v)) c |= 0x0200;   // C1 = sign
    R->fsw = (R->fsw & ~0x4700) | c;
}
void fnsave(CPU *R, uint32_t a) {
    memset(PTR(a), 0, 108);
    wr16(a, R->fcw); wr16(a + 4, fstsw(R)); wr16(a + 8, 0);
    for (int i = 0; i < 8; i++) wrf80(a + 28 + i * 10, ST(i));
    R->fcw = 0x37f; R->fsw = 0; R->ftop = 0;
}
void frstor(CPU *R, uint32_t a) {
    R->fcw = rd16(a); uint32_t sw = rd16(a + 4);
    R->ftop = (sw >> 11) & 7; R->fsw = sw & ~0x3800;
    for (int i = 0; i < 8; i++) ST(i) = rdf80(a + 28 + i * 10);
}

