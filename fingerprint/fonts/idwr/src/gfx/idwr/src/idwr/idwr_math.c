/*
 * idwr_math: ports of ucrtbase.dll 10.0.26100.9711 (x64) powf, tanf, tan, sinf, cosf, sin, cos,
 * _hypot, pow, expf, logf, asinf, acosf, atanf and atan2f.
 *
 * ucrtbase picks a code path per CPU at load time (__acrt_initialize_fma3): with FMA3, AVX and
 * OS AVX state it sets __use_fma3_lib and all but _hypot take their FMA3 code; otherwise their
 * SSE2 code. Every CPU Windows 11 supports has FMA3, so these are ports of the FMA3 code, operation
 * by operation, each x86 instruction as the C operation with the same IEEE result:
 *   vfmadd / vfnmadd / vfmsub        idwr_fma, idwr_fmaf (exact, software: the same bits on any CPU)
 *   cvtpd2dq                          round half to even (default MXCSR)
 *   cvttpd2dq                         truncation, 0x80000000 when out of range
 *   x86 shifts                        count masked to 6 bits
 * _hypot has no FMA3 variant. Addresses in the comments are ucrtbase 10.0.26100.9711 VAs.
 * Notes: notes/math.md.
 */
#include <stdint.h>
#include <string.h>
#include "../truetype/fp_exact.h"
#include "idwr_math.h"
#include "idwr_math_tables.h"

/* Negative controls (reimpl/math_check.py --negative-control): 1-3 an internal constant one ulp
   off; 4, 7, 8 the result one ulp off; 5-6 a fused multiply-add left unfused; 9 tanf's reduction
   without its pi/2 tail. sinf/cosf/sin/cos: 10, 12, 13, 15 a polynomial coefficient one ulp off;
   11 sinf/cosf's reduction without its pi/2 tail; 14 sin/cos's reduction without its tail rr;
   16-17 a sinf/cosf coefficient rounded to float. pow, expf, logf, asinf, acosf, atanf, atan2f:
   18, 20, 22, 24, 27, 29 an internal constant one ulp off; 19, 21, 23, 25, 26, 28, 30 the result
   one ulp off; 31 idwr_fmaf rounding twice (no round to odd). */
#ifndef IDWR_MATH_NEGATIVE_CONTROL
#define IDWR_MATH_NEGATIVE_CONTROL 0
#endif
#define NC(k) (IDWR_MATH_NEGATIVE_CONTROL == (k) ? 1u : 0u)

static inline uint64_t d2u(double d) { uint64_t u; memcpy(&u, &d, 8); return u; }
static inline double u2d(uint64_t u) { double d; memcpy(&d, &u, 8); return d; }
static inline uint32_t f2u(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }
static inline float u2f(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }

#define SIGN64 0x8000000000000000ull

/* ---------------------------------------------------------------------------------------------
 * Exact fused multiply-add, round to nearest even, in integer arithmetic.
 * ------------------------------------------------------------------------------------------- */

typedef struct { uint64_t hi, lo; } u128;

static u128 mul64x64(uint64_t a, uint64_t b)
{
    uint64_t a0 = a & 0xffffffffu, a1 = a >> 32, b0 = b & 0xffffffffu, b1 = b >> 32;
    uint64_t p00 = a0 * b0, p01 = a0 * b1, p10 = a1 * b0, p11 = a1 * b1;
    uint64_t mid = (p00 >> 32) + (p01 & 0xffffffffu) + (p10 & 0xffffffffu);
    u128 r;
    r.lo = (mid << 32) | (p00 & 0xffffffffu);
    r.hi = p11 + (p01 >> 32) + (p10 >> 32) + (mid >> 32);
    return r;
}

static int bitlen64(uint64_t x)
{
    int n = 0;
    while (x) { n++; x >>= 1; }
    return n;
}

static int bitlen128(u128 x) { return x.hi ? 64 + bitlen64(x.hi) : bitlen64(x.lo); }

static u128 shl128(u128 x, int s) /* 0 <= s < 128 */
{
    u128 r;
    if (s == 0) return x;
    if (s >= 64) { r.hi = x.lo << (s - 64); r.lo = 0; return r; }
    r.hi = (x.hi << s) | (x.lo >> (64 - s));
    r.lo = x.lo << s;
    return r;
}

/* x >> s, with every bit shifted out ORed into bit 0 */
static u128 shr128_sticky(u128 x, int s)
{
    u128 r;
    uint64_t sticky;
    if (s <= 0) return x;
    if (s >= 128) { r.hi = 0; r.lo = (x.hi | x.lo) != 0; return r; }
    if (s >= 64) {
        sticky = x.lo != 0 || (s > 64 && (x.hi << (128 - s)) != 0);
        r.lo = (s == 64 ? x.hi : x.hi >> (s - 64)) | sticky;
        r.hi = 0;
        return r;
    }
    sticky = (x.lo << (64 - s)) != 0;
    r.lo = (x.lo >> s) | (x.hi << (64 - s)) | sticky;
    r.hi = x.hi >> s;
    return r;
}

static int cmp128(u128 a, u128 b)
{
    if (a.hi != b.hi) return a.hi < b.hi ? -1 : 1;
    if (a.lo != b.lo) return a.lo < b.lo ? -1 : 1;
    return 0;
}

static u128 add128(u128 a, u128 b)
{
    u128 r;
    r.lo = a.lo + b.lo;
    r.hi = a.hi + b.hi + (r.lo < a.lo);
    return r;
}

static u128 sub128(u128 a, u128 b) /* a >= b */
{
    u128 r;
    r.lo = a.lo - b.lo;
    r.hi = a.hi - b.hi - (a.lo < b.lo);
    return r;
}

/* bit i of x (0 if i is outside 0..127) */
static int bit128(u128 x, int i)
{
    if (i < 0 || i >= 128) return 0;
    return (int)((i >= 64 ? x.hi >> (i - 64) : x.lo >> i) & 1);
}

/* any bit of x below position i */
static int below128(u128 x, int i)
{
    if (i <= 0) return 0;
    if (i >= 128) return (x.hi | x.lo) != 0;
    if (i > 64) return x.lo != 0 || (x.hi << (128 - i)) != 0;
    if (i == 64) return x.lo != 0;
    return (x.lo << (64 - i)) != 0;
}

/* finite nonzero x = m * 2^e, m < 2^53 */
static void split(double x, uint64_t *m, int *e)
{
    uint64_t b = d2u(x);
    int be = (int)((b >> 52) & 0x7ff);
    uint64_t f = b & 0x000fffffffffffffull;
    if (be == 0) { *m = f; *e = -1074; }
    else { *m = f | 0x0010000000000000ull; *e = be - 1075; }
}

double idwr_fma(double a, double b, double c)
{
    uint64_t ma, mb, mc, sp, sc, sr;
    int ea, eb, ec, ep, e, lp, lc, L, E, drop;
    u128 P, C, R, big, small;
    uint64_t m;
    uint64_t ua = d2u(a), ub = d2u(b), uc = d2u(c);

    /* NaN and infinity results written out, so that they do not depend on the CPU's NaN rules
       (x86 and ARM pick different NaNs): the first NaN operand quieted; an invalid operation
       gives x86's default NaN. The ports above never pass a NaN or an infinity. */
    {
        int na = (ua & ~SIGN64) > 0x7ff0000000000000ull, nb = (ub & ~SIGN64) > 0x7ff0000000000000ull;
        int nc = (uc & ~SIGN64) > 0x7ff0000000000000ull;
        int ia = (ua & ~SIGN64) == 0x7ff0000000000000ull, ib = (ub & ~SIGN64) == 0x7ff0000000000000ull;
        int ic = (uc & ~SIGN64) == 0x7ff0000000000000ull;
        if (na || nb || nc) return u2d((na ? ua : nb ? ub : uc) | 0x0008000000000000ull);
        if (ia || ib) {
            uint64_t s = (ua ^ ub) & SIGN64;
            if ((ia && b == 0) || (ib && a == 0)) return u2d(0xfff8000000000000ull);
            if (ic && (uc & SIGN64) != s) return u2d(0xfff8000000000000ull);
            return u2d(s | 0x7ff0000000000000ull);
        }
        if (ic) return c;
    }
    if (a == 0 || b == 0)
        return a * b + c;           /* the product is an exact zero: one rounding, as fused */
    if (c == 0)
        return a * b;               /* one rounding; the product is nonzero */

    split(a, &ma, &ea);
    split(b, &mb, &eb);
    split(c, &mc, &ec);
    sp = (ua ^ ub) & SIGN64;
    sc = uc & SIGN64;

    /* both terms as 128-bit integers with their top bit at position 125 */
    P = mul64x64(ma, mb);
    ep = ea + eb;
    lp = bitlen128(P) - 1;
    P = shl128(P, 125 - lp);
    ep -= 125 - lp;
    C.hi = 0; C.lo = mc;
    lc = bitlen64(mc) - 1;
    C = shl128(C, 125 - lc);
    ec -= 125 - lc;

    /* align the smaller exponent (bits shifted out fold into a sticky bit: the operands have at
       least 20 zero low bits, so a shift of 0 or 1 is exact and cancellation stays exact) */
    if (ep >= ec) { big = P; small = shr128_sticky(C, ep - ec); e = ep; sr = sp; }
    else { big = C; small = shr128_sticky(P, ec - ep); e = ec; sr = sc; }
    if (sp == sc) {
        R = add128(big, small);
    } else {
        int k = cmp128(big, small);  /* small > big only when the exponents are equal */
        if (k == 0) return 0.0;      /* exact cancellation: +0 in round to nearest */
        if (k > 0) R = sub128(big, small);
        else { R = sub128(small, big); sr = (sr == sp) ? sc : sp; }
    }

    /* round R * 2^e to a double */
    L = bitlen128(R) - 1;
    E = e + L;
    if (E > 1023) return u2d(sr | 0x7ff0000000000000ull);
    drop = L - 52;
    if (E < -1022) drop += -1022 - E;
    if (drop <= 0) {
        m = shl128(R, -drop).lo;
    } else {
        int guard = bit128(R, drop - 1), sticky = below128(R, drop - 1);
        if (drop >= 128) m = 0;
        else if (drop >= 64) m = R.hi >> (drop - 64);
        else m = (R.lo >> drop) | (R.hi << (64 - drop));
        if (guard && (sticky || (m & 1))) m++;
    }
    if (E >= -1022) {
        if (m >> 53) { m >>= 1; E++; }
        if (E > 1023) return u2d(sr | 0x7ff0000000000000ull);
        return u2d(sr | ((uint64_t)(E + 1023) << 52) | (m & 0x000fffffffffffffull));
    }
    return u2d(sr | m);             /* subnormal, or the smallest normal after rounding up */
}

#define FMA idwr_fma

/* cvtpd2dq / cvttpd2dq for one lane */
static int32_t cvtpd2dq(double x)
{
    double r;
    if (!(x > -2147483648.5 && x < 2147483647.5)) return INT32_MIN;
    /* round half to even: x + 1.5*2^52 - 1.5*2^52 is exact for |x| < 2^51 */
    r = (x + 6755399441055744.0) - 6755399441055744.0;
    if (r < -2147483648.0 || r > 2147483647.0) return INT32_MIN;
    return (int32_t)r;
}

static int32_t cvttpd2dq(double x)
{
    if (!(x > -2147483649.0 && x < 2147483648.0)) return INT32_MIN;
    return (int32_t)x;
}

/* ---------------------------------------------------------------------------------------------
 * powf (0x1800a9f20; FMA3 code at 0x1800aa6d6)
 * ------------------------------------------------------------------------------------------- */

#define C_THIRD     0x3fd5555555555555ull
#define C_SIXTH     0x3fc5555555555555ull
#define C_LN2       (0x3fe62e42fefa39efull + NC(1))
#define C_LN2_64    0x3f862e42fefa39efull
#define C_64_LN2    0x40571547652b82feull
#define C_OVF       0x40562e4300000000ull   /* 88.72283935546875 */
#define C_UNF       0xc059d1da00000000ull   /* -103.2789306640625 */
#define C_A0        0x3f89999999bac6d4ull   /* near-one log series */
#define C_A1        0x3f3c8034c85dfff0ull
#define C_B0        0x3fb55555555554e6ull
#define C_B1        0x3f62492307f1519full

/* y (finite, nonzero, |y| < 2^24) an integer? odd? */
static int float_is_int(uint32_t ay)
{
    int e = (int)(ay >> 23) - 127;
    if (e < 0) return 0;
    if (e >= 23) return 1;
    return (ay & ((1u << (23 - e)) - 1)) == 0;
}

static int float_int_is_odd(uint32_t ay)
{
    int e = (int)(ay >> 23) - 127;
    if (e < 0 || e > 23) return 0;
    return (int)((((ay & 0x7fffffu) | 0x800000u) >> (23 - e)) & 1);
}

/* the scaling at 0x1800aa840: paddq of (n >> 6) into the exponent field, then cvtsd2ss */
static float pow_finish(double z, int32_t n, uint32_t sign)
{
    uint64_t adj = (uint64_t)(uint32_t)(n >> 6) << 52;
    float r = (float)u2d(d2u(z) + adj);
    if (NC(7)) r = u2f(f2u(r) + 1);
    return u2f(f2u(r) | sign);
}

float idwr_powf(float x, float y)
{
    uint32_t ux = f2u(x), uy = f2u(y);
    uint32_t ax = ux & 0x7fffffffu, ay = uy & 0x7fffffffu;
    int32_t sx = (int32_t)ux;
    uint32_t sign = 0;
    double xd, yd, u, ylogx, t, nd, r, p, T, z;
    int32_t n;

    if (ay >= 0x7f800000u) {                                    /* 0x1800aaa80 */
        if (ay > 0x7f800000u) {                                 /* y NaN */
            if (ax > 0x7f800000u)
                return u2f((ux == 0xffc00000u ? uy : ux) | 0x400000u);
            if (ux == 0x3f800000u)
                return u2f(ay >= 0x7fc00000u ? 0x3f800000u : 0x3fc00000u);
            return u2f(uy | 0x400000u);
        }
        if (ax > 0x7f800000u) return u2f(ux | 0x400000u);    /* y inf */
        if (ax == 0x3f800000u) return 1.0f;
        if (!(uy >> 31)) return ax < 0x3f800000u ? 0.0f : u2f(0x7f800000u);
        return ax < 0x3f800000u ? u2f(0x7f800000u) : 0.0f;
    }
    if (ay <= 0x3f800000u) {                                    /* 0x1800aab20 */
        if (ay == 0) {
            if (ax <= 0x7f800000u || ax >= 0x7fc00000u) return 1.0f;
            return u2f(ax | 0x400000u);
        }
        if (uy == 0x3f800000u) {
            if (ax > 0x7f800000u) return u2f(ux | 0x400000u);
            return x;
        }
    }
    if (ax >= 0x7f800000u) {                                    /* 0x1800aa9e0: x inf or NaN */
        if (ux == 0x7f800000u) return (int32_t)uy < 0 ? 0.0f : u2f(0x7f800000u);
        if (ux == 0xff800000u) {
            uint32_t mag = (int32_t)uy < 0 ? 0 : 0x7f800000u, s = 0x80000000u;
            if ((ay & 0x7f800000u) > 0x4b000000u || !float_is_int(ay) || !float_int_is_odd(ay)) s = 0;
            return u2f(s | mag);
        }
        return u2f(ux | 0x400000u);
    }
    xd = (double)x;
    yd = (double)y;
    if (sx < 0x3f880000) {                                      /* 0x1800aa870 */
        if (sx <= 0) {
            if (ax == 0) {                                      /* 0x1800aa940 */
                uint32_t mag = (int32_t)uy < 0 ? 0x7f800000u : 0, s = 0x80000000u;
                if ((ay & 0x7f800000u) > 0x4b000000u || !float_is_int(ay) || !float_int_is_odd(ay)) s = 0;
                else s = ux & 0x80000000u;
                return u2f(s | mag);
            }
            if ((ay & 0x7f800000u) <= 0x4b000000u) {
                if (!float_is_int(ay)) return u2f(0xffc00000u);
                if (float_int_is_odd(ay)) sign = 0x80000000u;
            }
        }
        /* 0x1800aa8ab: near 1? */
        u = xd - 1.0;
        if ((d2u(u) & ~SIGN64) < 0x3fb0000000000000ull) {
            /* log(1 + u) by the series in s = 2u/(u + 2): non-FMA code in both libraries */
            double q = u / (u + 2.0), s, qu, s2, s3, s4, s7, p0, p1;
            s = q + q;
            qu = q * u;
            s2 = s * s;
            p0 = s2 * u2d(C_A0) + u2d(C_B0);
            p1 = s2 * u2d(C_A1) + u2d(C_B1);
            s3 = s * s2;
            s4 = s2 * s2;
            s7 = s4 * s3;
            p0 = s3 * p0;
            p1 = s7 * p1;
            p0 = p1 + p0;
            p0 = p0 - qu;
            ylogx = yd * (u + p0);
            /* the FMA3 code jumps into the SSE2 exp at 0x1800aa01e */
            if (ylogx > u2d(C_OVF)) return u2f(0x7f800000u | sign);
            if (!(ylogx > u2d(C_UNF))) return u2f(sign);
            t = ylogx * u2d(C_64_LN2);
            n = cvtpd2dq(t);
            nd = (double)n;
            r = ylogx - nd * u2d(C_LN2_64);
            p = u2d(C_SIXTH) * r + 0.5;
            p = (r * r) * p + r;
            T = u2d(IDWR_M_TWO_JBY64[n & 63]);
            z = p * T + T;
            return pow_finish(z, n, sign);
        }
    }
    {   /* 0x1800aa727: log by table */
        uint64_t bx = d2u(xd), mant = bx & 0x000fffffffffffffull;
        uint32_t idx = (uint32_t)(mant >> 44) + (uint32_t)((mant >> 43) & 1);
        double F = u2d((uint64_t)(idx | 0x3fe00u) << 44);
        double f = u2d(mant | 0x3fe0000000000000ull);
        int32_t e = (int32_t)((bx & 0x7ff0000000000000ull) >> 52) - 0x3ff;
        double rr = (F - f) * u2d(IDWR_M_LOG_F_INV[idx]);
        double pl = FMA(rr, u2d(C_THIRD), 0.5);
        double l;
        pl = FMA(rr, pl, 1.0);
        pl = rr * pl;
        l = (double)e * u2d(C_LN2);
        l = l + u2d(IDWR_M_LOG_TABLE[idx]);
        l = l - pl;
        ylogx = yd * l;
    }
    /* 0x1800aa7bd: exp */
    if (ylogx > u2d(C_OVF)) return u2f(0x7f800000u | sign);
    if (!(ylogx > u2d(C_UNF))) return u2f(sign);
    t = ylogx * u2d(C_64_LN2);
    n = cvtpd2dq(t);
    nd = (double)n;
    r = FMA(-nd, u2d(C_LN2_64), ylogx);
    p = FMA(r, u2d(C_SIXTH), 0.5);
    p = FMA(r, p, 1.0);
    p = r * p;
    T = u2d(IDWR_M_TWO_JBY64[n & 63]);
    z = NC(5) ? T * p + T : FMA(T, p, T);
    return pow_finish(z, n, sign);
}

/* ---------------------------------------------------------------------------------------------
 * Payne-Hanek reduction of a large |x| (>= 2^31 for tanf, >= 2e7 for tan): the integer code at
 * 0x1800ad856 (inline in tanf) and 0x1800ab710 (__remainder_piby2_fma3).
 * Gives the fraction of |x| * 2/pi nearest 0, as a double hi (and a second double lo for tan),
 * and the region. x86 semantics kept: shift counts mod 64, bsr of 0 leaves its destination.
 * ------------------------------------------------------------------------------------------- */

/* the unaligned little-endian qword at byte offset o of __L_2_by_pi_bits (zero past its end,
   as in the DLL) */
static uint64_t load_2bypi(int64_t o)
{
    int64_t k = o >> 3;
    int s = (int)(o & 7) * 8;
    uint64_t lo = IDWR_M_TWO_BY_PI[k], hi = k + 1 < 19 ? IDWR_M_TWO_BY_PI[k + 1] : 0;
    return s ? (lo >> s) | (hi << (64 - s)) : lo;
}

/* bsr: index of the top set bit; 0 leaves the destination as it was */
static int64_t bsr64(uint64_t x, int64_t prev)
{
    int64_t n = -1;
    if (!x) return prev;
    while (x) { n++; x >>= 1; }
    return n;
}

#define SHL(v, c) ((v) << ((c) & 63))
#define SHR(v, c) ((v) >> ((c) & 63))

static void reduce_large(uint64_t xbits, uint64_t *hi_out, uint64_t *lo_out, int *region)
{
    int64_t r11, r10i, c;
    uint64_t rcx, rax, r8, r9, r10, rdx, cf;
    u128 p;

    r11 = (int64_t)(xbits >> 52) - 0x3ff;
    r10i = 0x86 - (int64_t)((uint64_t)r11 >> 3);
    rcx = (xbits & 0x000fffffffffffffull) | 0x0010000000000000ull;
    p = mul64x64(load_2bypi(r10i), rcx);
    r8 = p.lo; r10 = p.hi;
    p = mul64x64(load_2bypi(r10i + 8), rcx);
    r11 &= 7;
    rax = p.lo + r10;
    rdx = p.hi + (rax < r10);
    r9 = rax; r10 = rdx;
    r10 += load_2bypi(r10i + 16) * rcx;
    c = 0x36 - r11;
    rax = SHR(r10, c);
    cf = SHR(r10, c - 1) & 1;
    rdx = 0;
    if (cf) { r10 = ~r10; r9 = ~r9; r8 = ~r8; rdx = SIGN64; }
    rax = (rax + cf) & 3;
    *region = (int)rax;
    c = r11 + 10;
    r10 = SHR(SHL(r10, c), c);
    c -= 64;
    r11 = c;
    {
        int64_t b = bsr64(r10, c);
        int64_t rc;
        if (!r10) {
            r10 = r9; r9 = r8; r8 = 0;
            b = bsr64(r10, b);
            r11 -= 64;
        }
        r11 += b;
        rc = b - 0x34;
        if (rc < 0) {
            rc = -rc;
            rax = r9;
            r10 = SHL(r10, rc);
            r9 = SHL(r9, rc);
            r10 |= SHR(rax, 64 - rc);
            r8 = SHR(r8, 64 - rc);
            r9 |= r8;
        } else if (rc > 0) {
            r8 = r10;
            r10 = SHR(r10, rc);
            r9 = SHR(r9, rc);
            r8 = SHL(r8, 64 - rc);
            r9 |= r8;
        }
        r11 += 0x3ff;
        r10 &= ~(1ull << 52);
        r10 |= rdx;
        r10 |= (uint64_t)r11 << 52;
        *hi_out = r10;
        if (lo_out) {   /* 0x1800ab86e */
            int64_t s = 64 - bsr64(r9, (int64_t)((uint64_t)r11 << 52));
            r9 = SHL(r9, s);
            r9 >>= 12;
            s += 0x34;
            r9 |= rdx;
            r9 |= (uint64_t)(r11 - s) << 52;
            *lo_out = r9;
        }
    }
}

/* ---------------------------------------------------------------------------------------------
 * tanf (0x1800ad740; FMA3 code inline, SSE2 code tanf_sse2 at 0x1800ad420)
 * ------------------------------------------------------------------------------------------- */

#define TF_N0     (0x3fd8a8b0da56cb17ull + NC(2))
#define TF_N1     0xbf919dba6efd6aadull
#define TF_D0     0x3ff27e84a3e73a2eull
#define TF_D1     0xbfe07266d7b3511bull
#define TF_D2     0x3f92e29003c692d9ull
#define TF_2PI    0x3fe45f306dc9c883ull
#define TF_PIO2_1 0x3ff921fb54400000ull
#define TF_PIO2_T 0x3dd0b4611a626331ull
#define TF_PIO2   0x3ff921fb54442d18ull

static double tanf_poly(double r)
{
    double x2 = r * r, num, den, x3;
    num = FMA(x2, u2d(TF_N1), u2d(TF_N0));
    den = FMA(x2, u2d(TF_D2), u2d(TF_D1));
    den = FMA(x2, den, u2d(TF_D0));
    num = num / den;
    x3 = x2 * r;
    return NC(6) ? x3 * num + r : FMA(x3, num, r);
}

float idwr_tanf(float x)
{
    uint32_t ux = f2u(x), ax = ux & 0x7fffffffu;
    double xd, r, t;
    uint64_t r9;
    int region;

    if (ax >= 0x7f800000u) {           /* _tanf_special: NaN quieted; tan(inf) = -nan(ind) */
        if (ax > 0x7f800000u) return u2f(ux | 0x400000u);
        return u2f(0xffc00000u);
    }
    xd = (double)x;
    r9 = d2u(xd) & ~SIGN64;
    if (ax <= 0x3f490fdbu) {
        if (ax < 0x39000000u) {
            double x2, x3;
            if (ax < 0x32000000u) return x;
            x2 = xd * xd;
            x3 = x2 * xd;
            return (float)FMA(x3, u2d(C_THIRD), xd);
        }
        return (float)tanf_poly(xd);
    }
    if ((int64_t)r9 < (int64_t)0x41e921fb40000000ull) {   /* 3373259264.0 */
        double ad = u2d(r9), nd, rh, rt;
        int32_t n = cvttpd2dq(FMA(ad, u2d(TF_2PI), 0.5));
        region = n & 3;
        nd = (double)n;
        rh = FMA(-nd, u2d(TF_PIO2_1), ad);
        rt = NC(9) ? 0.0 : nd * u2d(TF_PIO2_T);
        r = rh - rt;
    } else {
        uint64_t hi;
        reduce_large(r9, &hi, 0, &region);
        r = u2d(hi) * u2d(TF_PIO2);
    }
    t = tanf_poly(r);
    if (region & 1) t = -1.0 / t;
    t = u2d(d2u(t) ^ (d2u(xd) & SIGN64));
    return NC(8) ? u2f(f2u((float)t) + 1) : (float)t;
}

/* ---------------------------------------------------------------------------------------------
 * tan (0x1800acc70; FMA3 code at 0x1800acef0)
 * ------------------------------------------------------------------------------------------- */

#define T_N0      0x3fd7d50f6638564aull
#define T_N1      (0xbf977c24c7569abbull + NC(3))
#define T_N2      0x3f2d5daf289c385aull
#define T_D0      0x3ff1dfcb8caa40b8ull
#define T_D1      0xbfe08046499eb90full
#define T_D2      0x3f9ab0f4f80a0acfull
#define T_D3      0xbf2e7517ef6d98f8ull
#define T_PI4_HI  0x3fe921fb54442d18ull
#define T_PI4_LO  0x3c81a62633145c06ull
#define T_068     0x3fe5c28f5c28f5c3ull
#define T_SMALL   0x3e40000000000000ull   /* 2^-27 */
#define T_LARGE   0x417312d000000000ull   /* 2e7 */
#define T_HALFMASK 0xffffffff00000000ull
/* __remainder_piby2_fma3_bdl */
#define B_2PI     0x3fe45f306dc9c883ull
#define B_PIO2_1  0x3ff921fb54442d18ull
#define B_PIO2_2  0x3c91a62633145c00ull
#define B_PIO2_3  0x397b839a252049c0ull
#define B_SHIFT   0x4338000000000000ull
/* __remainder_piby2_fma3 */
#define F_PIO2    0x3ff921fb54442d18ull
#define F_SPLIT   0xfffffffff8000000ull
#define F_PIO2_H  0x3ff921fb50000000ull
#define F_PIO2_M  0x3e5110b460000000ull
#define F_PIO2_L  0x3c91a62633145c06ull

static void rem_piby2_bdl(double x, double *r, double *rr, int *region)
{
    double n, rhead, p2, perr, r1, t4, r2, w;
    n = FMA(x, u2d(B_2PI), u2d(B_SHIFT)) - u2d(B_SHIFT);
    *region = cvttpd2dq(n) & 3;
    rhead = FMA(-n, u2d(B_PIO2_1), x);
    p2 = n * u2d(B_PIO2_2);
    perr = FMA(n, u2d(B_PIO2_2), -p2);
    r1 = rhead - p2;
    t4 = (rhead - r1) - p2;
    r2 = FMA(-n, u2d(B_PIO2_2), rhead);
    w = r1 - r2;
    w = w + t4;
    w = w - perr;
    *r = r2;
    *rr = FMA(-n, u2d(B_PIO2_3), w);
}

static void rem_piby2_large(double x, double *r, double *rr, int *region)
{
    uint64_t hb, lb;
    double h, l, hh, hl, p, s, q;
    reduce_large(d2u(x), &hb, &lb, region);
    h = u2d(hb);
    l = u2d(lb);
    hh = u2d(hb & F_SPLIT);
    hl = h - hh;
    p = h * u2d(F_PIO2);
    s = hh * u2d(F_PIO2_H);
    s = s - p;
    s = FMA(hl, u2d(F_PIO2_H), s);
    s = FMA(hh, u2d(F_PIO2_M), s);
    s = FMA(hl, u2d(F_PIO2_M), s);
    q = l * u2d(F_PIO2);
    q = FMA(h, u2d(F_PIO2_L), q);
    s = s + q;
    *r = p + s;
    *rr = (p - *r) + s;
}

static double tan_num(double z)
{
    double nn = FMA(z, u2d(T_N2), u2d(T_N1));
    return FMA(z, nn, u2d(T_N0));
}

static double tan_den(double z)
{
    double d = FMA(z, u2d(T_D3), u2d(T_D2));
    d = FMA(z, d, u2d(T_D1));
    return FMA(z, d, u2d(T_D0));
}

double idwr_tan(double x)
{
    uint64_t ux = d2u(x), ax = ux & ~SIGN64;
    double flag = 0.0, xx = x, r, rr, z, q, qq, t, res;
    int region;

    if (ax < T_PI4_HI) {
        if (ax > T_068) {
            if (x >= 0.0) { flag = 1.0; xx = (u2d(T_PI4_HI) - x) + u2d(T_PI4_LO); }
            else { flag = -1.0; xx = (u2d(T_PI4_HI) + x) + u2d(T_PI4_LO); }
        } else if ((int64_t)ax < (int64_t)T_SMALL) {
            return x;
        }
        z = xx * xx;
        q = tan_num(z) / tan_den(z);
        q = q * z;
        t = FMA(xx, q, xx);
        if (flag == 0.0) return t;
        q = t / (t + 1.0);
        return FMA(-q, 2.0, 1.0) * flag;
    }
    if (ax >= 0x7ff0000000000000ull) {  /* _tan_special: NaN quieted; tan(inf) = -nan(ind) */
        if (ax > 0x7ff0000000000000ull) return u2d(ux | 0x0008000000000000ull);
        return u2d(0xfff8000000000000ull);
    }
    if ((int64_t)ax >= (int64_t)T_LARGE) rem_piby2_large(u2d(ax), &r, &rr, &region);
    else rem_piby2_bdl(u2d(ax), &r, &rr, &region);
    if (r > u2d(T_068)) {
        flag = 1.0;
        r = (u2d(T_PI4_HI) - r) + (u2d(T_PI4_LO) - rr);
        rr = 0.0;
    } else if (!(r >= -u2d(T_068))) {
        flag = -1.0;
        r = (u2d(T_PI4_HI) + r) + (u2d(T_PI4_LO) + rr);
        rr = 0.0;
    }
    z = FMA(rr * r, 2.0, r * r);
    q = tan_num(z) / tan_den(z);
    q = q * z;
    qq = FMA(r, q, rr);
    t = r + qq;
    if (flag != 0.0) {
        if (region & 1) { q = t / (t - 1.0); res = FMA(q, 2.0, -1.0) * flag; }
        else { q = t / (t + 1.0); res = FMA(-q, 2.0, 1.0) * flag; }
    } else if (region & 1) {
        double th = u2d(d2u(t) & T_HALFMASK);
        double tl = qq - (th - r);
        double z1 = -1.0 / t;
        double zh = u2d(d2u(z1) & T_HALFMASK);
        double a = FMA(zh, th, 1.0);
        a = FMA(tl, zh, a);
        res = FMA(z1, a, zh);
    } else {
        res = t;
    }
    return u2d(d2u(res) ^ (ux & SIGN64));
}

/* ---------------------------------------------------------------------------------------------
 * sinf (0x1800ac360) and cosf (0x1800a7f70): FMA3 code inline; SSE2 code sinf_sse2 (0x1800abff0)
 * and cosf_sse2 (0x1800a7cc0). In double, Taylor polynomials on [-pi/4, pi/4]: x + x^3 (S1 + ..
 * + S4 x^6) for sin, 1 - x^2/2 + x^4 (C1 + .. + C4 x^6) for cos (tables __Lsinfarray,
 * __Lcosfarray). The two functions reduce with different cut-offs: sinf Cody-Waite (2-part pi/2,
 * as tanf) below 0x4b800456 and the inline integer Payne-Hanek above; cosf Cody-Waite below
 * 0x4f490fdb and __remainder_piby2_fma3 (tan's) above, of which it uses only the head.
 * ------------------------------------------------------------------------------------------- */

/* -1/3!, 1/5!, -1/7!, 1/9! and -1/2!, 1/4!, -1/6!, 1/8!, -1/10! */
static double SF(int i)
{
    double c = u2d(IDWR_M_SINF_POLY[i] + (i == 0 ? NC(10) : 0));
    return NC(16) && i == 0 ? (double)(float)c : c;
}

static double CF(int i)
{
    double c = u2d(IDWR_M_COSF_POLY[i] + (i == 1 ? NC(15) : 0));
    return NC(17) && i == 1 ? (double)(float)c : c;
}

/* sin on [-pi/4, pi/4]: sinf 0x1800ac3e0 and 0x1800ac5f4, cosf 0x1800a80ec */
static double sinf_poly(double r)
{
    double x2 = r * r, p, x3;
    p = FMA(x2, SF(3), SF(2));
    p = FMA(x2, p, SF(1));
    p = FMA(x2, p, SF(0));
    x3 = r * x2;
    return FMA(p, x3, r);
}

/* cos on [-pi/4, pi/4]: sinf forms 1 - x^2/2 with an fma (0x1800ac630), cosf with a multiply and
   a subtraction (0x1800a7ff0, 0x1800a8120); x^2/2 is exact, so both give the same bits */
static double cosf_poly(double r, int fused_head)
{
    double x2 = r * r, h, p, x4;
    h = fused_head ? FMA(x2, CF(0), 1.0) : 1.0 - x2 * 0.5;
    p = FMA(x2, CF(4), CF(3));
    p = FMA(x2, p, CF(2));
    p = FMA(x2, p, CF(1));
    x4 = x2 * x2;
    return FMA(p, x4, h);
}

/* Cody-Waite with tanf's 2-part pi/2 (sinf 0x1800ac42c, cosf 0x1800a805c): the head only */
static double sincosf_cw(double ad, int *region)
{
    int32_t n = cvttpd2dq(FMA(ad, u2d(TF_2PI), 0.5));
    double nd = (double)n, rh, rt;
    *region = n & 3;
    rh = FMA(-nd, u2d(TF_PIO2_1), ad);
    rt = NC(11) ? 0.0 : nd * u2d(TF_PIO2_T);
    return rh - rt;
}

/* _sinf_special, _cosf_special: NaN quieted; sin(inf) = cos(inf) = -nan(ind) */
static float sincosf_special(uint32_t ux)
{
    if ((ux & 0x7fffffffu) > 0x7f800000u) return u2f(ux | 0x400000u);
    return u2f(0xffc00000u);
}

float idwr_sinf(float x)
{
    uint32_t ux = f2u(x), ax = ux & 0x7fffffffu;
    double xd, ad, r, t;
    int region;

    if (ax >= 0x7f800000u) return sincosf_special(ux);
    xd = (double)x;
    if (ax <= 0x3f490fdbu) {
        if (ax < 0x3c000000u) {
            double x2, x3;
            if (ax < 0x39000000u) return x;
            x2 = xd * xd;
            x3 = x2 * xd;
            return (float)FMA(-x3, u2d(C_SIXTH), xd);
        }
        return (float)sinf_poly(xd);
    }
    ad = u2d(d2u(xd) & ~SIGN64);
    if (ax < 0x4b800456u) {
        r = sincosf_cw(ad, &region);
    } else {
        uint64_t hi;
        reduce_large(d2u(ad), &hi, 0, &region);
        r = u2d(hi) * u2d(TF_PIO2);
    }
    t = (region & 1) ? cosf_poly(r, 1) : sinf_poly(r);
    /* 0x1800ac675: negated in quadrants 2 and 3, and for a negative x */
    t = u2d(d2u(t) ^ ((region & 2) ? SIGN64 : 0) ^ (d2u(xd) & SIGN64));
    return (float)t;
}

float idwr_cosf(float x)
{
    uint32_t ux = f2u(x), ax = ux & 0x7fffffffu;
    double xd, ad, r, t;
    int region;

    if (ax >= 0x7f800000u) return sincosf_special(ux);
    xd = (double)x;
    if (ax <= 0x3f490fdbu) {
        if (ax < 0x3c000000u) {
            double h;
            if (ax < 0x39000000u) return 1.0f;
            h = xd * 0.5;
            return (float)FMA(-xd, h, 1.0);
        }
        return (float)cosf_poly(xd, 0);
    }
    ad = u2d(d2u(xd) & ~SIGN64);
    if (ax < 0x4f490fdbu) {
        r = sincosf_cw(ad, &region);
    } else {
        double rr;
        rem_piby2_large(ad, &r, &rr, &region);
    }
    t = (region & 1) ? sinf_poly(r) : cosf_poly(r, 0);
    /* 0x1800a8168: ((region + 1) >> 1) << 63, negated in quadrants 1 and 2 */
    if ((region + 1) & 2) t = u2d(d2u(t) ^ SIGN64);
    return (float)t;
}

/* ---------------------------------------------------------------------------------------------
 * sin (0x1800aba70; FMA3 code at 0x1800abdd0) and cos (0x1800a7730; FMA3 code at 0x1800a7ac0).
 * Minimax polynomials (__Lsinarray, __Lcosarray) on [-pi/4, pi/4]; the reduction is tan's:
 * __remainder_piby2_fma3_bdl below 2e7, __remainder_piby2_fma3 above, and the polynomials then
 * carry the reduction's tail rr.
 * ------------------------------------------------------------------------------------------- */

#define SD(i) u2d(IDWR_M_SIN_POLY[i] + ((i) == 1 ? NC(12) : 0))   /* x^3 .. x^13 */
#define CD(i) u2d(IDWR_M_COS_POLY[i] + ((i) == 1 ? NC(13) : 0))   /* x^4 .. x^14 */
#define SC_2M13   0x3f20000000000000ull   /* 2^-13 */

/* sin(r + rr), r reduced (sin 0x1800abebf, cos 0x1800a7c10) */
static double sin_tail(double r, double rr)
{
    double x2 = r * r, p, x3, t;
    p = FMA(x2, SD(5), SD(4));
    p = FMA(x2, p, SD(3));
    p = FMA(x2, p, SD(2));
    p = FMA(x2, p, SD(1));
    x3 = r * x2;
    t = x3 * p;
    t = rr * 0.5 - t;
    t = x2 * t;
    t = t - rr;
    t = FMA(-x3, SD(0), t);
    return r - t;
}

/* cos(r + rr) (sin 0x1800abf20, cos 0x1800a7ba6): 1 - x^2/2 split into w and its error c */
static double cos_tail(double r, double rr)
{
    double x2 = r * r, h, w, c, x4, p;
    h = x2 * 0.5;
    w = 1.0 - h;
    c = 1.0 - w;
    c = c - h;
    c = FMA(-r, rr, c);
    x4 = x2 * x2;
    p = FMA(x2, CD(5), CD(4));
    p = FMA(x2, p, CD(3));
    p = FMA(x2, p, CD(2));
    p = FMA(x2, p, CD(1));
    p = FMA(x2, p, CD(0));
    p = FMA(x4, p, c);
    return p + w;
}

static void sincos_reduce(double ad, double *r, double *rr, int *region)
{
    if (d2u(ad) >= T_LARGE) {
        rem_piby2_large(ad, r, rr, region);
    } else {
        rem_piby2_bdl(ad, r, rr, region);
        if (NC(14)) *rr = 0.0;   /* negative control: the reduction's tail dropped */
    }
}

/* _sin_special, _cos_special: NaN quieted; sin(inf) = cos(inf) = -nan(ind) */
static double sincos_special(uint64_t ux)
{
    if ((ux & ~SIGN64) > 0x7ff0000000000000ull) return u2d(ux | 0x0008000000000000ull);
    return u2d(0xfff8000000000000ull);
}

double idwr_sin(double x)
{
    uint64_t ux = d2u(x), ax = ux & ~SIGN64;
    double r, rr, t;
    int region;

    if (ax < T_PI4_HI) {
        if (ax >= SC_2M13) {                   /* 0x1800abe40 */
            double x2 = x * x, p, x3;
            p = FMA(x2, SD(5), SD(4));
            p = FMA(x2, p, SD(3));
            p = FMA(x2, p, SD(2));
            p = FMA(x2, p, SD(1));
            x3 = x * x2;
            p = FMA(x2, p, SD(0));
            return FMA(x3, p, x);
        }
        if (ax >= T_SMALL) {                   /* 0x1800abe20 */
            double x3 = (x * x) * x;
            return FMA(-x3, u2d(C_SIXTH), x);
        }
        return x;
    }
    if (ax >= 0x7ff0000000000000ull) return sincos_special(ux);
    sincos_reduce(u2d(ax), &r, &rr, &region);
    t = (region & 1) ? cos_tail(r, rr) : sin_tail(r, rr);
    /* 0x1800abf83: negated in quadrants 2 and 3, and for a negative x */
    return u2d(d2u(t) ^ ((region & 2) ? SIGN64 : 0) ^ (ux & SIGN64));
}

double idwr_cos(double x)
{
    uint64_t ux = d2u(x), ax = ux & ~SIGN64;
    double r, rr, t;
    int region;

    if (ax <= T_PI4_HI) {
        if (ax >= SC_2M13) {                   /* 0x1800a7b25 */
            double x2 = x * x, p;
            p = FMA(x2, CD(5), CD(4));
            p = FMA(x2, p, CD(3));
            p = FMA(x2, p, CD(2));
            p = FMA(x2, p, CD(1));
            p = FMA(x2, p, CD(0));
            p = FMA(x2, p, -0.5);
            return FMA(x2, p, 1.0);
        }
        if (ax >= T_SMALL) {                   /* 0x1800a7b07 */
            double h = x * 0.5;
            return FMA(-h, x, 1.0);
        }
        return 1.0;
    }
    if (ax >= 0x7ff0000000000000ull) return sincos_special(ux);
    sincos_reduce(u2d(ax), &r, &rr, &region);
    t = (region & 1) ? sin_tail(r, rr) : cos_tail(r, rr);
    /* 0x1800a7c69: negated in quadrants 1 and 2 */
    if ((region + 1) & 2) t = u2d(d2u(t) ^ SIGN64);
    return t;
}

/* ---------------------------------------------------------------------------------------------
 * _hypot (0x18004e790): sqrt(x*x + y*y) in double, scaled by 2^-600 or 2^600 at the extremes.
 * Not correctly rounded; the same rounding steps here. sqrt is IEEE-exact everywhere.
 * ------------------------------------------------------------------------------------------- */

/* IEEE 754 square root, correctly rounded: sqrtsd on x86-64, fsqrt on aarch64. The compilers
   inline it (no libm call for a nonnegative argument). */
#if defined(__GNUC__) || defined(__clang__)
#define sqrt_exact(v) __builtin_sqrt(v)
#else
#include <math.h>
#define sqrt_exact(v) sqrt(v)
#endif

double idwr_hypot(double x, double y)
{
    uint64_t ax = d2u(x) & ~SIGN64, ay = d2u(y) & ~SIGN64;
    int ex = (int)(ax >> 52), ey = (int)(ay >> 52), scale = 0;
    double a, b, s;

    /* NaN: the DLL returns x*x + y*y, which on SSE is the first NaN operand, quieted. Written out
       here: a compiler may swap the operands of a commutative add. */
    if (ex == 0x7ff) {
        if (ax & 0x000fffffffffffffull) {
            if (ey == 0x7ff && !(ay & 0x000fffffffffffffull)) return u2d(0x7ff0000000000000ull);
            return u2d(d2u(x) | 0x0008000000000000ull);
        }
        return u2d(0x7ff0000000000000ull);
    }
    if (ey == 0x7ff) {
        if (!(ay & 0x000fffffffffffffull)) return u2d(0x7ff0000000000000ull);
        return u2d(d2u(y) | 0x0008000000000000ull);
    }
    a = u2d(ax);
    b = u2d(ay);
    if (ax == 0) return b;
    if (ay == 0) return a;
    if ((unsigned)(ex - ey + 54) > 108u) return a + b;
    if (ex > 0x5f3 || ey > 0x5f3) {
        a = u2d(ax + 0xda80000000000000ull);
        b = u2d(ay + 0xda80000000000000ull);
        scale = 600;
    } else if (ex < 0x20b || ey < 0x20b) {
        if (ex) a = u2d(ax + 0x2580000000000000ull);
        else a = u2d(ax + 0x2590000000000000ull) + u2d(0xa590000000000000ull);
        if (ey) b = u2d(ay + 0x2580000000000000ull);
        else b = u2d(ay + 0x2590000000000000ull) + u2d(0xa590000000000000ull);
        scale = -600;
    }
    s = a * a;
    s = s + b * b;
    s = sqrt_exact(s);
    if (NC(4)) s = u2d(d2u(s) + 1);
    if (scale) s = s * u2d((uint64_t)(scale + 0x3ff) << 52);
    return s;   /* > DBL_MAX: _handle_error returns +inf, which s already is */
}

/* =============================================================================================
 * pow, expf, logf, asinf, acosf, atanf, atan2f
 * ========================================================================================== */

/* fmaf, rounded once (vfmadd*ss): the product of two floats is exact in double; the sum is
   rounded to odd in double (TwoSum gives its exact error), and a value rounded to odd at 53 bits
   rounds to 24 bits as the exact value would. The ports below pass only finite operands. */
float idwr_fmaf(float a, float b, float c)
{
    double p = (double)a * (double)b, cd = (double)c, s = p + cd, bb, e;
    uint64_t u;
    if ((d2u(s) & 0x7ff0000000000000ull) == 0x7ff0000000000000ull) return (float)s;
    bb = s - p;
    e = (p - (s - bb)) + (cd - bb);
    u = d2u(s);
    if (e != 0 && !(u & 1) && !NC(31)) u = ((e > 0) == (s > 0)) ? u + 1 : u - 1;
    return (float)u2d(u);
}

#define FMAF idwr_fmaf

/* Constants the code below reads from the DLL, by name: gen_math_tables.py --dll compares each
   with the bytes at its address in the DLL (data, or the immediate of the instruction). */
#define M_PIO2    0x3ff921fb54442d18ull   /* pi/2 */
#define M_PI_     0x400921fb54442d18ull   /* pi */
#define M_PIO4    0x3fe921fb54442d18ull   /* pi/4 */
#define FL_PIO2    0x3fc90fdbu             /* pi/2, float */
#define FL_MPIO2   0xbfc90fdbu
#define FL_PI      0x40490fdbu
#define FL_MPI     0xc0490fdbu
#define FL_PIO4    0x3f490fdbu
#define FL_MPIO4   0xbf490fdbu
#define FL_3PIO4   0x4016cbe4u
#define FL_M3PIO4  0xc016cbe4u

#if defined(__GNUC__) || defined(__clang__)
#define sqrtf_exact(v) __builtin_sqrtf(v)
#else
#define sqrtf_exact(v) sqrtf(v)
#endif

/* ---------------------------------------------------------------------------------------------
 * pow (0x1800766e0 picks pow_fma at 0x18005e990): ARM optimized-routines' pow (log with a
 * 128-entry table, exp with a 256-entry table, both copied from the DLL), with Microsoft's early
 * returns for y == 1 and x == +-1 and its own __math_* helpers. FMA where the DLL has vfmadd.
 * ------------------------------------------------------------------------------------------- */

#define P_LN2HI   0x3fe62e42fefa3800ull
#define P_LN2LO   0x3d2ef35793c76730ull
#define P_A0      0xbfe0000000000000ull   /* -0.5 */
#define P_A1N     0x3fe5555555555560ull   /* -A1 */
#define P_A2      (0x3fe0000000000006ull + NC(18))
#define P_A3      0x3fe999999959554eull
#define P_A4N     0x3fe555555529a47aull   /* -A4 */
#define P_A5N     0x3ff2495b9b4845e9ull   /* -A5 */
#define P_A6      0x3ff0002b8b263fc3ull
#define E_INVLN2N 0x40771547652b82feull   /* 256 / ln 2 */
#define E_SHIFT   0x4238000000008000ull   /* 0x1.8p36 plus half a unit of bit 16 */
#define E_LN2HIN  0x3f662e42fefc0000ull
#define E_LN2LON  0x3d2c610ca86c3899ull
#define E_C2      0x3fdffffffffffdbdull
#define E_C3      0x3fc555555555543cull
#define E_C4      0x3fa55555cf16e1edull
#define E_C5      0x3f81111167a4b553ull
#define P_2P52    0x4330000000000000ull   /* 2^52: subnormal x */
#define P_NOFF    0xc0196aab00000000ull   /* -OFF, the log table's offset 0x3fe6955500000000 */
#define P_BIAS_HI 0xc0f0000000000000ull   /* -(1009 << 52) */
#define P_BIAS_LO 0x3fe0000000000000ull   /* 1022 << 52 */
#define P_2P1009  0x7f00000000000000ull
#define P_2M1022  0x0010000000000000ull

/* x + y where one is a NaN (addsd: the first NaN operand, quieted), written out */
static double nan_add(double x, double y)
{
    if ((d2u(x) & ~SIGN64) > 0x7ff0000000000000ull) return u2d(d2u(x) | 0x0008000000000000ull);
    if ((d2u(y) & ~SIGN64) > 0x7ff0000000000000ull) return u2d(d2u(y) | 0x0008000000000000ull);
    return x + y;
}

/* y finite and nonzero: 0 not an integer, 1 odd, 2 even */
static int pow_checkint(uint64_t iy)
{
    int e = (int)((iy >> 52) & 0x7ff);
    uint64_t m;
    if (e < 0x3ff) return 0;
    if (e > 0x433) return 2;
    m = 1ull << (0x433 - e);
    if (iy & (m - 1)) return 0;
    return (iy & m) ? 1 : 2;
}

/* __math_oflow, __math_divzero (+-inf) and __math_uflow (+-0: Microsoft's returns a zero) */
static double pow_inf(int sign) { return u2d(sign ? 0xfff0000000000000ull : 0x7ff0000000000000ull); }
static double pow_zero(int sign) { return u2d(sign ? SIGN64 : 0); }

static double pow_result(double r)
{
    if (NC(19)) r = u2d(d2u(r) + 1);
    return r;
}

double idwr_pow(double x, double y)
{
    uint64_t ix = d2u(x), iy = d2u(y), tmp, iz, sbits, kb;
    uint32_t topx = (uint32_t)(ix >> 52), topy = (uint32_t)(iy >> 52), sign_bias = 0, abstop;
    double z, kd, invc, logc, logctail, r, t1, t2, lo1, lo2, lo3, lo4, ar, ar2, ar3, hi, lo, p;
    double q1, q2, q3, lhi, ltail, ehi, elo, kn, rr, r2, tl, tmpv, scale;
    int i, j;

    if (topx - 1 >= 0x7fe || (topy & 0x7ff) - 0x3be >= 0x80) {   /* 0x18005ea0f */
        if (2 * iy - 1 >= 0xffdfffffffffffffull) {                  /* y zero, inf or NaN */
            if (2 * iy == 0)
                return 2 * (ix ^ 0x0008000000000000ull) > 0xfff0000000000000ull ? nan_add(x, y) : 1.0;
            if (ix == 0x3ff0000000000000ull)
                return 2 * (iy ^ 0x0008000000000000ull) > 0xfff0000000000000ull ? nan_add(x, y) : 1.0;
            if (2 * ix > 0xffe0000000000000ull || 2 * iy > 0xffe0000000000000ull) return nan_add(x, y);
            if (2 * ix == 0x7fe0000000000000ull) return 1.0;
            if ((2 * ix < 0x7fe0000000000000ull) == !(iy >> 63)) return 0.0;
            return y * y;
        }
        if (2 * ix - 1 >= 0xffdfffffffffffffull) {                  /* x zero, inf or NaN */
            int nan = (ix & ~SIGN64) > 0x7ff0000000000000ull, sb = 0;
            double x2 = nan ? u2d(ix | 0x0008000000000000ull) : x * x;   /* mulsd: x quieted */
            if ((ix >> 63) && pow_checkint(iy) == 1) { x2 = u2d(d2u(x2) ^ SIGN64); sb = 1; }
            if (2 * ix == 0) return (iy >> 63) ? pow_inf(sb) : x2;     /* __math_divzero */
            return (iy >> 63) && !nan ? 1.0 / x2 : x2;
        }
        if (ix >> 63) {                                              /* finite x < 0 */
            int yint = pow_checkint(iy);
            if (yint == 0) return u2d(0xfff8000000000000ull);        /* __math_invalid: (x-x)/(x-x) */
            if (yint == 1) sign_bias = 0x80000u;
            ix &= ~SIGN64;
            topx &= 0x7ff;
        }
        if ((topy & 0x7ff) - 0x3be >= 0x80) {
            if (ix == 0x3ff0000000000000ull) return 1.0;
            if ((topy & 0x7ff) < 0x3be) return ix > 0x3ff0000000000000ull ? 1.0 + y : 1.0 - y;
            return (ix > 0x3ff0000000000000ull) == (topy < 0x800) ? pow_inf(0) : pow_zero(0);
        }
        if (topx == 0) {                                             /* subnormal x */
            ix = d2u(x * u2d(P_2P52)) & ~SIGN64;
            ix -= 52ull << 52;
        }
    }
    if (iy == 0x3ff0000000000000ull) return x;                       /* 0x18005ecce */
    if (ix == 0x3ff0000000000000ull) return sign_bias ? -1.0 : 1.0;

    /* log_inline (0x18005ed10): log x = lhi + ltail */
    tmp = ix + P_NOFF;
    i = (int)((tmp >> 45) & 127);
    kd = (double)(int32_t)(uint32_t)((uint64_t)((int64_t)tmp >> 52) & 0xffffffffu);
    iz = ix - (tmp & 0xfff0000000000000ull);
    z = u2d(iz);
    invc = u2d(IDWR_M_POW_LOG[4 * i]);
    logc = u2d(IDWR_M_POW_LOG[4 * i + 2]);
    logctail = u2d(IDWR_M_POW_LOG[4 * i + 3]);
    r = FMA(z, invc, -1.0);
    t1 = kd * u2d(P_LN2HI) + logc;
    ar = r * u2d(P_A0);
    ar2 = ar * r;
    lo3 = FMA(r, ar, -ar2);
    lo1 = kd * u2d(P_LN2LO) + logctail;
    t2 = t1 + r;
    lo2 = (t1 - t2) + r;
    lo3 = lo3 + (lo1 + lo2);
    hi = ar2 + t2;
    lo4 = (t2 - hi) + ar2;
    lo = lo3 + lo4;
    q1 = u2d(P_A3) - r * u2d(P_A4N);
    q2 = r * u2d(P_A6) - u2d(P_A5N);
    q3 = r * u2d(P_A2) - u2d(P_A1N);
    p = (q1 + q2 * ar2) * ar2 + q3;
    ar3 = ar2 * r;
    p = p * ar3;
    lo = lo + p;
    lhi = lo + hi;
    ltail = (hi - lhi) + lo;

    /* y log x = ehi + elo */
    ehi = lhi * y;
    abstop = (uint32_t)(d2u(ehi) >> 52) & 0x7ff;
    elo = FMA(lhi, y, -ehi) + ltail * y;

    /* exp_inline (0x18005ee6c) */
    if (abstop - 0x3c9 >= 0x3f) {
        if (abstop - 0x3c9 >= 0x80000000u) {                         /* |y log x| < 2^-54 */
            double one = ehi + 1.0;
            return pow_result(sign_bias ? -one : one);
        }
        if (abstop >= 0x409) return (d2u(ehi) >> 63) ? pow_zero(sign_bias != 0) : pow_inf(sign_bias != 0);
        abstop = 0;                                                  /* large: the special case below */
    }
    kb = d2u(ehi * u2d(E_INVLN2N) + u2d(E_SHIFT)) >> 16;
    kn = (double)(int32_t)(uint32_t)(kb & 0xffffffffu);
    rr = (ehi - kn * u2d(E_LN2HIN)) + kn * u2d(E_LN2LON);
    rr = rr + elo;
    j = (int)(kb & 0xff);
    r2 = rr * rr;
    tl = (rr * u2d(E_C3) + u2d(E_C2)) * r2;
    sbits = (((uint64_t)sign_bias + kb) << 44) + IDWR_M_EXP_TAB[2 * j + 1];
    tmpv = (tl + (rr + u2d(IDWR_M_EXP_TAB[2 * j]))) + (rr * u2d(E_C5) + u2d(E_C4)) * (r2 * r2);
    if (abstop == 0) {                                               /* specialcase (0x18005ef81) */
        double st, yy;
        if (!(kb & 0x80000000u)) {
            scale = u2d(sbits + P_BIAS_HI);
            return pow_result((scale * tmpv + scale) * u2d(P_2P1009));
        }
        sbits += P_BIAS_LO;
        scale = u2d(sbits);
        st = scale * tmpv;
        yy = scale + st;
        if ((d2u(yy) & ~SIGN64) < 0x3ff0000000000000ull) {
            double one = yy < 0 ? -1.0 : 1.0, l, h;
            l = (scale - yy) + st;
            h = one + yy;
            l = l + ((one - h) + yy);
            yy = (l + h) - one;
            if (yy == 0) yy = u2d(sbits & SIGN64);
        }
        return pow_result(yy * u2d(P_2M1022));
    }
    scale = u2d(sbits);
    return pow_result(tmpv * scale + scale);
}

/* ---------------------------------------------------------------------------------------------
 * expf (0x1800a8740; expf_sse2 at 0x1800a85d0 without FMA3): in double, the 2^(j/64) table and
 * a cubic, as powf's exp.
 * ------------------------------------------------------------------------------------------- */

#define EF_BIG    0x42b00000u             /* |x| from which the limits are checked */
#define EF_OVF    0x40c0000000000000ull   /* 8192 = 64 * 128 */
#define EF_UNF    0xc0c2c00000000000ull   /* -9600 */
#define EF_HALF   0x3fe0000000000000ull

float idwr_expf(float x)
{
    uint32_t ux = f2u(x), ax = ux & 0x7fffffffu;
    double xd = (double)x, t, nd, r, r2, p, T, zz;
    int32_t n;
    float res;
    t = xd * u2d(C_64_LN2);
    if (ax >= EF_BIG) {                                         /* 0x1800a87e0 */
        if (ax >= 0x7f800000u) {
            if (ux == 0x7f800000u) return x;
            if (ux == 0xff800000u) return 0.0f;
            return u2f(ux | 0x400000u);
        }
        if (t >= u2d(EF_OVF)) return u2f(0x7f800000u);                    /* _expf_special: +inf */
        if (t < u2d(EF_UNF)) return 0.0f;                                /* _expf_special: +0 */
    }
    n = cvtpd2dq(t);
    nd = (double)n;
    r = FMA(nd, -u2d(C_LN2_64), xd);
    r2 = r * r;
    p = FMA(r, u2d(C_SIXTH + NC(20)), u2d(EF_HALF));
    p = FMA(p, r2, r);
    T = u2d(IDWR_M_TWO_JBY64[n & 63]);
    zz = FMA(p, T, T);
    zz = zz * u2d((uint64_t)((int64_t)((n - (n & 63)) / 64) + 0x3ff) << 52);
    res = (float)zz;
    if (NC(21)) res = u2f(f2u(res) + 1);
    return res;
}

/* ---------------------------------------------------------------------------------------------
 * logf (0x1800a9d40; logf_sse2 at 0x1800a9b80 without FMA3): AMD's logf in single precision:
 * 128-entry lead/tail tables, a series in s = 2u/(u + 2) for |x - 1| < 1/16.
 * ------------------------------------------------------------------------------------------- */

#define LF_THIRD  0x3eaaaaabu
#define LF_LN2LD  0x3f317000u
#define LF_LN2TL  0x3805fdf4u
#define LF_CA     (0x3daaaaabu + NC(22))
#define LF_CB     0x3c4ccccdu
#define LF_NEAR   0x3d800000u             /* 1/16 */
#define LF_TWO    0x40000000u
#define LF_HALF   0x3f000000u

static float logf_result(float r)
{
    if (NC(23)) r = u2f(f2u(r) + 1);
    return r;
}

float idwr_logf(float x)
{
    uint32_t ux = f2u(x), ax = ux & 0x7fffffffu, mant, e, idx;
    float xf, ef;
    if (ax >= 0x7f800000u) {
        if (ux == 0x7f800000u) return x;
        if (ux == 0xff800000u) return u2f(0xffc00000u);
        return u2f(ux | 0x400000u);
    }
    if (!(x > 0.0f)) return x == 0.0f ? u2f(0xff800000u) : u2f(0xffc00000u);   /* _logf_special */
    mant = ax & 0x7fffffu;
    e = ax >> 23;
    if (e == 0) {                                                    /* subnormal: 0x1800a9e80 */
        uint32_t b = f2u(u2f(mant | 0x3f800000u) - 1.0f);
        ef = (float)((int32_t)(b >> 23) - 0xfd);
        mant = b & 0x7fffffu;
        xf = u2f(mant);
    } else {
        ef = (float)((int32_t)e - 0x7f);
        xf = x;
    }
    if (u2f(f2u(xf - 1.0f) & 0x7fffffffu) < u2f(LF_NEAR)) {               /* near one: 0x1800a9e30 */
        float u = xf - 1.0f, q = u / (u2f(LF_TWO) + u), uq = u * q, s, s2, s3, pp;
        s = q + q;
        s2 = s * s;
        s3 = s * s2;
        pp = FMAF(s2, u2f(LF_CB), u2f(LF_CA));
        pp = FMAF(s3, pp, -uq);
        return logf_result(u + pp);
    }
    {
        float f = u2f(mant | 0x3f000000u), F, rr, pl, r2, t3, t0;
        idx = (mant >> 16) + ((mant >> 15) & 1);
        F = u2f((idx << 16) | 0x3f000000u);
        rr = (F - f) * u2f(IDWR_M_LOGF_INV[idx]);
        pl = FMAF(rr, u2f(LF_THIRD), u2f(LF_HALF));
        r2 = rr * rr;
        rr = FMAF(pl, r2, rr);
        t3 = FMAF(ef, u2f(LF_LN2TL), -rr);
        t0 = FMAF(ef, u2f(LF_LN2LD), u2f(IDWR_M_LOGF_LEAD[idx]));
        t3 = t3 + u2f(IDWR_M_LOGF_TAIL[idx]);
        return logf_result(t0 + t3);
    }
}

/* ---------------------------------------------------------------------------------------------
 * asinf (0x18004cf80, asinf_fma at 0x18004cf9c) and acosf (0x180073890, acosf_fma at
 * 0x1800738ac): AMD's scheme in single precision: a (3,1) rational in z = x^2, or in
 * z = (1 - |x|)/2 with s = sqrt(z) and the head of s split off; acosf finishes some cases in
 * double. Out of the domain: _handle_errorf's default, 0xffc00000.
 * ------------------------------------------------------------------------------------------- */

#define AS_P0  0x3b81ce6bu
#define AS_P1  0x3d678bddu
#define AS_P2  0x3e3c94dcu
#define AS_P3  (0xbc5b3fe1u + NC(24))
#define AS_Q0  0x3f561f0du
#define AS_Q1  0x3f8d6fa5u
#define AS_HALF   0x3f000000u
#define AS_TAILF  0x33a22168u             /* pi/2 - FL_PIO2 head, float */
#define AS_PIO4HF 0x3f490fdau             /* pi/4 head, float */
#define AC_TAILD  0x3c91a62633145c07ull   /* pi/2 - M_PIO2 */

static float asacos_poly(float z)
{
    float a = u2f(AS_P0), d = u2f(AS_Q0), num;
    a = FMAF(-z, a, u2f(AS_P3));
    a = FMAF(z, a, -u2f(AS_P1));
    a = FMAF(z, a, u2f(AS_P2));
    num = a * z;
    d = FMAF(-z, d, u2f(AS_Q1));
    return num / d;
}

float idwr_asinf(float x)
{
    uint32_t ux = f2u(x), ax = ux & 0x7fffffffu, ex = (ux >> 23) & 0xff;
    float ab, z, u, res;
    if (ax > 0x7f800000u) return u2f(ux | 0x400000u);
    if (ex < 0x71) return x;
    if (ex >= 0x7f) {
        if (ux == 0x3f800000u) return u2f(FL_PIO2);
        if (ux == 0xbf800000u) return u2f(FL_MPIO2);
        return u2f(0xffc00000u);
    }
    ab = u2f(ax);
    if (ex < 0x7e) {
        z = ab * ab;
        u = asacos_poly(z);
        res = FMAF(u, ab, ab);
    } else {
        float s, s1, c, t4;
        z = (1.0f - ab) * u2f(AS_HALF);
        s = sqrtf_exact(z);
        u = asacos_poly(z);
        s1 = u2f(f2u(s) & 0xffff0000u);
        c = FMAF(-s1, s1, z) / (s1 + s);
        t4 = u2f(AS_TAILF) - (c + c);                             /* pi/2 tail - 2c */
        t4 = FMAF(u, s + s, -t4);
        res = u2f(AS_PIO4HF) - (t4 - (u2f(AS_PIO4HF) - (s1 + s1)));
    }
    if (NC(25)) res = u2f(f2u(res) + 1);
    return (ux >> 31) ? u2f(f2u(res) ^ 0x80000000u) : res;
}

float idwr_acosf(float x)
{
    uint32_t ux = f2u(x), ax = ux & 0x7fffffffu, ex = (ux >> 23) & 0xff;
    float ab, z, u, res;
    if (ax > 0x7f800000u) return u2f(ux | 0x400000u);
    if (ex < 0x65) return u2f(FL_PIO2);
    if (ex >= 0x7f) {
        if (ux == 0x3f800000u) return 0.0f;
        if (ux == 0xbf800000u) return u2f(FL_PI);
        return u2f(0xffc00000u);
    }
    ab = u2f(ax);
    if (ex < 0x7e) {
        double t;
        z = ab * ab;
        u = asacos_poly(z);
        t = u2d(AC_TAILD) - (double)(u * x);
        res = (float)(u2d(M_PIO2) - ((double)x - t));
    } else {
        float s;
        z = (1.0f - ab) * u2f(AS_HALF);
        s = sqrtf_exact(z);
        u = asacos_poly(z);
        if (!(ux >> 31)) {
            float s1 = u2f(f2u(s) & 0xffff0000u), c;
            c = FMAF(-s1, s1, z) / (s1 + s);
            res = FMAF(u, s + s, c + c) + (s1 + s1);
        } else {
            double t = ((double)(s * u) - u2d(AC_TAILD)) + (double)s;
            res = (float)(u2d(M_PI_) - (t + t));
        }
    }
    if (NC(26)) res = u2f(f2u(res) + 1);
    return res;
}

/* ---------------------------------------------------------------------------------------------
 * atanf (0x180078d20, atanf_fma at 0x180078d3c): in double: |x| reduced to one of five
 * intervals as c + atan v, and a (2,2) rational in v^2.
 * ------------------------------------------------------------------------------------------- */

#define ATF_T0    0x3fdc000000000000ull   /* the interval limits 0.4375, 0.6875, 1.1875, 2.4375 */
#define ATF_T1    0x3fe6000000000000ull
#define ATF_T2    0x3ff3000000000000ull
#define ATF_T3    0x4003800000000000ull
#define ATF_HUGE  0x43d3200000000000ull
#define ATF_A05   0x3fddac670561bb4full   /* atan 1/2 */
#define ATF_A15   0x3fef730bd281f69bull   /* atan 3/2 */
#define ATF_1P5   0x3ff8000000000000ull
#define ATF_N0    (0x3f73476a758da22aull + NC(27))
#define ATF_N1    0x3fc89e173a81ee7full
#define ATF_N2    0x3fd2fa5316907834ull
#define ATF_D0    0x3fd327e3df2cf2aaull
#define ATF_D1    0x3ff1c58793da6ea4ull
#define ATF_D2    0x3fec777ca21053f0ull

float idwr_atanf(float x)
{
    uint32_t ux = f2u(x);
    double v = (double)x, c = 0.0, z, n, d, q;
    uint64_t ab = d2u(v) & ~SIGN64;
    float res;
    if (ux >> 31) v = -v;
    if (ab >= ATF_T0) {
        if (ab < ATF_T1) {
            c = u2d(ATF_A05);                          /* atan 1/2 */
            v = (v + v - 1.0) / (v + 2.0);
        } else if (ab < ATF_T2) {
            c = u2d(M_PIO4);                          /* atan 1 */
            v = (v - 1.0) / (v + 1.0);
        } else if (ab < ATF_T3) {
            c = u2d(ATF_A15);                          /* atan 3/2 */
            v = (v - u2d(ATF_1P5)) / FMA(v, u2d(ATF_1P5), 1.0);
        } else {
            if (ab > 0x7ff0000000000000ull) return u2f(ux | 0x400000u);
            if (v > u2d(ATF_HUGE)) return u2f((ux & 0x80000000u) | FL_PIO2);
            c = u2d(M_PIO2);                          /* pi/2 */
            v = -1.0 / v;
        }
    }
    z = v * v;
    n = FMA(z, u2d(ATF_N0), u2d(ATF_N1));
    n = FMA(z, n, u2d(ATF_N2));
    n = n * (v * z);
    d = FMA(z, u2d(ATF_D0), u2d(ATF_D1));
    d = FMA(z, d, u2d(ATF_D2));
    q = c - (n / d - v);
    if (ux >> 31) q = -q;
    res = (float)q;
    if (NC(28)) res = u2f(f2u(res) + 1);
    return res;
}

/* ---------------------------------------------------------------------------------------------
 * atan2f (0x180051610, atan2f_fma at 0x180051630): in double. r = min(|x|, |y|) / max: above
 * 1/16, atan(k/256) from the table plus a cubic in v = (256 b - k a) / (256 a + k b); below,
 * a series (r itself below 1e-4); then pi/2 - and pi - by octant, and the sign of y.
 * ------------------------------------------------------------------------------------------- */

#define AT2_R0    0x3fb0000000000000ull   /* 1/16: table or series */
#define AT2_256   0x4070000000000000ull
#define AT2_HALF  0x3fe0000000000000ull
#define AT2_CUB   0x3fd5555555550877ull   /* the cubic after the table */
#define AT2_TINY  0x3f1a36e2eb1c432dull   /* 1e-4: r itself */
#define AT2_S0    0x3fc2492482bd6be1ull   /* series: ~1/7, 1/5, 1/3 */
#define AT2_S1    0x3fc99999999643a3ull
#define AT2_S2    0x3fd5555555555538ull
#define AT2_2P100 0x4630000000000000ull

float idwr_atan2f(float y, float x)
{
    double xd = (double)x, yd = (double)y, a, b, r, res;
    uint64_t ux = d2u(xd), uy = d2u(yd), ay = uy & ~SIGN64, ax = ux & ~SIGN64, sy = uy & SIGN64;
    int diff = (int)((uy >> 52) & 0x7ff) - (int)((ux >> 52) & 0x7ff), swapped;
    if (ax > 0x7ff0000000000000ull) return u2f(f2u(x) | 0x400000u);
    if (ay > 0x7ff0000000000000ull) return u2f(f2u(y) | 0x400000u);
    if (ay == 0) {
        if (ux >> 63) return u2f(sy ? FL_MPI : FL_PI);
        return (float)yd;
    }
    if (ax == 0 && sy) return u2f(FL_MPIO2);
    if (diff > 26) return u2f(sy ? FL_MPIO2 : FL_PIO2);       /* x == 0 lands here too */
    if (diff < -13 && !(ux >> 63)) {
        if (diff < -150) return u2f(sy ? 0x80000000u : 0);
        if (diff < -126) {   /* 0x1800518c3: y * 2^100 / x, scaled back by 0x180051d68 */
            uint64_t q = d2u(yd * u2d(AT2_2P100) / xd), s = q & SIGN64, m = q & ~SIGN64;
            int e = (int)(m >> 52);
            if (e > 100) {
                q = s | ((uint64_t)(e - 100) << 52) | (m & 0x000fffffffffffffull);
            } else {
                m = (m & 0x001fffffffffffffull) | 0x0010000000000000ull;
                if (101 - e > 54) q = s;
                else { m >>= (100 - e); q = s | ((m >> 1) + (m & 1)); }
            }
            return (float)u2d(q);
        }
        return (float)(yd / xd);
    }
    if (diff < -26) return u2f(sy ? FL_MPI : FL_PI);      /* x < 0 here */
    if (ay == 0x7ff0000000000000ull && ax == 0x7ff0000000000000ull) {
        if (!(ux >> 63)) return u2f(sy ? FL_MPIO4 : FL_PIO4);
        return u2f(sy ? FL_M3PIO4 : FL_3PIO4);
    }
    a = u2d(ax);
    b = u2d(ay);
    swapped = b > a;
    if (swapped) { double t = a; a = b; b = t; }                      /* a = max, b = min */
    r = b / a;
    if (r > u2d(AT2_R0)) {
        double kd, num, den, v, v2;
        int32_t k = cvttpd2dq(FMA(r, u2d(AT2_256), u2d(AT2_HALF)));
        kd = (double)k;
        den = a * u2d(AT2_256);
        num = FMA(b, u2d(AT2_256), -(kd * a));
        den = FMA(b, kd, den);
        v = num / den;
        v2 = v * v;
        res = v + u2d(IDWR_M_ATAN_K256[k - 16] + NC(29));
        res = FMA(-(v2 * v), u2d(AT2_CUB), res);
    } else if (r < u2d(AT2_TINY)) {
        res = r;
    } else {
        double r2 = r * r, p = u2d(AT2_S0);
        p = FMA(-r2, p, u2d(AT2_S1));
        p = FMA(-r2, p, u2d(AT2_S2));
        res = FMA(-(r2 * r), p, r);
    }
    if (swapped) res = u2d(M_PIO2) - res;
    if (ux >> 63) res = u2d(M_PI_) - res;
    if (sy) res = u2d(d2u(res) ^ SIGN64);
    if (NC(30)) res = u2d(d2u(res) + 0x20000000ull);   /* one float ulp, about */
    return (float)res;
}
