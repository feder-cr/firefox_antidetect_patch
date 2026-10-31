/*
 * TrueType instruction handlers, ported from DWrite.dll 10.0.26100.9711 (itrp_*).
 * Each handler has the original's signature: handler(gs, p, op), p = byte after the opcode,
 * returns the next instruction pointer (the program end on error, with gs error set).
 * Checked instruction by instruction against the original (reimpl/oracle.py --lockstep).
 */
#include <string.h>
#include "tt_state.h"
#include "orig.h"

/* SafeIntExceptionHandler<SafeIntRasterizerException>::SafeIntOnOverflow 0x1801090b0: the
 * original's checked arithmetic (stack pointer differences, loop counts, indices) raises
 * STATUS_INTEGER_OVERFLOW there. TrueTypeRasterizer::Implementation's entry points catch it
 * (FilterSafeIntExceptions 0x1800ef6d4) and rethrow it as FileFormatException
 * (TranslateRasterizerException 0x18014ccb4): the glyph comes out empty, it is not retried
 * without hinting. The port calls the same function: DWrite's in oracle mode, native.c's
 * idwr_raise in standalone mode. The lockstep harness (reimpl/oracle/lockstep.c) sets
 * tt_trap_record while it runs a handler, and gets the trap as tt_trapped instead. */
#define RVA_SafeIntOnOverflow 0x1090b0
#ifndef IDWR_STANDALONE
int tt_trap_record, tt_trapped;
#endif
static void trap_overflow(void) {
#ifndef IDWR_STANDALONE
    if (tt_trap_record) {
        tt_trapped = 1;
        return;
    }
#endif
    ORIG(void (*)(void), RVA_SafeIntOnOverflow)();
}
static const uint8_t *trap(uint8_t *gs) {
    trap_overflow();
    return GS_ENDPTR(gs);
}
/* SafeIntOnDivZero 0x18014cd24 (STATUS_INTEGER_DIVIDE_BY_ZERO), caught the same way */
#define RVA_SafeIntOnDivZero 0x14cd24
static void trap_divzero(void) {
#ifndef IDWR_STANDALONE
    if (tt_trap_record) {
        tt_trapped = 2;
        return;
    }
#endif
    ORIG(void (*)(void), RVA_SafeIntOnDivZero)();
}

static const uint8_t *fail(uint8_t *gs, int32_t err) {
    GS_ERROR(gs) = err;
    return GS_ENDPTR(gs);
}

/* 32-bit wrapping arithmetic, as the original's (ADD/SUB on 32-bit registers): a signed overflow
 * in C is undefined, and the compiler may then keep the exact value in a wider register (a
 * difference that is widened to 64 bits next, for instance) */
static inline int32_t wsub(int32_t a, int32_t b) { return (int32_t)((uint32_t)a - (uint32_t)b); }
static inline int32_t wadd(int32_t a, int32_t b) { return (int32_t)((uint32_t)a + (uint32_t)b); }

static inline ptrdiff_t depth(uint8_t *gs) {
    return GS_SP(gs) - G_STACKBASE(GS_G(gs));
}

static inline int32_t *stack_limit(uint8_t *gs) {
    uint8_t *G = GS_G(gs);
    return G_STACKBASE(G) + G_STACKSIZE(G);
}

#define NEED(n)                                                        \
    do {                                                               \
        ptrdiff_t d_ = depth(gs);                                      \
        if (d_ < 0) return trap(gs);                                   \
        if (d_ < (n)) return fail(gs, ERR_STACK_UNDER);                \
    } while (0)

static inline int32_t pop(uint8_t *gs) { return *--GS_SP(gs); }

/* ---- reference points ---- */
static const uint8_t *itrp_SRP0(uint8_t *gs, const uint8_t *p, int op) { (void)op; NEED(1); GS_RP0(gs) = pop(gs); return p; }
static const uint8_t *itrp_SRP1(uint8_t *gs, const uint8_t *p, int op) { (void)op; NEED(1); GS_RP1(gs) = pop(gs); return p; }
static const uint8_t *itrp_SRP2(uint8_t *gs, const uint8_t *p, int op) { (void)op; NEED(1); GS_RP2(gs) = pop(gs); return p; }

/* ---- jumps ---- */
/* JMPR 0x1800c9600, JROT 0x1800d3c10, JROF 0x1800d0280: the offset minus one in 32 bits
 * (DEC EAX, CDQE), the jump budget, then a target before the program start is an error and one
 * past its end is clamped to the end (no error). The original adds the offset to the pointer and
 * compares addresses, which wraps only for a program within 2 GB of address 0 (never in DWrite's
 * process); here the target is placed relative to the program start, so the result does not
 * depend on where the program sits in memory (a low heap, as a non-PIE Linux process has, wraps) */
static const uint8_t *jump(uint8_t *gs, const uint8_t *p, int32_t offset) {
    int64_t at = (int64_t)(p - GS_STARTPTR(gs)) + (int32_t)((uint32_t)offset - 1u);
    if (--GS_JUMPS(gs) == 0) return fail(gs, ERR_JUMP_BUDGET);
    if (at < 0) return fail(gs, ERR_JUMP_BEFORE);
    return at <= (int64_t)(GS_ENDPTR(gs) - GS_STARTPTR(gs)) ? GS_STARTPTR(gs) + at : GS_ENDPTR(gs);
}

static const uint8_t *itrp_JMPR(uint8_t *gs, const uint8_t *p, int op) {
    (void)op;
    NEED(1);
    return jump(gs, p, pop(gs));
}

static const uint8_t *itrp_JROT(uint8_t *gs, const uint8_t *p, int op) {
    (void)op;
    NEED(2);
    int32_t cond = pop(gs), offset = pop(gs);
    return cond ? jump(gs, p, offset) : p;
}

static const uint8_t *itrp_JROF(uint8_t *gs, const uint8_t *p, int op) {
    (void)op;
    NEED(2);
    int32_t cond = pop(gs), offset = pop(gs);
    return cond ? p : jump(gs, p, offset);
}

/* ---- stack ---- */
static const uint8_t *itrp_DUP(uint8_t *gs, const uint8_t *p, int op) {
    (void)op;
    NEED(1);
    int32_t *sp = GS_SP(gs);
    if (sp + 1 > stack_limit(gs)) return fail(gs, ERR_STACK_OVER);
    *sp = sp[-1];
    GS_SP(gs) = sp + 1;
    return p;
}

static const uint8_t *itrp_POP(uint8_t *gs, const uint8_t *p, int op) { (void)op; NEED(1); GS_SP(gs)--; return p; }

static const uint8_t *itrp_CLEAR(uint8_t *gs, const uint8_t *p, int op) {
    (void)op;
    GS_SP(gs) = G_STACKBASE(GS_G(gs));
    return p;
}

static const uint8_t *itrp_SWAP(uint8_t *gs, const uint8_t *p, int op) {
    (void)op;
    NEED(2);
    int32_t *sp = GS_SP(gs), t = sp[-1];
    sp[-1] = sp[-2];
    sp[-2] = t;
    return p;
}

static const uint8_t *itrp_DEPTH(uint8_t *gs, const uint8_t *p, int op) {
    (void)op;
    ptrdiff_t d = depth(gs);
    if (GS_SP(gs) + 1 > stack_limit(gs)) return fail(gs, ERR_STACK_OVER);
    *GS_SP(gs)++ = (int32_t)d;
    return p;
}

static const uint8_t *itrp_CINDEX(uint8_t *gs, const uint8_t *p, int op) {
    (void)op;
    NEED(1);
    int32_t *top = GS_SP(gs) - 1, k = *top;
    int32_t *base = G_STACKBASE(GS_G(gs));
    if (k < 0) return trap(gs);
    if ((ptrdiff_t)(top - base) < k) return fail(gs, ERR_STACK_UNDER);
    int32_t *src = top - k;
    if (src < base || src >= stack_limit(gs)) return fail(gs, ERR_INDEX);
    *top = *src;
    return p;
}

static const uint8_t *itrp_MINDEX(uint8_t *gs, const uint8_t *p, int op) {
    (void)op;
    NEED(1);
    int32_t *top = GS_SP(gs) - 1, k = *top;
    int32_t *base = G_STACKBASE(GS_G(gs));
    if (k < 0) return trap(gs);   /* MINDEX 0x1800abac0: SafeInt on a negative index */
    if ((ptrdiff_t)(top - base) < k) return fail(gs, ERR_STACK_UNDER);
    int32_t *src = top - k;
    if (src < base || src >= stack_limit(gs)) return fail(gs, ERR_INDEX);
    int32_t v = *src;
    int32_t *dst = top;
    if (k > 0) {
        for (int32_t i = 0; i < k; i++) src[i] = src[i + 1];
        dst = top - 1;
    }
    *dst = v;
    GS_SP(gs) = dst + 1;
    return p;
}

/* ---- pushes ---- */
static const uint8_t *push_bytes(uint8_t *gs, const uint8_t *p, int n, int words) {
    int32_t *sp = GS_SP(gs);
    if (sp + n > stack_limit(gs)) return fail(gs, ERR_STACK_OVER);
    if (p < GS_STARTPTR(gs) || p + (ptrdiff_t)n * (words ? 2 : 1) > GS_ENDPTR(gs)) return fail(gs, ERR_INSPTR);
    for (int i = 0; i < n; i++) {
        if (words) { *sp++ = (int16_t)((p[0] << 8) | p[1]); p += 2; }
        else *sp++ = *p++;
    }
    GS_SP(gs) = sp;
    return p;
}

static const uint8_t *itrp_PUSHB1(uint8_t *gs, const uint8_t *p, int op) { (void)op; return push_bytes(gs, p, 1, 0); }
static const uint8_t *itrp_PUSHB(uint8_t *gs, const uint8_t *p, int op) { return push_bytes(gs, p, op - 0xaf, 0); }
static const uint8_t *itrp_PUSHW1(uint8_t *gs, const uint8_t *p, int op) { (void)op; return push_bytes(gs, p, 1, 1); }
static const uint8_t *itrp_PUSHW(uint8_t *gs, const uint8_t *p, int op) { return push_bytes(gs, p, op - 0xb7, 1); }

static const uint8_t *npush(uint8_t *gs, const uint8_t *p, int words) {
    if (p < GS_STARTPTR(gs) || p + 1 > GS_ENDPTR(gs)) return fail(gs, ERR_INSPTR);
    int n = *p;
    if (GS_SP(gs) + n > stack_limit(gs)) return fail(gs, ERR_STACK_OVER);
    return push_bytes(gs, p + 1, n, words);
}

static const uint8_t *itrp_NPUSHB(uint8_t *gs, const uint8_t *p, int op) { (void)op; return npush(gs, p, 0); }
static const uint8_t *itrp_NPUSHW(uint8_t *gs, const uint8_t *p, int op) { (void)op; return npush(gs, p, 1); }

/* ---- storage ---- */
static const uint8_t *itrp_WS(uint8_t *gs, const uint8_t *p, int op) {
    (void)op;
    NEED(2);
    uint8_t *G = GS_G(gs);
    int32_t v = pop(gs), i = pop(gs);
    if (i < (int32_t)MAXP_STORAGE(G_MAXP(G)) && i >= 0) {
        G_STORAGE(G)[i] = v;
        return p;
    }
    return fail(gs, ERR_STORAGE);
}

static const uint8_t *itrp_RS(uint8_t *gs, const uint8_t *p, int op) {
    (void)op;
    NEED(1);
    uint8_t *G = GS_G(gs);
    int32_t *top = GS_SP(gs) - 1, i = *top;
    if (i < (int32_t)MAXP_STORAGE(G_MAXP(G)) && i >= 0) {
        if (i == 8 && (G_COMPAT1C0(G) & 1) && !(AT(G, 0x88, uint8_t) & 4) && (G_COMPAT1C2(G) & 0x400)) {
            G_COMPAT1C2(G) |= 8;
            *top = 0;
            return p;
        }
        *top = G_STORAGE(G)[i];
        return p;
    }
    GS_SP(gs) = top;
    return fail(gs, ERR_STORAGE);
}

/* ---- auto flip ---- */
static const uint8_t *itrp_FLIPON(uint8_t *gs, const uint8_t *p, int op) { (void)op; G_AUTOFLIP(GS_G(gs)) = 1; return p; }
static const uint8_t *itrp_FLIPOFF(uint8_t *gs, const uint8_t *p, int op) { (void)op; G_AUTOFLIP(GS_G(gs)) = 0; return p; }

/* ---- comparisons and logic ---- */
#define BINOP(name, expr)                                                       \
    static const uint8_t *name(uint8_t *gs, const uint8_t *p, int op) {        \
        (void)op;                                                               \
        NEED(2);                                                                \
        int32_t b = pop(gs), *t = GS_SP(gs) - 1, a = *t;                        \
        *t = (expr);                                                            \
        return p;                                                               \
    }
BINOP(itrp_LT, a < b)
BINOP(itrp_LTEQ, a <= b)
BINOP(itrp_GT, a > b)
BINOP(itrp_GTEQ, a >= b)
BINOP(itrp_EQ, a == b)
BINOP(itrp_NEQ, a != b)
BINOP(itrp_AND, (a != 0 && b != 0))
BINOP(itrp_OR, (a != 0 || b != 0))
BINOP(itrp_ADD, (int32_t)((uint32_t)a + (uint32_t)b))
BINOP(itrp_SUB, (int32_t)((uint32_t)a - (uint32_t)b))
BINOP(itrp_MAX, (a < b ? b : a))
BINOP(itrp_MIN, (b < a ? b : a))

#define UNOP(name, expr)                                                        \
    static const uint8_t *name(uint8_t *gs, const uint8_t *p, int op) {        \
        (void)op;                                                               \
        NEED(1);                                                                \
        int32_t *t = GS_SP(gs) - 1, a = *t;                                     \
        *t = (expr);                                                            \
        return p;                                                               \
    }

/* itrp_RoundToGrid with no engine compensation, as ODD inlines it */
static int32_t round_to_grid(int32_t v) {
    uint32_t u = (uint32_t)v, r;
    if (v < 0) r = (uint32_t)-(int32_t)((0x20u - u) & 0xffffffc0u);
    else r = (u + 0x20u) & 0xffffffc0u;
    if ((int32_t)(r ^ u) < 0 && u != 0) r = 0;
    return (int32_t)r;
}

UNOP(itrp_ODD, (round_to_grid(a) >> 6) & 1)
UNOP(itrp_EVEN, (int32_t)(((uint32_t)(round_to_grid(a) >> 6) - 1u) & 1u))
UNOP(itrp_NOT, a == 0)
UNOP(itrp_ABS, (a < 0 ? (int32_t)(0u - (uint32_t)a) : a))
UNOP(itrp_NEG, (int32_t)(0u - (uint32_t)a))
UNOP(itrp_FLOOR, (int32_t)((uint32_t)a & 0xffffffc0u))
UNOP(itrp_CEILING, (int32_t)(((uint32_t)a + 0x3fu) & 0xffffffc0u))

static const uint8_t *itrp_DIV(uint8_t *gs, const uint8_t *p, int op) {
    (void)op;
    NEED(2);
    int32_t b = pop(gs), *t = GS_SP(gs) - 1;
    if (b == 0) return fail(gs, ERR_DIVIDE_BY_ZERO);
    *t = (int32_t)(((int64_t)*t * 64) / b);
    return p;
}

static const uint8_t *itrp_MUL(uint8_t *gs, const uint8_t *p, int op) {
    (void)op;
    NEED(2);
    int32_t *t = GS_SP(gs) - 2;
    GS_SP(gs) = t + 1;
    uint32_t a = (uint32_t)t[0], b = (uint32_t)t[1], r;
    if ((int32_t)a < 0xb505 && (int32_t)b < 0xb505 && -0xb505 < (int32_t)a && -0xb505 < (int32_t)b) {
        r = (uint32_t)((int32_t)(a * b + 0x20) >> 6);
    } else {
        uint32_t ua = (int32_t)a < 0 ? 0u - a : a;
        uint32_t neg = a >> 31;
        if ((int32_t)b < 0) { b = 0u - b; neg ^= 1; }
        uint32_t mid = (b >> 16) * (ua & 0xffff) + (ua >> 16) * (b & 0xffff);
        uint32_t mid_lo = mid * 0x10000u + 0x20u;
        uint32_t lo = (ua & 0xffff) * (b & 0xffff) + mid_lo;
        r = ((b >> 16) * (ua >> 16) + (mid >> 16) + (uint32_t)(lo < mid_lo)) * 0x4000000u | lo >> 6;
        if (neg) r = 0u - r;
    }
    t[0] = (int32_t)r;
    return p;
}

/* ---- delta base / shift ---- */
static const uint8_t *itrp_SDB(uint8_t *gs, const uint8_t *p, int op) { (void)op; NEED(1); G_DELTABASE(GS_G(gs)) = (int16_t)pop(gs); return p; }
static const uint8_t *itrp_SDS(uint8_t *gs, const uint8_t *p, int op) { (void)op; NEED(1); G_DELTASHIFT(GS_G(gs)) = (int16_t)pop(gs); return p; }

static const uint8_t *itrp_EIF(uint8_t *gs, const uint8_t *p, int op) { (void)gs; (void)op; return p; }

/* ======================================================================================
 * Fixed-point helpers
 * ====================================================================================== */

static int32_t sat32(int64_t v) {  /* CHKOVF */
    if (v >= 0x80000000LL) return 0x7fffffff;
    if (v < -0x80000000LL) return INT32_MIN;
    return (int32_t)v;
}

int32_t tt_FixMul(int32_t a, int32_t b) {  /* DWRITE_FixMul */
    int64_t p = (int64_t)a * b;
    int64_t r = (p + (p >> 63) + 0x8000) >> 16;
    return sat32(r);
}

int32_t tt_FixDiv(int32_t a, int32_t b) {  /* DWRITE_FixDiv */
    int64_t num = (int64_t)a * 0x10000;
    int64_t half = -(int64_t)(b / 2);
    if ((b < 0) == (num < 0)) half = b / 2;
    if (b == 0) return 0x7fffffff;
    return sat32((num + half) / b);
}

static int32_t CompDiv(int32_t d, int64_t n);
TT_EXPORT int32_t tt_CompDiv(int32_t d, int64_t n) { return CompDiv(d, n); }  /* for the function swap */

static int32_t CompDiv(int32_t d, int64_t n) {
    int64_t half = -(int64_t)(d / 2);
    if ((d < 0) == (n < 0)) half = d / 2;
    if (d != 0) return (int32_t)((n + half) / d);
    return (int32_t)((uint32_t)(n + half < 0) + 0x7fffffffu);
}

/* ShortFracDiv 0x18014cd48: (a << 14) / b in 32 bits, then SafeCastHelper<short, int>::CastThrow
 * (0x180002fb0): a quotient outside the int16 range is a SafeInt trap, b = 0 SafeIntOnDivZero.
 * Returns 0 with *trapped set when it traps (the original does not return). */
static int16_t ShortFracDiv(int16_t a, int16_t b, int *trapped) {
    int32_t n = (int32_t)((uint32_t)(int32_t)a << 14);
    if (n == 0) return 0;
    if (b == 0) { trap_divzero(); *trapped = 1; return 0; }
    int32_t q = n / b;   /* n is a multiple of 2^14 in [-2^29, 2^29): no INT_MIN / -1 */
    if ((uint32_t)q + 0x8000u > 0xffffu) { trap_overflow(); *trapped = 1; return 0; }
    return (int16_t)q;
}

int32_t tt_FracSqrt(int32_t x) {  /* DWRITE_FracSqrt */
    if (x < 0) return INT32_MIN;
    uint32_t bit = 0x10000000, root = (uint32_t)x < 0x40000000u ? 0 : 0x40000000u;
    uint32_t rem = (uint32_t)x < 0x40000000u ? (uint32_t)x : (uint32_t)x + 0xc0000000u, r2;
    do {
        r2 = rem;
        if (bit + root <= r2) { r2 -= bit + root; root += bit * 2; }
        rem = r2 * 2;
        bit >>= 1;
    } while (bit);
    if (root < rem) { uint32_t d = rem - root; root += 1; r2 = d * 2 - 1; }
    else r2 <<= 2;
    return (int32_t)(root + (root < r2));
}

/* round to nearest for 2.14 x 26.6 products: ((a * b >> 13) + 1) >> 1 */
static inline int32_t mul14(int64_t a, int64_t b) { return (int32_t)(((a * b >> 13) + 1) >> 1); }

/* ======================================================================================
 * Projections, point movement, CVT access (installed in the gs function slots)
 * ====================================================================================== */

int32_t itrp_XProject(uint8_t *gs, int32_t x, int32_t y) { (void)gs; (void)y; return x; }
int32_t itrp_YProject(uint8_t *gs, int32_t x, int32_t y) { (void)gs; (void)x; return y; }
int32_t itrp_Project(uint8_t *gs, int32_t x, int32_t y) { return mul14(x, GS_PROJ_X(gs)) + mul14(GS_PROJ_Y(gs), y); }
int32_t itrp_OldProject(uint8_t *gs, int32_t x, int32_t y) { return mul14(x, GS_OPROJ_X(gs)) + mul14(GS_OPROJ_Y(gs), y); }

void itrp_XMovePoint(uint8_t *gs, uint8_t *e, int32_t pt, int32_t d) {
    (void)gs;
    E_X(e)[pt] += d;
    E_F(e)[pt] |= 1;
}

void itrp_YMovePoint(uint8_t *gs, uint8_t *e, int32_t pt, int32_t d) {
    (void)gs;
    E_Y(e)[pt] += d;
    E_F(e)[pt] |= 2;
}

void itrp_MovePoint(uint8_t *gs, uint8_t *e, int32_t pt, int32_t d) {
    int16_t pf = GS_PFPROJ(gs), fy = GS_FREE_Y(gs), fx = GS_FREE_X(gs);
    if (pf == 0x4000) {
        if (fx != 0) { E_X(e)[pt] += (int32_t)((((int64_t)fx * d >> 13) + 1) >> 1); E_F(e)[pt] |= 1; }
        if (fy == 0) return;
        E_Y(e)[pt] += (int32_t)((((int64_t)fy * d >> 13) + 1) >> 1);
        E_F(e)[pt] |= 2;
        return;
    }
    if (fx != 0) {
        if (pf == fx) {
            E_X(e)[pt] += d;
        } else {
            int64_t n = (int64_t)fx * d;
            int32_t q;
            if (pf < 0) q = (int32_t)((n - pf / 2) / pf);
            else if (pf != 0) q = (int32_t)((n + pf / 2) / pf);
            else q = (int32_t)((uint32_t)(n + pf / 2 < 0) + 0x7fffffffu);
            E_X(e)[pt] += q;
        }
        E_F(e)[pt] |= 1;
    }
    if (fy != 0) {
        int32_t dy = d;
        if (pf != fy) dy = CompDiv(pf, (int64_t)fy * d);
        E_Y(e)[pt] += dy;
        E_F(e)[pt] |= 2;
    }
}

/* itrp_GetCVTScale: scale of the projection direction relative to the y scale (16.16) */
static int32_t GetCVTScale(uint8_t *gs) {
    uint8_t *G = GS_G(gs);
    if (GS_PROJ_Y(gs) == 0) return AT(G, 0x160, int32_t);
    if (GS_PROJ_X(gs) == 0) return AT(G, 0x164, int32_t);
    int32_t s = AT(gs, 0xa0, int32_t);
    if (s) return s;
    int32_t py = GS_PROJ_Y(gs), px = GS_PROJ_X(gs);
    s = AT(G, 0x164, int32_t);
    int32_t a = tt_FixMul(AT(G, 0x160, int32_t), AT(G, 0x160, int32_t));
    a = tt_FixMul((int32_t)(int16_t)((px * px + 0x2000) >> 14) << 2, a);
    int32_t b = tt_FixMul(s, s);
    b = tt_FixMul((int32_t)(int16_t)((py * py + 0x2000) >> 14) << 2, b);
    s = 0x10000;
    if (a + b < 0x10001) {
        s = (tt_FracSqrt((a + b) * 0x4000) + 0x2000) >> 14;
        AT(gs, 0xa0, int32_t) = s;
    }
    return s;
}

int32_t itrp_GetCVTEntryFast(uint8_t *gs, int32_t i) { return G_CVT(GS_G(gs))[i]; }
int32_t itrp_GetCVTEntrySlow(uint8_t *gs, int32_t i) {
    int64_t v = (int64_t)G_CVT(GS_G(gs))[i] * GetCVTScale(gs);
    return sat32((v + 0x8000 + (v >> 63)) >> 16);
}
int32_t itrp_GetSingleWidthFast(uint8_t *gs) { return AT(GS_G(gs), 0x80, int32_t); }
int32_t itrp_GetSingleWidthSlow(uint8_t *gs) { return tt_FixMul(AT(GS_G(gs), 0x80, int32_t), GetCVTScale(gs)); }
void itrp_ChangeCvtFast(uint8_t *gs, uint8_t *e, int32_t i, int32_t d) { (void)e; G_CVT(GS_G(gs))[i] += d; }
void itrp_ChangeCvtSlow(uint8_t *gs, uint8_t *e, int32_t i, int32_t d) {
    (void)e;
    int32_t s = tt_FixDiv(d, GetCVTScale(gs));
    G_CVT(GS_G(gs))[i] += s;
}

/* ======================================================================================
 * Rounding: round(gs, value, engine compensation)
 * ====================================================================================== */

typedef int32_t (*round_fn)(uint8_t *gs, int32_t v, int32_t comp);

#define SIGN_FIX(r, v) do { if ((int32_t)((uint32_t)(r) ^ (uint32_t)(v)) < 0 && (v) != 0) (r) = 0; } while (0)

int32_t itrp_RoundToGrid(uint8_t *gs, int32_t v, int32_t c) {
    (void)gs;
    uint32_t u = (uint32_t)v, r = v < 0 ? 0u - (((uint32_t)c - u + 0x20u) & 0xffffffc0u) : ((uint32_t)c + 0x20u + u) & 0xffffffc0u;
    SIGN_FIX(r, v);
    return (int32_t)r;
}
int32_t itrp_RoundToHalfGrid(uint8_t *gs, int32_t v, int32_t c) {
    (void)gs;
    uint32_t u = (uint32_t)v, r = v < 0 ? (0u - (((uint32_t)c - u) & 0xffffffc0u)) - 0x20u : ((u + (uint32_t)c) & 0xffffffc0u) + 0x20u;
    if ((int32_t)(r ^ u) < 0 && v != 0) r = v < 1 ? 0xffffffe0u : 0x20u;
    return (int32_t)r;
}
int32_t itrp_RoundToDoubleGrid(uint8_t *gs, int32_t v, int32_t c) {
    (void)gs;
    uint32_t u = (uint32_t)v, r = v < 0 ? 0u - (((uint32_t)c - u + 0x10u) & 0xffffffe0u) : ((uint32_t)c + 0x10u + u) & 0xffffffe0u;
    SIGN_FIX(r, v);
    return (int32_t)r;
}
int32_t itrp_RoundDownToGrid(uint8_t *gs, int32_t v, int32_t c) {
    (void)gs;
    uint32_t u = (uint32_t)v, r = v < 0 ? 0u - (((uint32_t)c - u) & 0xffffffc0u) : ((uint32_t)c + u) & 0xffffffc0u;
    SIGN_FIX(r, v);
    return (int32_t)r;
}
int32_t itrp_RoundUpToGrid(uint8_t *gs, int32_t v, int32_t c) {
    (void)gs;
    uint32_t u = (uint32_t)v, r = v < 0 ? 0u - (((uint32_t)c - u + 0x3fu) & 0xffffffc0u) : ((uint32_t)c + 0x3fu + u) & 0xffffffc0u;
    SIGN_FIX(r, v);
    return (int32_t)r;
}
int32_t itrp_RoundOff(uint8_t *gs, int32_t v, int32_t c) {
    (void)gs;
    if (v < 0) c = (int32_t)(0u - (uint32_t)c);
    uint32_t r = (uint32_t)v + (uint32_t)c;
    SIGN_FIX(r, v);
    return (int32_t)r;
}
int32_t itrp_SuperRound(uint8_t *gs, int32_t v, int32_t c) {
    uint8_t *G = GS_G(gs);
    uint32_t phase = (uint32_t)(int32_t)AT(G, 0xa4, int16_t), mask = AT(G, 0x9c, uint32_t);
    uint32_t t = (uint32_t)((int32_t)AT(G, 0xa6, int16_t) - (int32_t)phase), u = (uint32_t)v, r;
    if (v < 0) r = 0u - (((t - u + (uint32_t)c) & mask) + phase);
    else r = ((t + u + (uint32_t)c) & mask) + phase;
    if ((int32_t)(r ^ u) < 0 && v != 0) r = v < 1 ? 0u - phase : phase;
    return (int32_t)r;
}
/* Super45Round 0x180150f90: the sum in 16 bits, ShortFracDiv by the period (G + 0xa0), the
 * quotient sign-extended before the mask (CWDE, AND, CDQE), times the period, rounded, plus the
 * phase (G + 0xa4) */
int32_t itrp_Super45Round(uint8_t *gs, int32_t v, int32_t c) {
    uint8_t *G = GS_G(gs);
    int16_t period = AT(G, 0xa0, int16_t), phase = AT(G, 0xa4, int16_t);
    uint16_t t = (uint16_t)(AT(G, 0xa6, uint16_t) - AT(G, 0xa4, uint16_t));
    uint16_t sum = v < 0 ? (uint16_t)(t - (uint16_t)v + (uint16_t)c) : (uint16_t)(t + (uint16_t)v + (uint16_t)c);
    int trapped = 0;
    int16_t q = ShortFracDiv((int16_t)sum, period, &trapped);
    if (trapped) return 0;
    int64_t m = (int64_t)period * (int64_t)(int32_t)((uint32_t)(int32_t)q & 0xffffffc0u);
    uint32_t r = (uint32_t)(int32_t)(((m >> 13) + 1) >> 1) + (uint32_t)(int32_t)phase;
    if (v < 0) r = 0u - r;
    if ((int32_t)(r ^ (uint32_t)v) < 0 && v != 0) r = v > 0 ? (uint32_t)(int32_t)phase : 0u - (uint32_t)(int32_t)phase;
    return (int32_t)r;
}

/* Subpixel variants: grid of 1/4 pixel (16 units), compensation halved */
int32_t itrp_RoundToGridSP(uint8_t *gs, int32_t v, int32_t c) {
    (void)gs;
    uint32_t u = (uint32_t)v, h = (uint32_t)(c / 2), r = v < 0 ? 0u - ((h - u + 2u) & 0xfffffffcu) : (h + 2u + u) & 0xfffffffcu;
    SIGN_FIX(r, v);
    return (int32_t)r;
}
int32_t itrp_RoundToHalfGridSP(uint8_t *gs, int32_t v, int32_t c) {
    (void)gs;
    uint32_t u = (uint32_t)v, h = (uint32_t)(c / 2), r = v < 0 ? (0u - ((h - u) & 0xfffffffcu)) - 2u : ((u + h) & 0xfffffffcu) + 2u;
    if ((int32_t)(r ^ u) < 0 && v != 0) r = v < 1 ? 0xfffffffeu : 2u;
    return (int32_t)r;
}
int32_t itrp_RoundToDoubleGridSP(uint8_t *gs, int32_t v, int32_t c) {
    (void)gs;
    uint32_t u = (uint32_t)v, h = (uint32_t)(c / 2), r = v < 0 ? 0u - ((h - u + 1u) & 0xfffffffeu) : (h + 1u + u) & 0xfffffffeu;
    SIGN_FIX(r, v);
    return (int32_t)r;
}
int32_t itrp_RoundDownToGridSP(uint8_t *gs, int32_t v, int32_t c) {
    if (!(AT(GS_G(gs), 0x88, uint8_t) & 4) && GS_OLDPROJECT(gs) == (void *)itrp_Project)
        return itrp_RoundDownToGrid(gs, v, c);
    uint32_t u = (uint32_t)v, h = (uint32_t)(c / 2), r = v < 0 ? 0u - ((h - u) & 0xfffffffcu) : (h + u) & 0xfffffffcu;
    SIGN_FIX(r, v);
    return (int32_t)r;
}
int32_t itrp_RoundUpToGridSP(uint8_t *gs, int32_t v, int32_t c) {
    (void)gs;
    uint32_t u = (uint32_t)v, h = (uint32_t)(c / 2), r = v < 0 ? 0u - ((h - u + 3u) & 0xfffffffcu) : (h + 3u + u) & 0xfffffffcu;
    SIGN_FIX(r, v);
    return (int32_t)r;
}
int32_t itrp_RoundOffSP(uint8_t *gs, int32_t v, int32_t c) {
    (void)gs;
    int32_t h = c / 2;
    if (v < 0) h = -(c / 2);
    uint32_t r = (uint32_t)h + (uint32_t)v;
    SIGN_FIX(r, v);
    return (int32_t)r;
}

/* PTR_itrp_RoundToDoubleGrid_18019a700: indexed by round state, + 8 for the subpixel set */
static const round_fn round_table[16] = {
    itrp_RoundToDoubleGrid, itrp_RoundDownToGrid, itrp_RoundUpToGrid, itrp_RoundToGrid,
    itrp_RoundToHalfGrid, itrp_RoundOff, itrp_SuperRound, itrp_Super45Round,
    itrp_RoundToDoubleGridSP, itrp_RoundDownToGridSP, itrp_RoundUpToGridSP, itrp_RoundToGridSP,
    itrp_RoundToHalfGridSP, itrp_RoundOffSP, itrp_SuperRound, itrp_Super45Round,
};

#define G_ROUNDFN(G)    AT(G, 0x90, round_fn)
#define G_ROUNDSTATE(G) AT(G, 0x98, uint16_t)
#define GS_SPFLAG(gs)   AT(gs, 0xcc, int16_t)   /* projection not along the subpixel axis */
#define GS_A4(gs)       AT(gs, 0xa4, int16_t)

static int subpixel_rounding(uint8_t *gs) {
    uint8_t *G = GS_G(gs);
    return GS_SPFLAG(gs) != 0 && ((AT(G, 0x88, uint8_t) & 4) || AT(G, 0x16b, uint8_t) != 0);
}

static void select_round(uint8_t *gs) {
    uint8_t *G = GS_G(gs);
    G_ROUNDFN(G) = round_table[G_ROUNDSTATE(G) + (subpixel_rounding(gs) ? 8 : 0)];
}

/* ======================================================================================
 * Vectors
 * ====================================================================================== */

static void check_pf_proj(uint8_t *gs) {  /* itrp_Check_PF_Proj */
    int16_t s = GS_PFPROJ(gs);
    if ((uint16_t)(s + 0x3ffu) < 0x7ff) GS_PFPROJ(gs) = (int16_t)(((s >> 15) & 0x8000) + 0x4000);
}

static void compute_pf_proj(uint8_t *gs) {  /* itrp_ComputeAndCheck_PF_Proj */
    int16_t s = (int16_t)((int16_t)((GS_FREE_Y(gs) * GS_PROJ_Y(gs) + 0x2000) >> 14) +
                          (int16_t)((GS_FREE_X(gs) * GS_PROJ_X(gs) + 0x2000) >> 14));
    if ((uint16_t)(s + 0x3ffu) < 0x7ff) s = (int16_t)(((s >> 15) & 0x8000) + 0x4000);
    AT(gs, 0xa0, int32_t) = 0;
    GS_PFPROJ(gs) = s;
}

static int16_t axis_spflag(uint8_t *gs, int x_axis) {
    uint16_t c = G_COMPAT1C0(GS_G(gs));
    if (!(c & 1)) return 0;
    return x_axis ? !(c & 4) : (c & 4) != 0;
}

static void set_cached_ffff(uint8_t *gs) { AT(gs, 0xd0, uint16_t) = 0xffff; AT(gs, 0xce, uint16_t) = 0xffff; }

static const uint8_t *svtca(uint8_t *gs, const uint8_t *p, int x) {
    AT(gs, 0x18, uint32_t) = x ? 0x4000 : 0x40000000;
    AT(gs, 0x1c, uint32_t) = x ? 0x4000 : 0x40000000;
    GS_MOVEPOINT(gs) = x ? (void *)itrp_XMovePoint : (void *)itrp_YMovePoint;
    GS_PROJECT(gs) = x ? (void *)itrp_XProject : (void *)itrp_YProject;
    GS_SPFLAG(gs) = axis_spflag(gs, x);
    select_round(gs);
    set_cached_ffff(gs);
    GS_OLDPROJECT(gs) = GS_PROJECT(gs);
    GS_PFPROJ(gs) = 0x4000;
    if (GS_A4(gs) != 0) GS_A4(gs) = x ? 1 : 2;
    return p;
}
static const uint8_t *itrp_SVTCA_0(uint8_t *gs, const uint8_t *p, int op) { (void)op; return svtca(gs, p, 0); }
static const uint8_t *itrp_SVTCA_1(uint8_t *gs, const uint8_t *p, int op) { (void)op; return svtca(gs, p, 1); }

static const uint8_t *spvtca(uint8_t *gs, const uint8_t *p, int x) {
    GS_PROJECT(gs) = x ? (void *)itrp_XProject : (void *)itrp_YProject;
    AT(gs, 0x18, uint32_t) = x ? 0x4000 : 0x40000000;
    GS_SPFLAG(gs) = axis_spflag(gs, x);
    select_round(gs);
    int16_t f = x ? GS_FREE_X(gs) : GS_FREE_Y(gs);
    set_cached_ffff(gs);
    GS_PFPROJ(gs) = f;
    if ((uint16_t)(f + 0x3ffu) < 0x7ff) GS_PFPROJ(gs) = (int16_t)(((f >> 15) & 0x8000) + 0x4000);
    GS_MOVEPOINT(gs) = (void *)itrp_MovePoint;
    GS_OLDPROJECT(gs) = GS_PROJECT(gs);
    GS_A4(gs) = 0;
    return p;
}
static const uint8_t *itrp_SPVTCA_0(uint8_t *gs, const uint8_t *p, int op) { (void)op; return spvtca(gs, p, 0); }
static const uint8_t *itrp_SPVTCA_1(uint8_t *gs, const uint8_t *p, int op) { (void)op; return spvtca(gs, p, 1); }

static const uint8_t *sfvtca(uint8_t *gs, const uint8_t *p, int x) {
    GS_PFPROJ(gs) = x ? GS_PROJ_X(gs) : GS_PROJ_Y(gs);
    AT(gs, 0x1c, uint32_t) = x ? 0x4000 : 0x40000000;
    check_pf_proj(gs);
    GS_A4(gs) = 0;
    GS_MOVEPOINT(gs) = (void *)itrp_MovePoint;
    return p;
}
static const uint8_t *itrp_SFVTCA_0(uint8_t *gs, const uint8_t *p, int op) { (void)op; return sfvtca(gs, p, 0); }
static const uint8_t *itrp_SFVTCA_1(uint8_t *gs, const uint8_t *p, int op) { (void)op; return sfvtca(gs, p, 1); }

static const uint8_t *itrp_SFVTPV(uint8_t *gs, const uint8_t *p, int op) {
    (void)op;
    AT(gs, 0x1c, uint32_t) = AT(gs, 0x18, uint32_t);
    GS_MOVEPOINT(gs) = (void *)itrp_MovePoint;
    GS_A4(gs) = 0;
    GS_PFPROJ(gs) = 0x4000;
    return p;
}

static const uint8_t *itrp_WPV(uint8_t *gs, const uint8_t *p, int op) {
    (void)op;
    NEED(2);
    GS_PROJ_Y(gs) = (int16_t)pop(gs);
    GS_PROJ_X(gs) = (int16_t)pop(gs);
    set_cached_ffff(gs);
    compute_pf_proj(gs);
    uint8_t *G = GS_G(gs);
    GS_MOVEPOINT(gs) = (void *)itrp_MovePoint;
    GS_PROJECT(gs) = (void *)itrp_Project;
    uint16_t c = G_COMPAT1C0(G);
    int16_t flag = 0;
    if (c & 1) {
        if (!(c & 4)) flag = !(GS_PROJ_Y(gs) == 0x4000 && GS_PROJ_X(gs) == 0);
        else flag = !(GS_PROJ_X(gs) == 0x4000 && GS_PROJ_Y(gs) == 0);
    }
    GS_SPFLAG(gs) = flag;
    select_round(gs);
    GS_OLDPROJECT(gs) = GS_PROJECT(gs);
    GS_A4(gs) = 0;
    return p;
}

static const uint8_t *itrp_WFV(uint8_t *gs, const uint8_t *p, int op) {
    (void)op;
    NEED(2);
    GS_FREE_Y(gs) = (int16_t)pop(gs);
    GS_FREE_X(gs) = (int16_t)pop(gs);
    compute_pf_proj(gs);
    GS_A4(gs) = 0;
    GS_MOVEPOINT(gs) = (void *)itrp_MovePoint;
    return p;
}

static const uint8_t *push2(uint8_t *gs, const uint8_t *p, int32_t a, int32_t b) {
    if (GS_SP(gs) + 2 > stack_limit(gs)) return fail(gs, ERR_STACK_OVER);
    *GS_SP(gs)++ = a;
    *GS_SP(gs)++ = b;
    return p;
}
static const uint8_t *itrp_RPV(uint8_t *gs, const uint8_t *p, int op) { (void)op; return push2(gs, p, GS_PROJ_X(gs), GS_PROJ_Y(gs)); }
static const uint8_t *itrp_RFV(uint8_t *gs, const uint8_t *p, int op) { (void)op; return push2(gs, p, GS_FREE_X(gs), GS_FREE_Y(gs)); }

/* ---- round state ---- */
static const uint8_t *set_round(uint8_t *gs, const uint8_t *p, int state, round_fn normal, round_fn sp, int clear_a4) {
    uint8_t *G = GS_G(gs);
    G_ROUNDSTATE(G) = (uint16_t)state;
    G_ROUNDFN(G) = (GS_SPFLAG(gs) == 0 || (!(AT(G, 0x88, uint8_t) & 4) && AT(G, 0x16b, uint8_t) == 0)) ? normal : sp;
    if (clear_a4) GS_A4(gs) = 0;
    return p;
}
static const uint8_t *itrp_RTG(uint8_t *gs, const uint8_t *p, int op) { (void)op; return set_round(gs, p, 3, itrp_RoundToGrid, itrp_RoundToGridSP, 0); }
static const uint8_t *itrp_RTHG(uint8_t *gs, const uint8_t *p, int op) { (void)op; return set_round(gs, p, 4, itrp_RoundToHalfGrid, itrp_RoundToHalfGridSP, 1); }
static const uint8_t *itrp_RTDG(uint8_t *gs, const uint8_t *p, int op) { (void)op; return set_round(gs, p, 0, itrp_RoundToDoubleGrid, itrp_RoundToDoubleGridSP, 1); }
static const uint8_t *itrp_RUTG(uint8_t *gs, const uint8_t *p, int op) { (void)op; return set_round(gs, p, 2, itrp_RoundUpToGrid, itrp_RoundUpToGridSP, 1); }
static const uint8_t *itrp_RDTG(uint8_t *gs, const uint8_t *p, int op) { (void)op; return set_round(gs, p, 1, itrp_RoundDownToGrid, itrp_RoundDownToGridSP, 1); }
static const uint8_t *itrp_ROFF(uint8_t *gs, const uint8_t *p, int op) { (void)op; return set_round(gs, p, 5, itrp_RoundOff, itrp_RoundOffSP, 1); }

static const uint8_t *itrp_SROUND(uint8_t *gs, const uint8_t *p, int op) {
    (void)op;
    NEED(1);
    uint8_t *G = GS_G(gs);
    uint32_t n = (uint32_t)pop(gs);
    int32_t period;
    uint16_t threshold, q1, q2, q3;  /* threshold default, phase quarters */
    switch (n & 0xc0) {
    case 0x00: threshold = 0x1f; period = 0x20; q3 = 0x18; q1 = 8; q2 = 0x10; break;
    case 0x40: threshold = 0x3f; period = 0x40; q3 = 0x30; q1 = 0x10; q2 = 0x20; break;
    case 0x80: threshold = 0x7f; period = 0x80; q3 = 0x60; q1 = 0x20; q2 = 0x40; break;
    default:   threshold = 0x3e6; period = 999; q3 = 0x2ed; q1 = 0xfa; q2 = 500; break;
    }
    AT(G, 0xa2, int16_t) = (int16_t)period;
    AT(G, 0x9c, int32_t) = -period;
    switch (n & 0x30) {
    case 0x00: AT(G, 0xa4, uint16_t) = 0; break;
    case 0x10: AT(G, 0xa4, uint16_t) = q1; break;
    case 0x20: AT(G, 0xa4, uint16_t) = q2; break;
    case 0x30: AT(G, 0xa4, uint16_t) = q3; break;
    }
    if (n & 0xf) threshold = (uint16_t)((int32_t)(((n & 0xf) - 4) * period + 4) >> 3);
    AT(G, 0xa6, uint16_t) = threshold;
    G_ROUNDSTATE(G) = 6;
    G_ROUNDFN(G) = itrp_SuperRound;
    GS_A4(gs) = 0;
    return p;
}

static void SetRoundValues(uint8_t *gs, uint32_t n, int square) {
    uint8_t *G = GS_G(gs);
    uint32_t sel = n & 0xc0;
    int32_t period = 0x20;
    if (!square) {
        AT(G, 0xa0, uint16_t) = 0x2d47;
        if (sel == 0) { AT(G, 0xa0, uint16_t) = 0x16a3; period = 0x17; }
        else if (sel == 0x40) period = 0x2d;
        else if (sel == 0x80) { AT(G, 0xa0, uint16_t) = 0x5a8e; period = 0x5b; }
        else { AT(G, 0xa0, uint16_t) = 999; period = 4; }
        AT(G, 0xa2, int16_t) = (int16_t)period;
    } else {
        if (sel == 0) AT(G, 0xa2, int16_t) = 0x20;
        else { period = sel == 0x40 ? 0x40 : sel == 0x80 ? 0x80 : 999; AT(G, 0xa2, int16_t) = (int16_t)period; }
        AT(G, 0x9c, int32_t) = -period;
    }
    int16_t s = (int16_t)period;
    switch (n & 0x30) {
    case 0x00: AT(G, 0xa4, uint16_t) = 0; break;
    case 0x10: AT(G, 0xa4, uint16_t) = (uint16_t)(s + 2u) >> 2; break;
    case 0x20: AT(G, 0xa4, uint16_t) = (uint16_t)(s + 1u) >> 1; break;
    case 0x30: AT(G, 0xa4, uint16_t) = (uint16_t)(s * 3 + 2u) >> 2; break;
    }
    AT(G, 0xa6, int16_t) = (n & 0xf) == 0 ? (int16_t)(s - 1) : (int16_t)((int32_t)(period * ((n & 0xf) - 4) + 4) >> 3);
}

static const uint8_t *itrp_S45ROUND(uint8_t *gs, const uint8_t *p, int op) {
    (void)op;
    NEED(1);
    SetRoundValues(gs, (uint32_t)pop(gs), 0);
    uint8_t *G = GS_G(gs);
    G_ROUNDSTATE(G) = 7;
    G_ROUNDFN(G) = itrp_Super45Round;
    GS_A4(gs) = 0;
    return p;
}

/* ---- simple graphics-state setters ---- */
static const uint8_t *itrp_SLOOP(uint8_t *gs, const uint8_t *p, int op) { (void)op; NEED(1); AT(gs, 0x5c, int32_t) = pop(gs) - 1; return p; }
static const uint8_t *itrp_SMD(uint8_t *gs, const uint8_t *p, int op) { (void)op; NEED(1); AT(GS_G(gs), 0x8c, int32_t) = pop(gs); return p; }
static const uint8_t *itrp_SCVTCI(uint8_t *gs, const uint8_t *p, int op) { (void)op; NEED(1); AT(GS_G(gs), 0x78, int32_t) = pop(gs); return p; }
static const uint8_t *itrp_SSWCI(uint8_t *gs, const uint8_t *p, int op) { (void)op; NEED(1); AT(GS_G(gs), 0x7c, int32_t) = pop(gs); GS_A4(gs) = 0; return p; }
static const uint8_t *itrp_SANGW(uint8_t *gs, const uint8_t *p, int op) { (void)op; NEED(1); AT(GS_G(gs), 0xac, int16_t) = (int16_t)pop(gs); return p; }
static const uint8_t *itrp_AA(uint8_t *gs, const uint8_t *p, int op) { (void)op; NEED(1); GS_SP(gs)--; return p; }

typedef int32_t (*scale_fn)(void *ctx, int32_t v);
static const uint8_t *itrp_SSW(uint8_t *gs, const uint8_t *p, int op) {
    (void)op;
    uint8_t *G = GS_G(gs);
    GS_A4(gs) = 0;
    NEED(1);
    int16_t v = (int16_t)pop(gs);
    AT(G, 0xae, int16_t) = v;
    AT(G, 0x80, int32_t) = AT(G, 0xe8, scale_fn)(G + 0x150, v);  /* FUnits -> 26.6 */
    return p;
}

static const uint8_t *itrp_SZP(uint8_t *gs, const uint8_t *p, int op) {  /* itrp_SetElementPtr */
    NEED(1);
    uint32_t z = (uint32_t)pop(gs);
    if (z > 1) return fail(gs, 0x1116);   /* 0x1800cd470: no zone pointer written */
    uint8_t *e = GS_ELEMENTS(gs) + (int64_t)(int32_t)z * 0x70;
    if (op == 0x13) GS_CE0(gs) = e;
    else if (op == 0x14) GS_CE1(gs) = e;
    else if (op == 0x15) GS_CE2(gs) = e;
    else { GS_CE2(gs) = e; GS_CE1(gs) = e; GS_CE0(gs) = e; }
    GS_A4(gs) = 0;
    return p;
}

/* ---- CVT ---- */
#define ERR_CVT 0x111b
typedef int32_t (*getcvt_fn)(uint8_t *gs, int32_t i);

static const uint8_t *itrp_RCVT(uint8_t *gs, const uint8_t *p, int op) {
    (void)op;
    NEED(1);
    uint8_t *G = GS_G(gs);
    int32_t *t = GS_SP(gs) - 1, i = *t;
    GS_SP(gs) = t;
    if ((i < (int32_t)G_CVTCOUNT(G) || i < 0x100) && i >= 0) {
        int32_t v;
        if (GS_GETCVT(gs) == (void *)itrp_GetCVTEntryFast) v = G_CVT(G)[i];
        else if (GS_GETCVT(gs) == (void *)itrp_GetCVTEntrySlow) v = itrp_GetCVTEntrySlow(gs, i);
        else return fail(gs, 0x110a);
        *GS_SP(gs)++ = v;
        return p;
    }
    return fail(gs, ERR_CVT);
}

static const uint8_t *itrp_WCVTP(uint8_t *gs, const uint8_t *p, int op) {  /* itrp_WCVT */
    (void)op;
    NEED(2);
    uint8_t *G = GS_G(gs);
    int32_t v = pop(gs), i = pop(gs);
    if (i < (int32_t)G_CVTCOUNT(G) && i >= 0) {
        int64_t w = v;
        if (v != 0 && GS_GETCVT(gs) != (void *)itrp_GetCVTEntryFast) {
            int32_t s = GetCVTScale(gs);
            int64_t half = -(int64_t)(s / 2);
            if ((s < 0) == (v < 0)) half = s / 2;
            w = 0x7fffffff;
            if (s != 0) {
                int64_t q = ((int64_t)v * 0x10000 + half) / s;
                w = q >= 0x80000000LL ? 0x7fffffff : q < -0x80000000LL ? (int64_t)INT32_MIN : q;
            }
        }
        G_CVT(G)[i] = (int32_t)w;
        return p;
    }
    return fail(gs, ERR_CVT);
}

static const uint8_t *itrp_WCVTF(uint8_t *gs, const uint8_t *p, int op) {  /* itrp_WCVTFOD */
    (void)op;
    NEED(2);
    uint8_t *G = GS_G(gs);
    int32_t v = pop(gs), i = pop(gs);
    if (i < (int32_t)G_CVTCOUNT(G) && i >= 0) {
        G_CVT(G)[i] = AT(G, 0xe8, scale_fn)(G + 0x150, v);
        return p;
    }
    return fail(gs, ERR_CVT);
}

/* ---- measurements ---- */
static const uint8_t *push1(uint8_t *gs, const uint8_t *p, int32_t v) {
    if (GS_SP(gs) + 1 > stack_limit(gs)) return fail(gs, ERR_STACK_OVER);
    *GS_SP(gs)++ = v;
    return p;
}

static const uint8_t *itrp_MPPEM(uint8_t *gs, const uint8_t *p, int op) {
    (void)op;
    uint8_t *G = GS_G(gs);
    uint16_t ppem = AT(G, 0x20, uint16_t);
    uint32_t r = ppem;
    if (AT(G, 0x170, uint8_t) == 0) {
        int32_t s;
        if (GS_PROJ_Y(gs) == 0) s = AT(G, 0x160, int32_t);
        else if (GS_PROJ_X(gs) == 0) s = AT(G, 0x164, int32_t);
        else s = GetCVTScale(gs);
        int64_t v = (int64_t)s * ppem;
        v = (v + 0x8000 + (v >> 63)) >> 16;
        if (v < 0x80000000LL) r = v < -0x80000000LL ? 0 : (uint32_t)v;
        else r = 0xffff;
    }
    return push1(gs, p, (int32_t)(r & 0xffff));
}

static const uint8_t *itrp_MPS(uint8_t *gs, const uint8_t *p, int op) {
    (void)op;
    return push1(gs, p, AT(GS_G(gs), 0x22, uint16_t));
}

/* ---- flow ---- */
/* DAT_1801c1110: inline data length per opcode; 0x15 = NPUSHB, 0x16 = NPUSHW */
static uint8_t inline_len(int op) {
    if (op == 0x40) return 0x15;
    if (op == 0x41) return 0x16;
    if (op >= 0xb0 && op <= 0xb7) return (uint8_t)(op - 0xaf);
    if (op >= 0xb8 && op <= 0xbf) return (uint8_t)((op - 0xb7) * 2);
    return 0;
}

static const uint8_t *skip_inline(uint8_t *gs, const uint8_t *p, int op) {
    uint8_t n = inline_len(op);
    if (!n) return p;
    size_t len = n;
    if (n == 0x15 || n == 0x16) {
        if (p < GS_STARTPTR(gs) || p + 1 > GS_ENDPTR(gs)) { GS_ERROR(gs) = ERR_INSPTR; return GS_ENDPTR(gs); }
        len = n == 0x15 ? (size_t)*p + 1 : (size_t)*p * 2 + 1;
    }
    p += len;
    if (p < GS_STARTPTR(gs)) { GS_ERROR(gs) = ERR_JUMP_BEFORE; return GS_ENDPTR(gs); }
    if (p > GS_ENDPTR(gs)) return GS_ENDPTR(gs);
    return p;
}

#define ERR_IF 0x1105
/* IF 0x18007f130: skips to the matching ELSE or EIF. A push whose data runs past the end ends
 * the skip at the end without an error; reaching the end with the level still open (ELSE at
 * level 1 included) is ERR_IF. */
static const uint8_t *itrp_IF(uint8_t *gs, const uint8_t *p, int op) {
    (void)op;
    NEED(1);
    if (pop(gs) != 0) return p;
    int32_t level = 1;
    while (level != 0) {
        const uint8_t *end = GS_ENDPTR(gs), *start = GS_STARTPTR(gs);
        if (p >= end) break;
        if (p < start) return fail(gs, ERR_INSPTR);
        int c = *p++;
        if (c == 0x59) { level--; continue; }
        if (c == 0x58) { level++; continue; }
        if (c == 0x1b) {
            if (level == 1) break;
            continue;
        }
        size_t n = inline_len(c);
        if (!n) continue;
        if (n == 0x15 || n == 0x16) {
            if (p < start || p + 1 > end) return fail(gs, ERR_INSPTR);
            n = n == 0x15 ? (size_t)*p + 1 : (size_t)*p * 2 + 1;
        }
        p += n;
        if (p < start) return fail(gs, ERR_JUMP_BEFORE);
        if (p > end) return end;
    }
    if (p == GS_ENDPTR(gs) && level != 0) GS_ERROR(gs) = ERR_IF;
    return p;
}

/* ELSE 0x180005010: skips to the matching EIF with a 16-bit level. An inline push that does not
 * fit sets its error and moves to the end, where the open level then sets ERR_IF over it. */
static const uint8_t *itrp_ELSE(uint8_t *gs, const uint8_t *p, int op) {
    (void)op;
    uint16_t level = 1;
    while (level != 0) {
        const uint8_t *end = GS_ENDPTR(gs), *start = GS_STARTPTR(gs);
        if (p >= end) { GS_ERROR(gs) = ERR_IF; return p; }
        if (p < start) return fail(gs, ERR_INSPTR);
        int c = *p++;
        if (c == 0x59) { level--; continue; }
        if (c == 0x58) { level++; continue; }
        size_t n = inline_len(c);
        if (!n) continue;
        if (n == 0x15 || n == 0x16) {
            if (p < start || p + 1 > end) { GS_ERROR(gs) = ERR_INSPTR; p = end; continue; }
            n = n == 0x15 ? (size_t)*p + 1 : (size_t)*p * 2 + 1;
        }
        p += n;
        if (p < start) { GS_ERROR(gs) = ERR_JUMP_BEFORE; p = end; continue; }
        if (p > end) p = end;
    }
    return p;
}

static const uint8_t *itrp_ROLL(uint8_t *gs, const uint8_t *p, int op) {  /* itrp_ROTATE */
    (void)op;
    NEED(3);
    int32_t *sp = GS_SP(gs), t = sp[-2];
    sp[-2] = sp[-1];
    sp[-1] = sp[-3];
    sp[-3] = t;
    return p;
}

static const uint8_t *itrp_SCANCTRL(uint8_t *gs, const uint8_t *p, int op) {
    (void)op;
    NEED(1);
    uint8_t *G = GS_G(gs);
    AT(G, 0x84, uint32_t) = (AT(G, 0x84, uint32_t) & 0xffff0000u) | (uint32_t)pop(gs);
    return p;
}

static const uint8_t *itrp_SCANTYPE(uint8_t *gs, const uint8_t *p, int op) {
    (void)op;
    NEED(1);
    uint8_t *G = GS_G(gs);
    AT(G, 0x84, uint32_t) = ((uint32_t)pop(gs) << 16) | AT(G, 0x84, uint16_t);
    return p;
}

static const uint8_t *itrp_ROUND(uint8_t *gs, const uint8_t *p, int op) {
    NEED(1);
    uint8_t *G = GS_G(gs);
    int32_t *t = GS_SP(gs) - 1;
    *t = G_ROUNDFN(G)(gs, *t, AT(G, 0x24 + (op - 0x68) * 4, int32_t));
    return p;
}

static const uint8_t *itrp_NROUND(uint8_t *gs, const uint8_t *p, int op) {
    NEED(1);
    uint8_t *G = GS_G(gs);
    int32_t *t = GS_SP(gs) - 1;
    *t = itrp_RoundOff(gs, *t, AT(G, 0x24 + (op - 0x6c) * 4, int32_t));
    return p;
}

static const uint8_t *itrp_IllegalInstruction(uint8_t *gs, const uint8_t *p, int op) {
    (void)p; (void)op;
    return fail(gs, 0x1101);
}

/* ======================================================================================
 * Points
 * ====================================================================================== */

#define ERR_POINT 0x1112
typedef int32_t (*project_fn)(uint8_t *gs, int32_t x, int32_t y);
typedef void (*movepoint_fn)(uint8_t *gs, uint8_t *e, int32_t pt, int32_t d);
#define PROJECT(gs, x, y)      ((project_fn)GS_PROJECT(gs))(gs, x, y)
#define OLDPROJECT(gs, x, y)   ((project_fn)GS_OLDPROJECT(gs))(gs, x, y)
#define MOVEPOINT(gs, e, p, d) ((movepoint_fn)GS_MOVEPOINT(gs))(gs, e, p, d)
#define GETCVT(gs, i)          ((getcvt_fn)GS_GETCVT(gs))(gs, i)

/* CHECK_ELEMENT_EP_PTR_ALLOW_OUTOF_BOUND: sanity check of the glyph zone's end-point array */
static int ep_ptr_ok(uint8_t *gs, uint8_t *e) {
    if (e != GS_ELEMENTS(gs) + 0x70) return 1;
    uint8_t *maxp = G_MAXP(GS_G(gs));
    uint16_t contours = AT(maxp, 0xc, uint16_t) > AT(maxp, 8, uint16_t) ? AT(maxp, 0xc, uint16_t) : AT(maxp, 8, uint16_t);
    if (contours == 0) contours = 1;
    uint16_t one = 1, p6 = AT(maxp, 6, uint16_t), p10 = AT(maxp, 10, uint16_t);
    const uint16_t *pts = (p10 > p6 ? p10 : p6) == 0 ? &one : (p10 > p6 ? &AT(maxp, 10, uint16_t) : &AT(maxp, 6, uint16_t));
    int32_t nc = E_NC(e);
    if (nc < 1) return 0;
    if ((uint32_t)contours < (uint32_t)nc)
        return (uint64_t)((int64_t)(nc - 1) * 2) <=
               (uint64_t)(int64_t)(((*pts + 8) * 0xc - AT(e, 0x40, int32_t)) + AT(e, 0x68, int32_t));
    return 1;
}

/* The point check the handlers inline: index range of the zone, then the last end point
 * (plus the 4 phantom points in the glyph zone). */
static int point_ok(uint8_t *gs, uint8_t *e, int32_t pt) {
    uint8_t *G = GS_G(gs);
    int32_t extra;
    if (e == GS_ELEMENTS(gs)) {
        if (pt >= (int32_t)MAXP_TWILIGHT(G_MAXP(G)) || pt < 0) return 0;
        extra = 0;
    } else {
        if (pt >= AT(G, 0x1b0, int32_t) || pt < 0) return 0;
        extra = 4;
    }
    if (!ep_ptr_ok(gs, e) || E_NC(e) < 1) return 0;
    return pt < E_EP(e)[E_NC(e) - 1] + extra + 1;
}

static const uint8_t *itrp_GC(uint8_t *gs, const uint8_t *p, int op) {  /* itrp_RC */
    NEED(1);
    uint8_t *e = GS_CE2(gs);
    int32_t pt = pop(gs);
    if (!point_ok(gs, e, pt)) return fail(gs, ERR_POINT);
    int32_t v = (op & 1) ? OLDPROJECT(gs, E_OX(e)[pt], E_OY(e)[pt]) : PROJECT(gs, E_X(e)[pt], E_Y(e)[pt]);
    *GS_SP(gs)++ = v;
    return p;
}

static const uint8_t *itrp_SCFS(uint8_t *gs, const uint8_t *p, int op) {  /* itrp_WC */
    (void)op;
    NEED(2);
    uint8_t *e = GS_CE2(gs);
    int32_t v = pop(gs), pt = pop(gs);
    if (!point_ok(gs, e, pt)) return fail(gs, ERR_POINT);
    int32_t cur = PROJECT(gs, E_X(e)[pt], E_Y(e)[pt]);
    MOVEPOINT(gs, e, pt, v - cur);
    if (e == GS_ELEMENTS(gs)) {
        E_OX(e)[pt] = E_X(e)[pt];
        E_OY(e)[pt] = E_Y(e)[pt];
    }
    return p;
}

static const uint8_t *itrp_MDAP(uint8_t *gs, const uint8_t *p, int op) {
    NEED(1);
    uint8_t *G = GS_G(gs), *e = GS_CE0(gs);
    int32_t pt = pop(gs), d = 0;
    if (!point_ok(gs, e, pt)) return fail(gs, ERR_POINT);
    GS_RP0(gs) = pt;
    GS_RP1(gs) = pt;
    if (op & 1) {
        int32_t cur = PROJECT(gs, E_X(e)[pt], E_Y(e)[pt]);
        d = G_ROUNDFN(G)(gs, cur, AT(G, 0x24, int32_t)) - cur;
    }
    MOVEPOINT(gs, e, pt, d);
    return p;
}

static const uint8_t *itrp_MIAP(uint8_t *gs, const uint8_t *p, int op) {
    NEED(2);
    uint8_t *G = GS_G(gs), *e = GS_CE0(gs);
    int32_t ci = pop(gs);
    if (ci >= (int32_t)G_CVTCOUNT(G) || ci < 0) return fail(gs, ERR_CVT);
    int32_t cvt = GETCVT(gs, ci);
    int32_t pt = pop(gs);
    if (!point_ok(gs, e, pt)) return fail(gs, ERR_POINT);
    GS_RP0(gs) = pt;
    GS_RP1(gs) = pt;
    if (e == GS_ELEMENTS(gs)) {
        E_X(e)[pt] = mul14(GS_PROJ_X(gs), cvt);
        E_OX(e)[pt] = E_X(e)[pt];
        E_Y(e)[pt] = mul14(GS_PROJ_Y(gs), cvt);
        E_OY(e)[pt] = E_Y(e)[pt];
    }
    int32_t cur = PROJECT(gs, E_X(e)[pt], E_Y(e)[pt]);
    if (op & 1) {
        int32_t diff = cvt - cur;
        if (diff < 0) diff = cur - cvt;
        if (AT(G, 0x78, int32_t) < diff) cvt = cur;
        cvt = G_ROUNDFN(G)(gs, cvt, AT(G, 0x24, int32_t));
    }
    MOVEPOINT(gs, e, pt, cvt - cur);
    return p;
}

static const uint8_t *itrp_GETINFO(uint8_t *gs, const uint8_t *p, int op) {
    (void)op;
    NEED(1);
    uint8_t *G = GS_G(gs);
    uint32_t *t = (uint32_t *)GS_SP(gs) - 1, s = *t, r = (s & 1) ? 0x2a : 0;
    uint8_t ct = AT(G, 0x1c0, uint8_t);
    if (AT(G, 0x1be, uint8_t) == 0) {
        if ((s & 2) && (AT(G, 0x169, uint8_t) & 1)) r |= 0x100;
        if ((s & 4) && (AT(G, 0x169, uint8_t) & 2)) r |= 0x200;
    }
    if (s & 8) r |= 0x400;
    if (s & 0x10) r |= 0x800;
    if ((s & 0x20) && AT(G, 0x194, uint8_t)) r |= 0x1000;
    if ((s & 0x40) && (ct & 1)) r |= 0x2000;
    if ((s & 0x80) && (ct & 2)) r |= 0x4000;
    if ((s & 0x100) && (ct & 4)) r |= 0x8000;
    if ((s & 0x200) && (ct & 8)) r |= 0x10000;
    if ((s & 0x400) && (ct & 0x10)) r |= 0x20000;
    if ((s & 0x800) && (ct & 0x20)) r |= 0x40000;
    if ((s & 0x1000) && (ct & 0x80)) r |= 0x80000;
    *t = r;
    return p;
}

/* FLIPRGON 0x1800ea730: both points checked on the stack, which is popped only on success */
static const uint8_t *itrp_FLIPRGON(uint8_t *gs, const uint8_t *p, int op) {
    (void)op;
    NEED(2);
    uint8_t *e = GS_CE0(gs);
    int32_t hi = GS_SP(gs)[-1], lo = GS_SP(gs)[-2];
    if (!point_ok(gs, e, hi) || !point_ok(gs, e, lo)) return fail(gs, ERR_POINT);
    for (int32_t i = lo; i <= hi; i++) E_ONCURVE(e)[i] |= 1;
    GS_SP(gs) -= 2;
    return p;
}

/* FLIPRGOFF 0x180150810: each point is popped before it is checked */
static const uint8_t *itrp_FLIPRGOFF(uint8_t *gs, const uint8_t *p, int op) {
    (void)op;
    NEED(2);
    uint8_t *e = GS_CE0(gs);
    int32_t hi = *--GS_SP(gs);
    if (!point_ok(gs, e, hi)) return fail(gs, ERR_POINT);
    int32_t lo = *--GS_SP(gs);
    if (!point_ok(gs, e, lo)) return fail(gs, ERR_POINT);
    for (int32_t i = lo; i <= hi; i++) E_ONCURVE(e)[i] &= 0xfe;
    return p;
}

/* FLIPPT 0x1801506a0: the count is SafeInt(loop) + 1, a SafeInt trap when it overflows or is
 * negative */
static const uint8_t *itrp_FLIPPT(uint8_t *gs, const uint8_t *p, int op) {
    (void)op;
    int32_t loop = AT(gs, 0x5c, int32_t);
    if (loop == INT32_MAX || loop + 1 < 0) return trap(gs);
    if (depth(gs) < 0) return trap(gs);
    if ((ptrdiff_t)loop + 1 > depth(gs)) return fail(gs, ERR_STACK_UNDER);
    int32_t *sp = GS_SP(gs);
    for (; loop >= 0; loop--) {
        uint8_t *e = GS_CE0(gs);
        int32_t pt = *--sp;
        if (!point_ok(gs, e, pt)) return fail(gs, ERR_POINT);
        E_ONCURVE(e)[pt] ^= 1;
    }
    AT(gs, 0x5c, int32_t) = 0;
    GS_SP(gs) = sp;
    return p;
}

static const uint8_t *itrp_UTP(uint8_t *gs, const uint8_t *p, int op) {
    (void)op;
    NEED(1);
    uint8_t *e = GS_CE0(gs);
    int32_t pt = pop(gs);
    if (!point_ok(gs, e, pt)) return fail(gs, ERR_POINT);
    if (GS_FREE_X(gs) != 0) E_F(e)[pt] &= 0xfe;
    if (GS_FREE_Y(gs) != 0) E_F(e)[pt] &= 0xfd;
    return p;
}

/* ======================================================================================
 * ClearType compatibility: per-point dependency records at E + 0x68, 12 bytes per point:
 * +0 parent a, +2 parent b (-1 = none), +4 link, +6 flags (bit 0 = conflict)
 * ====================================================================================== */

#define DEP(e, i) ((int16_t *)(AT(e, 0x68, uint8_t *) + (int64_t)(i) * 12))

static int IndirectlyDependsOn(uint8_t *e, int a, int b, int d) {
    d -= 1;
    if (d < 0) return 1;
    int16_t *r = DEP(e, a);
    if (r[0] == -1) return 0;
    if (r[1] == -1) return r[0] == b || IndirectlyDependsOn(e, r[0], b, d - 1);
    if (r[0] == b || r[1] == b) return 1;
    return IndirectlyDependsOn(e, r[0], b, d - 1) || IndirectlyDependsOn(e, r[1], b, d - 1);
}

static int dep_limit(uint8_t *e) { return E_EP(e)[E_NC(e) - 1] + 5; }

static void AddDistance(uint8_t *e, int a, int b, int mode) {
    if (a < 0) return;
    int limit = dep_limit(e);
    if (!(a < limit && b >= 0 && b < limit && a != b)) return;
    int16_t *rb = DEP(e, b);
    int u = a;
    if (IndirectlyDependsOn(e, a, b, 100)) {
        ((uint16_t *)rb)[3] |= 1;
    } else if (rb[0] == -1) {
        int v = DEP(e, u)[0];
        while (v != -1) {
            if (E_OOX(e)[u] != E_OOX(e)[v]) break;
            u = v;
            v = DEP(e, v)[0];
        }
        rb[0] = (int16_t)u;
        rb[1] = -1;
    }
    if (mode == 1) {
        int16_t *ru = DEP(e, u);
        if (ru[2] == -1 && rb[2] != u) {
            int s = ru[0];
            if (s == -1 || DEP(e, s)[2] != u) ru[2] = (int16_t)b;
            else ((uint16_t *)rb)[3] |= 1;
        }
    }
}

static void AddProportion(uint8_t *e, int a, int b, int c) {
    if (a < 0) return;
    int limit = dep_limit(e);
    if (!(a < limit && c >= 0 && c < limit && b >= 0 && b < limit && a != b && c != b && a != c)) return;
    int16_t *rb = DEP(e, b);
    if (IndirectlyDependsOn(e, a, b, 100) || IndirectlyDependsOn(e, c, b, 100)) ((uint16_t *)rb)[3] |= 1;
    else if (rb[0] == -1 && rb[1] == -1) { rb[0] = (int16_t)a; rb[1] = (int16_t)c; }
}

static int InterAlign(uint8_t *e, int a, int pt, int b) {
    int32_t xa = E_OOX(e)[a], xb = E_OOX(e)[b], x = E_OOX(e)[pt];
    int32_t lo = xa <= xb ? xa : xb, hi = xa <= xb ? xb : xa;
    return lo <= x && x <= hi;
}

/* the dependency bookkeeping MDRP and MIRP do before moving a point */
static void track_dependency(uint8_t *gs, uint8_t *e, int32_t rp, int32_t pt, int mode) {
    uint8_t *G = GS_G(gs);
    if (e == GS_ELEMENTS(gs) || AT(G, 0x16b, uint8_t) != 2 || GS_SPFLAG(gs) == 0 || !(AT(G, 0x1c0, uint8_t) & 2)) return;
    int16_t a = AT(gs, 0xce, int16_t), b = AT(gs, 0xd0, int16_t);
    if (a == -1 || b == -1 || !InterAlign(e, a, pt, b)) AddDistance(e, rp, pt, mode);
    else AddProportion(e, a, pt, b);
}

typedef int32_t (*getsw_fn)(uint8_t *gs);
static int32_t CheckSingleWidth(uint8_t *gs, int32_t d) {  /* itrp_CheckSingleWidth */
    int32_t sw = ((getsw_fn)GS_GETSW(gs))(gs), cut = AT(GS_G(gs), 0x7c, int32_t);
    if (d < 0) {
        int32_t diff = -d - sw;
        if (diff < 0) diff = sw + d;
        int32_t r = -d;
        if (diff < cut) r = sw;
        return -r;
    }
    int32_t diff = sw - d;
    if (d - sw >= 0) diff = d - sw;
    return diff < cut ? sw : d;
}

/* scale functions in the global state: f(ctx, funits) -> 26.6 */
#define SCALE_X(G, v) AT(G, 0xd8, scale_fn)(G + 0x130, v)
#define SCALE_Y(G, v) AT(G, 0xe0, scale_fn)(G + 0x140, v)
#define SCALE_P(G, v) AT(G, 0xe8, scale_fn)(G + 0x150, v)

/* Original (unhinted) distance between two points along the dual projection vector. */
static int32_t original_distance(uint8_t *gs, uint8_t *e0, int32_t a, uint8_t *e1, int32_t b) {
    uint8_t *G = GS_G(gs), *tw = GS_ELEMENTS(gs);
    if (e0 == tw || e1 == tw || AT(G, 0x196, uint8_t) != 0)
        return OLDPROJECT(gs, wsub(E_OX(e1)[b], E_OX(e0)[a]), wsub(E_OY(e1)[b], E_OY(e0)[a]));
    int32_t dx = wsub(E_OOX(e1)[b], E_OOX(e0)[a]), dy = wsub(E_OOY(e1)[b], E_OOY(e0)[a]);
    if (AT(G, 0x170, uint8_t) != 0) return SCALE_P(G, OLDPROJECT(gs, dx, dy));
    int32_t sy = SCALE_Y(G, dy);
    int32_t sx = SCALE_X(G, dx);
    return OLDPROJECT(gs, sx, sy);
}

static const uint8_t *itrp_MDRP(uint8_t *gs, const uint8_t *p, int op) {
    uint8_t *G = GS_G(gs), *e0 = GS_CE0(gs), *e1 = GS_CE1(gs);
    int32_t rp0 = GS_RP0(gs);
    NEED(1);
    int32_t pt = pop(gs);
    if (!point_ok(gs, e0, rp0) || !point_ok(gs, e1, pt)) return fail(gs, ERR_POINT);
    track_dependency(gs, e1, rp0, pt, 3);
    int32_t d0 = original_distance(gs, e0, rp0, e1, pt);
    if (AT(G, 0x7c, int32_t) != 0) d0 = CheckSingleWidth(gs, d0);
    int32_t comp = AT(G, 0x24 + (op & 3) * 4, int32_t), d;
    if (op & 4) {
        d = G_ROUNDFN(G)(gs, d0, comp);
    } else {
        d = d0 < 0 ? d0 - comp : comp + d0;
        if ((d ^ d0) < 0 && d0 != 0) d = 0;
    }
    if (op & 8) {
        int32_t md = AT(G, 0x8c, int32_t), lim = GS_SPFLAG(gs) == 0 ? md : md / 2;
        if (d0 < 0) { if (d > -lim) d = -lim; }
        else if (d < lim) d = lim;
    }
    int32_t cur = PROJECT(gs, wsub(E_X(e1)[pt], E_X(e0)[rp0]), wsub(E_Y(e1)[pt], E_Y(e0)[rp0]));
    MOVEPOINT(gs, e1, pt, d - cur);
    GS_RP1(gs) = rp0;
    GS_RP2(gs) = pt;
    if (op & 0x10) GS_RP0(gs) = pt;
    return p;
}

static int contour_of(uint8_t *e, int pt) {  /* 0x18009a0bc */
    int nc = E_NC(e), c = 0;
    while (c < nc && pt > E_EP(e)[c]) c++;
    return c < nc ? c : -1;
}

/* 0x180099d30: refines the dependency mode of a MIRP link between neighbouring points of one
 * contour from the turn direction at both ends (FUnits) and the contour orientation (E + 0x58). */
static int DoubleCheckLinkColor(uint8_t *e, int a, int b, int mode) {
    int ca = contour_of(e, a);
    if (ca < 0) return 0;
    int cb = contour_of(e, b);
    if (cb < 0) return 0;
    if (ca != cb) return mode;
    int s = E_SP(e)[ca], t = E_EP(e)[ca];
    int next_a = a == t ? s : a + 1, prev_a = a == s ? t : a - 1;
    if (b != next_a && b != prev_a) return mode;
    int32_t *X = E_OOX(e), *Y = E_OOY(e);
    int32_t xa = X[a], ya = Y[a];
    int32_t c1 = (int32_t)((uint32_t)wsub(xa, X[prev_a]) * (uint32_t)(wsub(Y[next_a], ya)));
    int32_t c2 = (int32_t)((uint32_t)wsub(ya, Y[prev_a]) * (uint32_t)(wsub(X[next_a], xa)));
    int turn_a = c1 < c2;
    int prev_b = b == s ? t : b - 1, next_b = b == t ? s : b + 1;
    int32_t xb = X[b], yb = Y[b];
    int32_t c3 = (int32_t)((uint32_t)wsub(yb, Y[prev_b]) * (uint32_t)(wsub(X[next_b], xb)));
    int32_t c4 = (int32_t)((uint32_t)wsub(xb, X[prev_b]) * (uint32_t)(wsub(Y[next_b], yb)));
    if (turn_a != (c4 < c3)) return 0;
    int32_t dx = wsub(xb, xa), dy = wsub(yb, ya);
    if (dx < 0) dx = -dx;
    if (dy < 0) dy = -dy;
    if (dx * 2 < dy) return 0;
    return ((~AT(e, 0x58, uint8_t *)[ca] & 1) ^ turn_a) + 1;
}

/* inline 1/16-pixel rounding of the MIRP fast path */
static int32_t round16(int32_t v) { return v < 0 ? -(int32_t)((uint32_t)(2 - v) & 0xfffffffcu) : (int32_t)((uint32_t)(v + 2) & 0xfffffffcu); }
static int32_t round64(int32_t v) { return v < 0 ? -(int32_t)((uint32_t)(0x20 - v) & 0xffffffc0u) : (int32_t)((uint32_t)(v + 0x20) & 0xffffffc0u); }

static int32_t min_distance(uint8_t *gs, int32_t d, int32_t orig) {
    int32_t md = AT(GS_G(gs), 0x8c, int32_t), lim = GS_SPFLAG(gs) == 0 ? md : md / 2;
    if (orig < 0) { if (d > -lim) d = -lim; }
    else if (d < lim) d = lim;
    return d;
}

static int out_of_cutin(int32_t v, int32_t cut) { return v > cut || v < -cut; }

static const uint8_t *itrp_MIRP(uint8_t *gs, const uint8_t *p, int op) {
    uint8_t *G = GS_G(gs);
    NEED(2);
    uint8_t *e0 = GS_CE0(gs), *e1 = GS_CE1(gs), *tw = GS_ELEMENTS(gs);
    int32_t ci = pop(gs);
    if (!((int32_t)G_CVTCOUNT(G) > ci && ci >= 0)) return fail(gs, ERR_CVT);
    int32_t pt = pop(gs);
    if (!point_ok(gs, e1, pt)) return fail(gs, ERR_POINT);
    int32_t rp0 = GS_RP0(gs);
    if (!point_ok(gs, e0, rp0)) return fail(gs, ERR_POINT);
    GS_RP1(gs) = rp0;
    GS_RP2(gs) = pt;
    if (e1 != tw && AT(G, 0x16b, uint8_t) == 2 && GS_SPFLAG(gs) != 0 && (AT(G, 0x1c0, uint8_t) & 2))
        AddDistance(e1, rp0, pt, DoubleCheckLinkColor(e1, rp0, pt, op & 3));

    int32_t cut = AT(G, 0x78, int32_t);
    int sp = GS_SPFLAG(gs) != 0, g88 = (AT(G, 0x88, uint16_t) & 4) != 0;
    if (GS_A4(gs) != 0) {  /* fast path: axis-aligned vectors */
        int x = GS_A4(gs) == 1;
        int32_t cvt = G_CVT(G)[ci];
        int32_t orig = x ? wsub(E_OX(e1)[pt], E_OX(e0)[rp0]) : wsub(E_OY(e1)[pt], E_OY(e0)[rp0]);
        if ((orig ^ cvt) < 0 && G_AUTOFLIP(G)) cvt = -cvt;
        if (sp) {
            if (!g88 && out_of_cutin((cvt - orig) << 4, cut)) cvt = orig;
            if (op & 4) {
                if (g88 && out_of_cutin((cvt - orig) << 4, cut)) cvt = orig;
                cvt = round16(cvt);
            }
        } else if (op & 4) {
            if (out_of_cutin(cvt - orig, cut)) cvt = orig;
            cvt = round64(cvt);
        }
        if (op & 8) cvt = min_distance(gs, cvt, orig);
        if (x) { E_X(e1)[pt] = wadd(E_X(e0)[rp0], cvt); E_F(e1)[pt] |= 1; }
        else { E_Y(e1)[pt] = wadd(E_Y(e0)[rp0], cvt); E_F(e1)[pt] |= 2; }
        if (op & 0x10) GS_RP0(gs) = pt;
        return p;
    }

    int32_t cvt = GETCVT(gs, ci);
    if (AT(G, 0x7c, int32_t) != 0) cvt = CheckSingleWidth(gs, cvt);
    if (e1 == tw) {
        E_OX(e1)[pt] = mul14(GS_PROJ_X(gs), cvt) + E_OX(e0)[rp0];
        E_X(e1)[pt] = E_OX(e1)[pt];
        E_OY(e1)[pt] = mul14(GS_PROJ_Y(gs), cvt) + E_OY(e0)[rp0];
        E_Y(e1)[pt] = E_OY(e1)[pt];
    }
    int32_t orig;
    if (GS_OLDPROJECT(gs) == (void *)itrp_XProject) orig = wsub(E_OX(e1)[pt], E_OX(e0)[rp0]);
    else if (GS_OLDPROJECT(gs) == (void *)itrp_YProject) orig = wsub(E_OY(e1)[pt], E_OY(e0)[rp0]);
    else orig = OLDPROJECT(gs, wsub(E_OX(e1)[pt], E_OX(e0)[rp0]), wsub(E_OY(e1)[pt], E_OY(e0)[rp0]));
    if ((orig ^ cvt) < 0 && G_AUTOFLIP(G)) cvt = -cvt;
    int32_t comp = AT(G, 0x24 + (op & 3) * 4, int32_t), d;
    if (!sp) {
        if (op & 4) {
            if (out_of_cutin(cvt - orig, cut)) cvt = orig;
            d = G_ROUNDFN(G)(gs, cvt, comp);
        } else {
            d = cvt < 0 ? cvt - comp : comp + cvt;
            if ((d ^ cvt) < 0 && cvt != 0) d = 0;
        }
    } else {
        if (!g88 && out_of_cutin((cvt - orig) << 4, cut)) cvt = orig;
        if (op & 4) {
            if (g88 && out_of_cutin((cvt - orig) << 4, cut)) cvt = orig;
            d = G_ROUNDFN(G)(gs, cvt, comp);
        } else {
            d = itrp_RoundOffSP(gs, cvt, comp);
        }
    }
    if (op & 8) d = min_distance(gs, d, orig);
    int32_t cur;
    if (GS_PROJECT(gs) == (void *)itrp_XProject) cur = wsub(E_X(e1)[pt], E_X(e0)[rp0]);
    else if (GS_PROJECT(gs) == (void *)itrp_YProject) cur = wsub(E_Y(e1)[pt], E_Y(e0)[rp0]);
    else cur = PROJECT(gs, wsub(E_X(e1)[pt], E_X(e0)[rp0]), wsub(E_Y(e1)[pt], E_Y(e0)[rp0]));
    MOVEPOINT(gs, e1, pt, d - cur);
    if (op & 0x10) GS_RP0(gs) = pt;
    return p;
}

static int ct_tracking(uint8_t *gs, uint8_t *e) {
    uint8_t *G = GS_G(gs);
    return e != GS_ELEMENTS(gs) && AT(G, 0x16b, uint8_t) == 2 && GS_SPFLAG(gs) != 0 && (AT(G, 0x1c0, uint8_t) & 2);
}

static const uint8_t *itrp_IP(uint8_t *gs, const uint8_t *p, int op) {
    (void)op;
    uint8_t *G = GS_G(gs), *e0 = GS_CE0(gs), *e1 = GS_CE1(gs), *e2 = GS_CE2(gs), *tw = GS_ELEMENTS(gs);
    int32_t count = wadd(AT(gs, 0x5c, int32_t), 1), rp1 = GS_RP1(gs), rp2 = GS_RP2(gs);
    if (!point_ok(gs, e0, rp1) || !point_ok(gs, e1, rp2)) return fail(gs, ERR_POINT);
    int32_t *sp = GS_SP(gs);
    if (count < 0 || depth(gs) < 0) return trap(gs);
    if ((ptrdiff_t)count > depth(gs)) return fail(gs, ERR_STACK_UNDER);
    int use_scaled = e0 == tw || e1 == tw || e2 == tw || AT(G, 0x196, uint8_t) != 0;
    int32_t *OX = use_scaled ? E_OX(e2) : E_OOX(e2), *OY = use_scaled ? E_OY(e2) : E_OOY(e2);
    int32_t ox1 = use_scaled ? E_OX(e0)[rp1] : E_OOX(e0)[rp1], oy1 = use_scaled ? E_OY(e0)[rp1] : E_OOY(e0)[rp1];
    int32_t ox2 = use_scaled ? E_OX(e1)[rp2] : E_OOX(e1)[rp2], oy2 = use_scaled ? E_OY(e1)[rp2] : E_OOY(e1)[rp2];
    int32_t cx1 = E_X(e0)[rp1], cy1 = E_Y(e0)[rp1];
    int32_t orange = OLDPROJECT(gs, ox2 - ox1, oy2 - oy1);
    if (orange == 0) {
        for (; count > 0; count--) {
            int32_t pt = *--sp;
            if (!point_ok(gs, e2, pt)) return fail(gs, ERR_POINT);
            if (ct_tracking(gs, e2)) AddProportion(e2, rp1, pt, rp2);
            int32_t o = PROJECT(gs, wsub(OX[pt], ox1), wsub(OY[pt], oy1));
            int32_t c = PROJECT(gs, wsub(E_X(e2)[pt], cx1), wsub(E_Y(e2)[pt], cy1));
            MOVEPOINT(gs, e2, pt, wsub(o, c));
        }
    } else if (GS_MOVEPOINT(gs) == (void *)itrp_XMovePoint) {
        int32_t crange = wsub(E_X(e1)[rp2], cx1);
        for (; count > 0; count--) {
            int32_t pt = *--sp;
            if (!point_ok(gs, e2, pt)) return fail(gs, ERR_POINT);
            if (ct_tracking(gs, e2)) AddProportion(e2, rp1, pt, rp2);
            E_X(e2)[pt] = wadd(cx1, CompDiv(orange, (int64_t)(wsub(OX[pt], ox1)) * crange));
            E_F(e2)[pt] |= 1;
        }
    } else if (GS_MOVEPOINT(gs) == (void *)itrp_YMovePoint) {
        int32_t crange = wsub(E_Y(e1)[rp2], cy1);
        for (; count > 0; count--) {
            int32_t pt = *--sp;
            if (!point_ok(gs, e2, pt)) return fail(gs, ERR_POINT);
            if (ct_tracking(gs, e2)) AddProportion(e2, rp1, pt, rp2);
            E_Y(e2)[pt] = wadd(cy1, CompDiv(orange, (int64_t)(wsub(OY[pt], oy1)) * crange));
            E_F(e2)[pt] |= 2;
        }
    } else {
        int32_t crange = PROJECT(gs, wsub(E_X(e1)[rp2], cx1), wsub(E_Y(e1)[rp2], cy1));
        for (; count > 0; count--) {
            int32_t pt = *--sp;
            if (!point_ok(gs, e2, pt)) return fail(gs, ERR_POINT);
            if (ct_tracking(gs, e2)) AddProportion(e2, rp1, pt, rp2);
            int32_t o = PROJECT(gs, wsub(OX[pt], ox1), wsub(OY[pt], oy1));
            int32_t nd = CompDiv(orange, (int64_t)o * crange);
            int32_t c = PROJECT(gs, wsub(E_X(e2)[pt], cx1), wsub(E_Y(e2)[pt], cy1));
            MOVEPOINT(gs, e2, pt, wsub(nd, c));
        }
    }
    GS_SP(gs) = sp;
    AT(gs, 0x5c, int32_t) = 0;
    return p;
}

/* ======================================================================================
 * IUP, and the ClearType phase control it runs first on the glyph zone. The phase control
 * walks the dependency records (+8 of each holds the shift found, flags bit 1 = done,
 * bit 2 = on the recursion path) and shifts x by the subpixel phase G + 0x1d0 implies.
 * ====================================================================================== */

#define ERR_IUP 0x111c
#define DEP_FLAGS(e, i) (((uint16_t *)DEP(e, i))[3])
#define DEP_SHIFT(e, i) (*(int32_t *)(DEP(e, i) + 4))

static int32_t add32(int32_t a, int32_t b) { return (int32_t)((uint32_t)a + (uint32_t)b); }

static int64_t phase_product(uint8_t *gs, int32_t x) {
    return (int64_t)x * (int64_t)(int32_t)((uint32_t)AT(GS_G(gs), 0x1d0, int32_t) - 0x10000u);
}

static int32_t CalcAvgXPhaseShift(uint8_t *e, int a, int pt, int b, int32_t sa, int32_t sb) {
    if (sa == sb) return sa;
    int32_t oa = E_OX(e)[a], ob = E_OX(e)[b], o = E_OX(e)[pt];
    int32_t lo = oa, hi = ob, whi = sb, wlo = sa;
    if (oa >= ob) { lo = ob; hi = oa; whi = sa; wlo = sb; }
    if (lo == hi) return add32(whi, wlo) / 2;
    uint32_t n = ((uint32_t)o - (uint32_t)lo) * (uint32_t)whi + ((uint32_t)hi - (uint32_t)o) * (uint32_t)wlo;
    return (int32_t)n / (int32_t)((uint32_t)hi - (uint32_t)lo);
}

static int32_t PhaseShift(uint8_t *gs, uint8_t *e, int inside, int i) {
    if (i == -1) return 0;
    int16_t *r = DEP(e, i);
    uint16_t fl = DEP_FLAGS(e, i);
    if (fl & 4) return 0;
    DEP_FLAGS(e, i) = fl | 4;
    if (!(fl & 2)) {
        int32_t *X = E_X(e);
        int last = E_EP(e)[E_NC(e) - 1];
        int32_t s = 0;
        if (last < i && i <= last + 2) {  /* the horizontal phantom points */
            int64_t v = phase_product(gs, add32(X[i], X[i]));
            v += v < 0 ? -0x10000 : 0x10000;
            s = (int32_t)(v / 0x20000);
        } else if (r[0] == -1) {
            if (inside) s = CompDiv(0x20000, phase_product(gs, add32(X[i], X[i])));
        } else if (r[1] == -1) {
            s = PhaseShift(gs, e, inside, r[0]);
        } else {
            int32_t sb = PhaseShift(gs, e, inside, r[1]);
            int32_t sa = PhaseShift(gs, e, inside, r[0]);
            s = CalcAvgXPhaseShift(e, r[0], i, r[1], sa, sb);
        }
        if (!(DEP_FLAGS(e, i) & 2)) {
            int link = r[2];
            if (link != -1 && !(DEP_FLAGS(e, link) & 2)) {
                if (r[0] == -1 || r[1] == -1) s = CompDiv(0x20000, phase_product(gs, add32(X[link], X[i])));
                X[i] = add32(X[i], s);
                X[link] = add32(X[link], s);
                DEP_FLAGS(e, link) |= 2;
                DEP_SHIFT(e, link) = s;
            } else {
                if (inside && r[1] == -1 && (r[0] == -1 || E_OX(e)[i] != E_OX(e)[r[0]]))
                    s = CompDiv(0x20000, phase_product(gs, add32(X[i], X[i])));
                if (r[0] != -1) X[i] = add32(X[i], s);
            }
            DEP_FLAGS(e, i) |= 2;
            DEP_SHIFT(e, i) = s;
        }
    }
    DEP_FLAGS(e, i) &= ~4;
    return DEP_SHIFT(e, i);
}

static void ExecutePhaseControl(uint8_t *gs, uint8_t *e) {
    int n = dep_limit(e), k = 0;
    while (k < n && !(DEP_FLAGS(e, k) & 1)) k++;
    for (int i = 0; i < n; i++) PhaseShift(gs, e, k < n, i);
    AT(e, 0x60, uint8_t) = 1;
}

/* opcode bit 0: 1 = x, 0 = y */
static const uint8_t *itrp_IUP(uint8_t *gs, const uint8_t *p, int op) {
    uint8_t *G = GS_G(gs), *e = GS_CE2(gs), *tw = GS_ELEMENTS(gs);
    uint8_t mode = AT(G, 0x16b, uint8_t);
    if (mode != 2 && (mode != 0 || e != tw)) return fail(gs, ERR_IUP);
    if (e != tw && mode == 2) {
        uint8_t ct = (uint8_t)G_COMPAT1C0(G);
        if ((ct & 3) == 3 && (ct & 4) != (op & 1) && AT(e, 0x60, uint8_t) == 0) ExecutePhaseControl(gs, e);
    }
    int scaled = AT(G, 0x196, uint8_t) != 0;
    int32_t *cur, *orig, *ref;
    uint8_t mask, *f = E_F(e);
    if (op & 1) { cur = E_X(e); orig = E_OX(e); ref = scaled ? orig : E_OOX(e); mask = 1; }
    else        { cur = E_Y(e); orig = E_OY(e); ref = scaled ? orig : E_OOY(e); mask = 2; }
    for (int c = 0; c < E_NC(e); c++) {
        int32_t s = E_SP(e)[c], last = E_EP(e)[c];
        if (!point_ok(gs, e, s) || !point_ok(gs, e, last)) return fail(gs, ERR_POINT);
        int32_t first = s;
        while (!(f[first] & mask) && first <= last) first++;
        if (first > last) continue;
        int32_t i = first;
        do {
            /* p1: the last touched point of this run, j: the first untouched one after it */
            int32_t p1, j = i;
            do { p1 = j; j = j < last ? j + 1 : s; } while ((f[j] & mask) && j != first);
            if (j == first) break;
            int32_t p2 = j;
            do p2 = p2 < last ? p2 + 1 : s; while (!(f[p2] & mask));
            int32_t lo = p1, hi = p2, rlo = ref[p1];
            int32_t range = (int32_t)((uint32_t)ref[p2] - (uint32_t)ref[p1]);
            if (ref[p1] >= ref[p2]) { lo = p2; hi = p1; rlo = ref[p2]; range = (int32_t)((uint32_t)ref[p1] - (uint32_t)ref[p2]); }
            int32_t clo = cur[lo], olo = orig[lo], dlo = (int32_t)((uint32_t)clo - (uint32_t)olo);
            if (range == 0) {
                for (int32_t k = j; k != p2; k = k < last ? k + 1 : s) cur[k] = add32(cur[k], dlo);
            } else {
                int32_t chi = cur[hi], ohi = orig[hi];
                int32_t dhi = (int32_t)((uint32_t)chi - (uint32_t)ohi), scale = (int32_t)((uint32_t)chi - (uint32_t)clo);
                if (range < 0x8000 && scale < 0x8000) {
                    for (int32_t k = j; k != p2; k = k < last ? k + 1 : s) {
                        int32_t o = orig[k];
                        if (o > olo && o < ohi) {
                            uint32_t n = ((uint32_t)ref[k] - (uint32_t)rlo) * (uint32_t)scale + (uint32_t)(range >> 1);
                            cur[k] = add32((int32_t)n / range, clo);
                        } else {
                            cur[k] = add32(o, o < ohi ? dlo : dhi);
                        }
                    }
                } else {
                    int32_t ratio = tt_FixDiv(scale, range);
                    for (int32_t k = j; k != p2; k = k < last ? k + 1 : s) {
                        int32_t o = orig[k];
                        if (o <= olo) cur[k] = add32(o, dlo);
                        else if (o >= ohi) cur[k] = add32(o, dhi);
                        else cur[k] = add32(tt_FixMul((int32_t)((uint32_t)ref[k] - (uint32_t)rlo), ratio), clo);
                    }
                }
            }
            i = p2;
        } while (i != first);
    }
    G_COMPAT1C2(G) |= (uint16_t)((~op & 1) + 1);
    return p;
}

/* ======================================================================================
 * SHP, SHC, SHZ, SHPIX. The reference point's displacement (rp1 in zp0 for opcode bit 0 = 1,
 * else rp2 in zp1) is split along the freedom vector.
 * ====================================================================================== */

#define ERR_CONTOUR 0x111a
#define ERR_ZONE    0x1116

static void shift_amount(uint8_t *gs, int32_t d, int32_t *dx, int32_t *dy) {
    int16_t pf = GS_PFPROJ(gs), fx = GS_FREE_X(gs), fy = GS_FREE_Y(gs);
    *dx = *dy = 0;
    if (pf == 0x4000) {
        if (fx != 0) *dx = mul14(fx, d);
        if (fy != 0) *dy = mul14(fy, d);
    } else {
        if (fx != 0) *dx = CompDiv(pf, (int64_t)fx * d);
        if (fy != 0) *dy = CompDiv(pf, (int64_t)fy * d);
    }
}

/* itrp_SH_Common: returns the reference zone */
static uint8_t *shift_ref(uint8_t *gs, int op, int32_t *rp, int32_t *dx, int32_t *dy) {
    uint8_t *e = (op & 1) ? GS_CE0(gs) : GS_CE1(gs);
    int32_t r = (op & 1) ? GS_RP1(gs) : GS_RP2(gs);
    shift_amount(gs, PROJECT(gs, wsub(E_X(e)[r], E_OX(e)[r]), wsub(E_Y(e)[r], E_OY(e)[r])), dx, dy);
    *rp = r;
    return e;
}

/* itrp_SHP_Common: pops loop + 1 points of zp2 and moves them; SHPIX (pix) skips the moves
 * ClearType compatibility does not allow */
static const uint8_t *shift_points(uint8_t *gs, const uint8_t *p, int32_t rp, int pix, int32_t dx, int32_t dy) {
    uint8_t *G = GS_G(gs), *e = GS_CE2(gs);
    for (int32_t count = AT(gs, 0x5c, int32_t) + 1; count != 0; count--) {
        int32_t pt = pop(gs);
        if (!point_ok(gs, e, pt)) return fail(gs, ERR_POINT);
        if (ct_tracking(gs, e)) AddDistance(e, rp, pt, 0);
        int move = 1;
        uint16_t c2 = G_COMPAT1C2(G);
        if (pix && (c2 & 0x10)) {
            uint8_t f = E_F(e)[pt], fixed = AT(G, 0x171, uint8_t) != 0;
            move = 0;
            if (!(AT(G, 0x1c0, uint8_t) & 4)) {
                if (GS_PROJ_Y(gs) == 0x4000 && GS_PROJ_X(gs) == 0) move = fixed || ((f >> 1) & 1 & !(c2 & 2));
            } else if (GS_PROJ_X(gs) == 0x4000 && GS_PROJ_Y(gs) == 0) {
                move = fixed || (f & 1 & !(c2 & 1));
            }
        }
        if (!move) continue;
        if (GS_FREE_X(gs) != 0) { E_X(e)[pt] = add32(E_X(e)[pt], dx); E_F(e)[pt] |= 1; }
        if (GS_FREE_Y(gs) != 0) { E_Y(e)[pt] = add32(E_Y(e)[pt], dy); E_F(e)[pt] |= 2; }
    }
    AT(gs, 0x5c, int32_t) = 0;
    return p;
}

static const uint8_t *shift_count_check(uint8_t *gs, int extra) {
    int64_t count = (int64_t)AT(gs, 0x5c, int32_t) + extra;
    if (count > INT32_MAX || count < 0) return trap(gs);
    ptrdiff_t d = depth(gs);
    if (d < 0) return trap(gs);
    if (count > d) return fail(gs, ERR_STACK_UNDER);
    return 0;
}

static const uint8_t *itrp_SHP(uint8_t *gs, const uint8_t *p, int op) {
    uint8_t *e = (op & 1) ? GS_CE0(gs) : GS_CE1(gs);
    int32_t rp = (op & 1) ? GS_RP1(gs) : GS_RP2(gs), dx, dy;
    if (!point_ok(gs, e, rp)) return fail(gs, ERR_POINT);
    shift_ref(gs, op, &rp, &dx, &dy);
    const uint8_t *err = shift_count_check(gs, 1);
    return err ? err : shift_points(gs, p, rp, 0, dx, dy);
}

static const uint8_t *itrp_SHPIX(uint8_t *gs, const uint8_t *p, int op) {
    (void)op;
    const uint8_t *err = shift_count_check(gs, 2);
    if (err) return err;
    int32_t d = pop(gs);
    int16_t fx = GS_FREE_X(gs), fy = GS_FREE_Y(gs);
    return shift_points(gs, p, -1, 1, fx != 0 ? mul14(fx, d) : 0, fy != 0 ? mul14(fy, d) : 0);
}

static const uint8_t *itrp_SHC(uint8_t *gs, const uint8_t *p, int op) {
    uint8_t *G = GS_G(gs), *re = (op & 1) ? GS_CE0(gs) : GS_CE1(gs);
    int32_t rp = (op & 1) ? GS_RP1(gs) : GS_RP2(gs), dx, dy;
    if (!point_ok(gs, re, rp)) return fail(gs, ERR_POINT);
    re = shift_ref(gs, op, &rp, &dx, &dy);
    NEED(1);
    int32_t c = pop(gs);
    if (re != GS_ELEMENTS(gs) && AT(G, 0x16b, uint8_t) == 2 && (AT(G, 0x1c0, uint8_t) & 7) == 3 && AT(re, 0x60, uint8_t) == 0) {
        ExecutePhaseControl(gs, re);
        dx = add32(dx, DEP_SHIFT(re, rp));
    }
    uint8_t mode = AT(G, 0x16b, uint8_t), *e = GS_CE2(gs);
    if (mode != 2 && (mode != 0 || e != GS_ELEMENTS(gs))) return fail(gs, ERR_IUP);
    if (c < 0 || c >= E_NC(e)) return fail(gs, ERR_CONTOUR);
    int32_t s = E_SP(e)[c], last = E_EP(e)[c];
    if (!point_ok(gs, e, s) || !point_ok(gs, e, last)) return fail(gs, ERR_POINT);
    for (int32_t pt = s; pt <= last; pt++) {
        if (pt == rp && re == e) continue;
        if (GS_FREE_X(gs) != 0) { E_X(e)[pt] = add32(E_X(e)[pt], dx); E_F(e)[pt] |= 1; }
        if (GS_FREE_Y(gs) != 0) { E_Y(e)[pt] = add32(E_Y(e)[pt], dy); E_F(e)[pt] |= 2; }
    }
    return p;
}

/* itrp_SHE: the whole zone moves, flags untouched; a reference point in it stays put */
static const uint8_t *itrp_SHZ(uint8_t *gs, const uint8_t *p, int op) {
    uint8_t *G = GS_G(gs), *re = (op & 1) ? GS_CE0(gs) : GS_CE1(gs);
    int32_t rp = (op & 1) ? GS_RP1(gs) : GS_RP2(gs), dx, dy;
    if (!point_ok(gs, re, rp)) return fail(gs, ERR_POINT);
    re = shift_ref(gs, op, &rp, &dx, &dy);
    NEED(1);
    uint32_t z = (uint32_t)pop(gs);
    if (z > 1) return fail(gs, ERR_ZONE);
    uint8_t mode = AT(G, 0x16b, uint8_t), *e = GS_ELEMENTS(gs) + 0x70 * z;
    if (mode != 2 && (mode != 0 || e != GS_ELEMENTS(gs))) return fail(gs, ERR_IUP);
    int32_t last = E_EP(e)[E_NC(e) - 1];
    if (!point_ok(gs, e, last)) return fail(gs, ERR_POINT);
    int32_t first = E_SP(e)[0];
    if (!point_ok(gs, e, first)) return fail(gs, ERR_POINT);
    int32_t sx = 0, sy = 0;
    if (re == e) { sx = E_X(e)[rp]; sy = E_Y(e)[rp]; }
    if (GS_FREE_X(gs) != 0) for (int32_t pt = first; pt <= last; pt++) E_X(e)[pt] = add32(E_X(e)[pt], dx);
    if (GS_FREE_Y(gs) != 0) for (int32_t pt = first; pt <= last; pt++) E_Y(e)[pt] = add32(E_Y(e)[pt], dy);
    if (re == e) { E_X(e)[rp] = sx; E_Y(e)[rp] = sy; }
    return p;
}

static const uint8_t *itrp_MSIRP(uint8_t *gs, const uint8_t *p, int op) {
    uint8_t *G = GS_G(gs), *e0 = GS_CE0(gs), *e1 = GS_CE1(gs);
    int32_t rp0 = GS_RP0(gs);
    NEED(2);
    int32_t d = pop(gs), pt = pop(gs);
    if (!point_ok(gs, e0, rp0) || !point_ok(gs, e1, pt)) return fail(gs, ERR_POINT);
    if (e1 == GS_ELEMENTS(gs)) {
        E_OX(e1)[pt] = mul14(GS_PROJ_X(gs), d) + E_OX(e0)[rp0];
        E_OY(e1)[pt] = mul14(GS_PROJ_Y(gs), d) + E_OY(e0)[rp0];
        E_X(e1)[pt] = E_OX(e1)[pt];
        E_Y(e1)[pt] = E_OY(e1)[pt];
    } else if (AT(G, 0x16b, uint8_t) == 2 && GS_SPFLAG(gs) != 0 && (AT(G, 0x1c0, uint8_t) & 2)) {
        AddDistance(e1, rp0, pt, DoubleCheckLinkColor(e1, rp0, pt, 1));
    }
    if (!(AT(G, 0x88, uint8_t) & 4) && GS_SPFLAG(gs) != 0) {
        int32_t od = OLDPROJECT(gs, wsub(E_OX(e1)[pt], E_OX(e0)[rp0]), wsub(E_OY(e1)[pt], E_OY(e0)[rp0]));
        if (od != 0 && out_of_cutin((int32_t)((uint32_t)(d - od) << 4), AT(G, 0x78, int32_t))) d = od;
    }
    int32_t c = PROJECT(gs, wsub(E_X(e1)[pt], E_X(e0)[rp0]), wsub(E_Y(e1)[pt], E_Y(e0)[rp0]));
    MOVEPOINT(gs, e1, pt, d - c);
    GS_RP1(gs) = rp0;
    GS_RP2(gs) = pt;
    if (op & 1) GS_RP0(gs) = pt;
    return p;
}

static const uint8_t *itrp_ALIGNRP(uint8_t *gs, const uint8_t *p, int op) {
    (void)op;
    uint8_t *e0 = GS_CE0(gs), *e1 = GS_CE1(gs);
    int32_t rp0 = GS_RP0(gs);
    if (!point_ok(gs, e0, rp0)) return fail(gs, ERR_POINT);
    int32_t x0 = E_X(e0)[rp0], y0 = E_Y(e0)[rp0];
    const uint8_t *err = shift_count_check(gs, 1);
    if (err) return err;
    /* the loop counter counts down in the state as points are aligned */
    while (AT(gs, 0x5c, int32_t) >= 0) {
        int32_t pt = pop(gs);
        if (!point_ok(gs, e1, pt)) return fail(gs, ERR_POINT);
        track_dependency(gs, e1, rp0, pt, 3);
        MOVEPOINT(gs, e1, pt, -PROJECT(gs, wsub(E_X(e1)[pt], x0), wsub(E_Y(e1)[pt], y0)));
        AT(gs, 0x5c, int32_t) -= 1;
    }
    AT(gs, 0x5c, int32_t) = 0;
    return p;
}

/* MD[0] (0x49) measures the current outline, MD[1] (0x4a) the original one */
static const uint8_t *itrp_MD(uint8_t *gs, const uint8_t *p, int op) {
    uint8_t *G = GS_G(gs), *e0 = GS_CE0(gs), *e1 = GS_CE1(gs);
    NEED(2);
    int32_t *sp = GS_SP(gs), a = sp[-1], b = sp[-2], d;
    if (((op - 1) & 1) == 0) {
        if (!point_ok(gs, e0, b) || !point_ok(gs, e1, a)) return fail(gs, ERR_POINT);
        d = PROJECT(gs, wsub(E_X(e0)[b], E_X(e1)[a]), wsub(E_Y(e0)[b], E_Y(e1)[a]));
        if ((AT(G, 0x1c2, uint8_t) & 0xb) == 0xb && d == 0x40) d = 0x41;
    } else {
        if (!point_ok(gs, e0, a) || !point_ok(gs, e1, b)) return fail(gs, ERR_POINT);
        d = original_distance(gs, e0, a, e1, b);
    }
    sp[-2] = d;
    GS_SP(gs) = sp - 1;
    return p;
}

/* ======================================================================================
 * DELTAP1-3, DELTAC1-3
 * ====================================================================================== */

/* itrp_DeltaEngine: the (arg, point or cvt) pairs are scanned from the deepest one, which the
 * original takes as sorted by ppem: a binary search finds where to start, and the scan stops
 * at the first pair past the current ppem */
static const uint8_t *delta_engine(uint8_t *gs, const uint8_t *p, void *fn, int16_t base, int16_t shift) {
    uint8_t *G = GS_G(gs);
    NEED(1);
    int32_t n2 = (int32_t)((uint32_t)pop(gs) * 2);
    if (n2 < 0) return trap(gs);
    ptrdiff_t d = depth(gs);
    if (d < 0) return trap(gs);
    if (n2 > d) return fail(gs, ERR_STACK_UNDER);
    int32_t *pairs = GS_SP(gs) -= n2;
    int32_t ppem = AT(G, 0x20, uint16_t);
    if (AT(G, 0x170, uint8_t) == 0) ppem = tt_FixMul(GetCVTScale(gs), ppem);
    uint32_t rel = (uint32_t)ppem - (uint32_t)(int32_t)base;
    if (rel > 15) return p;
    int32_t target = (int32_t)(rel << 4), lo = 0;
    for (uint32_t half = (uint32_t)(n2 >> 1) & ~1u; half > 2; half = (half >> 1) & ~1u)
        if ((pairs[lo + (int32_t)half] & ~0xf) < target) lo += (int32_t)half;
    for (int32_t i = lo; i < n2; i += 2) {
        int32_t key = pairs[i] & ~0xf;
        if (key > target) return p;
        if (key != target) continue;
        int32_t mag = pairs[i] & 0xf, idx = pairs[i + 1];
        int32_t step = (int32_t)((uint32_t)(mag - (mag < 8 ? 8 : 7)) << 6) >> (shift & 31);
        uint8_t *e = GS_CE0(gs);
        if (fn == GS_MOVEPOINT(gs)) {
            if (!point_ok(gs, e, idx)) return fail(gs, ERR_POINT);
            uint16_t c0 = G_COMPAT1C0(G);
            if ((c0 & 1) && !(AT(G, 0x88, uint8_t) & 4)) {
                uint8_t f = E_F(e)[(int16_t)idx], c2 = AT(G, 0x1c2, uint8_t), fixed = AT(G, 0x171, uint8_t) != 0;
                int ok = !(c0 & 4) ? GS_PROJ_Y(gs) == 0x4000 && GS_PROJ_X(gs) == 0 && (fixed || ((f & 2) && !(c2 & 2)))
                                   : GS_PROJ_X(gs) == 0x4000 && GS_PROJ_Y(gs) == 0 && (fixed || ((f & 1) && !(c2 & 1)));
                if (!ok) continue;
            }
        } else if (!((int32_t)G_CVTCOUNT(G) > idx && idx >= 0)) {
            return fail(gs, ERR_CVT);
        }
        ((movepoint_fn)fn)(gs, e, idx, step);
    }
    return p;
}

/* DELTAP1-3 (0x5d, 0x71, 0x72) move points, DELTAC1-3 (0x73-0x75) change CVT entries; the
 * second and third start 16 and 32 ppem above the delta base */
static const uint8_t *itrp_DELTA(uint8_t *gs, const uint8_t *p, int op) {
    uint8_t *G = GS_G(gs);
    int range = op == 0x71 || op == 0x74 ? 1 : op == 0x72 || op == 0x75 ? 2 : 0;
    void *fn = op == 0x5d || op == 0x71 || op == 0x72 ? GS_MOVEPOINT(gs) : GS_CHANGECVT(gs);
    return delta_engine(gs, p, fn, (int16_t)(G_DELTABASE(G) + 16 * range), G_DELTASHIFT(G));
}

/* ======================================================================================
 * Vectors from lines, ISECT, ALIGNPTS
 * ====================================================================================== */

/* itrp_Normalize: unit vector (2.14) along (dx, dy) */
static void Normalize(int32_t dx, int32_t dy, int16_t *v) {
    int32_t s;
    if (dx == 0 && dy == 0) { v[0] = 0x4000; v[1] = 0; return; }
    if (dx < 0x7fff && dx > -0x8000 && (uint32_t)dy + 0x7fffu <= 0xfffd) {
        int c = 15;
        for (s = (int32_t)((uint32_t)dx * (uint32_t)dx + (uint32_t)dy * (uint32_t)dy); s < 0x20000000; c++)
            s = (int32_t)((uint32_t)s << 2);
        dx = (int32_t)((uint32_t)dx << c);
        dy = (int32_t)((uint32_t)dy << c);
    } else {
        while (dx < 0x20000000 && dx > -0x20000000 && (uint32_t)dy + 0x1fffffffu <= 0x3ffffffe) { dx += dx; dy += dy; }
        int64_t yy = (int64_t)dy * dy, xx = (int64_t)dx * dx;
        s = add32(sat32((yy + (yy >> 63) + 0x20000000) >> 30), sat32((xx + (xx >> 63) + 0x20000000) >> 30));
    }
    int32_t root = tt_FracSqrt(s), half = root / 2;
    int rneg = root < 0;
    int64_t nx = (int64_t)dx * 0x40000000, ny = (int64_t)dy * 0x40000000;
    nx += rneg == (nx < 0) ? half : -half;
    v[0] = (int16_t)((int32_t)((uint32_t)sat32(root ? nx / root : (int64_t)(nx < 0) + 0x7fffffff) + 0x8000) >> 16);
    ny += rneg == (ny < 0) ? half : -half;
    v[1] = (int16_t)((int32_t)((uint32_t)sat32(root ? ny / root : (int64_t)(ny < 0) + 0x7fffffff) + 0x8000) >> 16);
}

/* subpixel flag of the projection vector: set unless it lies along the subpixel axis */
static int16_t proj_spflag(uint8_t *gs) {
    uint16_t c = G_COMPAT1C0(GS_G(gs));
    if (!(c & 1)) return 0;
    if (!(c & 4)) return !(GS_PROJ_Y(gs) == 0x4000 && GS_PROJ_X(gs) == 0);
    return !(GS_PROJ_X(gs) == 0x4000 && GS_PROJ_Y(gs) == 0);
}

static void rotate90(int16_t *v) { int16_t x = v[0]; v[0] = (int16_t)-v[1]; v[1] = x; }

/* SPVTL (0x06/0x07), SDPVTL (0x86/0x87): projection along the line p2 (zp1) - p1 (zp2),
 * rotated by 90 degrees for opcode bit 0; SDPVTL also sets the dual vector from the
 * original outline */
static const uint8_t *vector_to_line(uint8_t *gs, const uint8_t *p, int op, int dual) {
    NEED(2);
    int32_t p1 = pop(gs), p2 = pop(gs);
    uint8_t *e2 = GS_CE2(gs), *e1 = GS_CE1(gs);
    if (!point_ok(gs, e2, p1) || !point_ok(gs, e1, p2)) return fail(gs, ERR_POINT);
    AT(gs, 0xce, int16_t) = (int16_t)p1;
    AT(gs, 0xd0, int16_t) = (int16_t)p2;
    Normalize(wsub(E_X(e1)[p2], E_X(e2)[p1]), wsub(E_Y(e1)[p2], E_Y(e2)[p1]), &GS_PROJ_X(gs));
    if (dual) Normalize(wsub(E_OX(e1)[p2], E_OX(e2)[p1]), wsub(E_OY(e1)[p2], E_OY(e2)[p1]), &GS_OPROJ_X(gs));
    if (op & 1) {
        rotate90(&GS_PROJ_X(gs));
        if (dual) rotate90(&GS_OPROJ_X(gs));
    }
    compute_pf_proj(gs);
    GS_MOVEPOINT(gs) = (void *)itrp_MovePoint;
    GS_PROJECT(gs) = (void *)itrp_Project;
    GS_SPFLAG(gs) = proj_spflag(gs);
    select_round(gs);
    GS_OLDPROJECT(gs) = dual ? (void *)itrp_OldProject : GS_PROJECT(gs);
    GS_A4(gs) = 0;
    return p;
}
static const uint8_t *itrp_SPVTL(uint8_t *gs, const uint8_t *p, int op) { return vector_to_line(gs, p, op, 0); }
static const uint8_t *itrp_SDPVTL(uint8_t *gs, const uint8_t *p, int op) { return vector_to_line(gs, p, op, 1); }

static const uint8_t *itrp_SFVTL(uint8_t *gs, const uint8_t *p, int op) {
    NEED(2);
    int32_t p1 = pop(gs), p2 = pop(gs);
    uint8_t *e2 = GS_CE2(gs), *e1 = GS_CE1(gs);
    if (!point_ok(gs, e2, p1) || !point_ok(gs, e1, p2)) return fail(gs, ERR_POINT);
    Normalize(wsub(E_X(e1)[p2], E_X(e2)[p1]), wsub(E_Y(e1)[p2], E_Y(e2)[p1]), &GS_FREE_X(gs));
    if (op & 1) rotate90(&GS_FREE_X(gs));
    compute_pf_proj(gs);
    GS_A4(gs) = 0;
    GS_MOVEPOINT(gs) = (void *)itrp_MovePoint;
    return p;
}

static int32_t abs32(int32_t v) { return v < 0 ? (int32_t)(0u - (uint32_t)v) : v; }
static int32_t sub32(int32_t a, int32_t b) { return (int32_t)((uint32_t)a - (uint32_t)b); }

/* 26.6 product, rounded */
static int32_t Mul26Dot6(int32_t a, int32_t b) {
    if (a < 0xb505 && b < 0xb505 && a > -0xb505 && b > -0xb505) return (b * a + 0x20) >> 6;
    uint32_t ua = a < 0 ? 0u - (uint32_t)a : (uint32_t)a, ub = (uint32_t)b, neg = (uint32_t)a >> 31;
    if (b < 0) { ub = 0u - ub; neg ^= 1; }
    uint32_t mid = (ub >> 16) * (ua & 0xffff) + (ua >> 16) * (ub & 0xffff);
    uint32_t add = mid * 0x10000 + 0x20, lo = (ua & 0xffff) * (ub & 0xffff) + add;
    uint32_t r = ((ub >> 16) * (ua >> 16) + (mid >> 16) + (lo < add)) * 0x4000000 | lo >> 6;
    return (int32_t)(neg ? 0u - r : r);
}

/* ISECT: p (zp2) goes to the intersection of line a0-a1 (zp0) with line b0-b1 (zp1) */
static const uint8_t *itrp_ISECT(uint8_t *gs, const uint8_t *p, int op) {
    (void)op;
    uint8_t *G = GS_G(gs), *e0 = GS_CE0(gs), *e1 = GS_CE1(gs), *e2 = GS_CE2(gs);
    NEED(5);
    int32_t *sp = GS_SP(gs), a1 = sp[-1], a0 = sp[-2];
    if (!point_ok(gs, e0, a1) || !point_ok(gs, e0, a0)) return fail(gs, ERR_POINT);
    int32_t ax = E_X(e0)[a0], ay = E_Y(e0)[a0], adx = sub32(E_X(e0)[a1], ax), ady = sub32(E_Y(e0)[a1], ay);
    int ct = AT(G, 0x16b, uint8_t) == 2 && (AT(G, 0x1c0, uint8_t) & 2);
    int32_t pa = 0, pb = 0;  /* dependency parents: the line closer to the point's direction */
    if (ct) { pa = a0; pb = a1; }
    int32_t b1 = sp[-3], b0 = sp[-4];
    if (!point_ok(gs, e1, b1) || !point_ok(gs, e1, b0)) return fail(gs, ERR_POINT);
    int32_t bx = E_X(e1)[b0], by = E_Y(e1)[b0], bdx = sub32(E_X(e1)[b1], bx), bdy = sub32(E_Y(e1)[b1], by);
    if (ct && abs32(Mul26Dot6(bdy, adx)) > abs32(Mul26Dot6(bdx, ady))) { pa = b0; pb = b1; }
    int32_t pt = sp[-5];
    if (!point_ok(gs, e2, pt)) return fail(gs, ERR_POINT);
    GS_SP(gs) = sp - 5;
    if (e2 != GS_ELEMENTS(gs) && ct) AddProportion(e2, pa, pt, pb);
    E_F(e2)[pt] |= 3;
    int32_t *X = E_X(e2), *Y = E_Y(e2), num, den;
    if (ady == 0) {
        if (bdx == 0) { X[pt] = bx; Y[pt] = ay; return p; }
        num = sub32(by, ay);
        den = (int32_t)(0u - (uint32_t)bdy);
    } else if (adx == 0) {
        if (bdy == 0) { X[pt] = ax; Y[pt] = by; return p; }
        num = sub32(bx, ax);
        den = (int32_t)(0u - (uint32_t)bdx);
    } else if (abs32(adx) < abs32(ady)) {
        num = add32(sub32(ax, bx), CompDiv(ady, (int64_t)sub32(by, ay) * adx));
        den = sub32(bdx, CompDiv(ady, (int64_t)bdy * adx));
    } else {
        num = sub32(sub32(by, ay), CompDiv(adx, (int64_t)sub32(bx, ax) * ady));
        den = sub32(CompDiv(adx, (int64_t)bdx * ady), bdy);
    }
    if (den == 0) {  /* parallel: the middle of the four points */
        X[pt] = add32(add32(add32(bdx >> 1, ax), adx >> 1), bx) >> 1;
        Y[pt] = add32(add32(add32(bdy >> 1, ay), ady >> 1), by) >> 1;
    } else {
        Y[pt] = add32(CompDiv(den, (int64_t)bdy * num), by);
        X[pt] = add32(CompDiv(den, (int64_t)bdx * num), bx);
    }
    return p;
}

static const uint8_t *itrp_ALIGNPTS(uint8_t *gs, const uint8_t *p, int op) {
    (void)op;
    uint8_t *e0 = GS_CE0(gs), *e1 = GS_CE1(gs);
    NEED(2);
    int32_t p1 = pop(gs), p2 = pop(gs);
    if (!point_ok(gs, e1, p1) || !point_ok(gs, e0, p2)) return fail(gs, ERR_POINT);
    int32_t dx = wsub(E_X(e1)[p1], E_X(e0)[p2]), dy = wsub(E_Y(e1)[p1], E_Y(e0)[p2]), d;
    if (GS_PROJECT(gs) == (void *)itrp_XProject) d = dx;
    else if (GS_PROJECT(gs) == (void *)itrp_YProject) d = dy;
    else d = PROJECT(gs, dx, dy);
    MOVEPOINT(gs, e0, p2, d >> 1);
    MOVEPOINT(gs, e1, p1, (d >> 1) - d);
    return p;
}

/* ======================================================================================
 * Interpreter loops, function and instruction definitions, calls
 * ====================================================================================== */

#define GS_INTERP_FN(gs)  AT(gs, 0x80, void *)     /* loop CALL and friends run bodies with */
#define GS_CALLS(gs)      AT(gs, 0xc4, int32_t)    /* nesting left */
#define G_FDEFS(G)        AT(G, 0xb8, uint8_t *)   /* 8 bytes each: offset, length, program */
#define G_IDEFS(G)        AT(G, 0xc0, uint8_t *)   /* 8 bytes each: offset, length, program, opcode */
#define G_NIDEFS(G)       AT(G, 0x16c, int32_t)
#define G_PGM(G, i)       AT(G, 0xf0 + 0x10 * (i), const uint8_t *)   /* 0 fpgm, 1 prep */
#define G_PGMLEN(G, i)    AT(G, 0xf8 + 0x10 * (i), uint32_t)
#define MAXP_FDEFS(m)     AT(m, 0x14, uint16_t)
#define MAXP_IDEFS(m)     AT(m, 0x16, uint16_t)

#define ERR_NO_ENDF     0x1104
#define ERR_NESTING     0x1106
#define ERR_FDEF_PGM    0x1108
#define ERR_IDEF_PGM    0x1109
#define ERR_INTERP      0x110a
#define ERR_RAW         0x110d
#define ERR_FDEF        0x1114
#define ERR_PGM         0x1115
#define ERR_IDEF_OP     0x1117
#define ERR_IDEF_COUNT  0x1118

typedef void (*interp_fn)(uint8_t *gs, const uint8_t *p, const uint8_t *end);
typedef void (*trace_fn)(uint8_t *gs, const uint8_t *end);

/* Negative control (tt_opcode_check.py --control opcode, edge_texture_check.py --control opcode=N):
 * built with -DIDWR_NEGATIVE_CONTROL_OPCODE=N, the port's handler of opcode tt_control_opcode
 * (N, or set at run time) also flips bit 30 of rp0 and moves glyph point 0 one pixel right */
#ifdef IDWR_NEGATIVE_CONTROL_OPCODE
TT_EXPORT int tt_control_opcode = IDWR_NEGATIVE_CONTROL_OPCODE;
static void control_perturb(uint8_t *gs, int op) {
    if (op != tt_control_opcode) return;
    GS_RP0(gs) ^= 0x40000000;
    uint8_t *e = GS_ELEMENTS(gs) + 0x70;
    if (E_NC(e) > 0 && E_X(e)) E_X(e)[0] = wadd(E_X(e)[0], 64);
}
#define CONTROL_PERTURB(gs, op) control_perturb(gs, op)
#else
#define CONTROL_PERTURB(gs, op) ((void)0)
#endif

/* one handler, as the loops below run it (the lockstep harness steps the port with this) */
TT_EXPORT const uint8_t *tt_step(uint8_t *gs, const uint8_t *p, int op) {
    const uint8_t *r = tt_opcodes[op](gs, p, op);
    CONTROL_PERTURB(gs, op);
    return r;
}

/* itrp_InnerExecute: PUSHB[0], DUP and POP are inlined */
void tt_InnerExecute(uint8_t *gs, const uint8_t *p, const uint8_t *end) {
    GS_ENDPTR(gs) = end;
    GS_STARTPTR(gs) = p;
    while (p < end) {
        int op = *p;
        if (--GS_BUDGET(gs) == 0) { GS_ERROR(gs) = ERR_BUDGET; return; }
        p++;
        if (op == 0xb0) {
            if (GS_SP(gs) + 1 > stack_limit(gs)) { GS_ERROR(gs) = ERR_STACK_OVER; p = GS_ENDPTR(gs); }
            else if (p < GS_STARTPTR(gs) || p + 1 > GS_ENDPTR(gs)) { GS_ERROR(gs) = ERR_INSPTR; p = GS_ENDPTR(gs); }
            else *GS_SP(gs)++ = *p++;
        } else if (op == 0x20 || op == 0x21) {
            ptrdiff_t d = depth(gs);
            if (d < 0) { p = trap(gs); continue; }
            if (d < 1) { GS_ERROR(gs) = ERR_STACK_UNDER; p = GS_ENDPTR(gs); continue; }
            if (op == 0x21) { GS_SP(gs)--; continue; }
            int32_t v = GS_SP(gs)[-1];
            if (GS_SP(gs) + 1 > stack_limit(gs)) { GS_ERROR(gs) = ERR_STACK_OVER; p = GS_ENDPTR(gs); }
            else *GS_SP(gs)++ = v;
        } else {
            p = tt_opcodes[op](gs, p, op);
            CONTROL_PERTURB(gs, op);
        }
    }
}

/* itrp_InnerTraceExecute: the trace function runs before every instruction and can stop the
 * program by clearing itself */
static void trace_loop(uint8_t *gs, const uint8_t *p, const uint8_t *end, int hook) {
    GS_ENDPTR(gs) = end;
    GS_STARTPTR(gs) = p;
    while (p < end) {
        if (hook && !GS_TRACE(gs)) return;
        GS_INSPTR(gs) = p;
        GS_OPCODE(gs) = *p;
        if (hook) {
            ((trace_fn)GS_TRACE(gs))(gs, end);
            if (!GS_TRACE(gs)) return;
        }
        int op = *p;
        if (--GS_BUDGET(gs) == 0) { GS_ERROR(gs) = ERR_BUDGET; return; }
        p = tt_opcodes[op](gs, p + 1, op);
        CONTROL_PERTURB(gs, op);
    }
}
void tt_InnerTraceExecute(uint8_t *gs, const uint8_t *p, const uint8_t *end) { trace_loop(gs, p, end, 1); }
/* the lockstep harness stands this in for the original's trace loop, so that function bodies
 * the port runs do not call back into the harness */
static void inner_trace_nohook(uint8_t *gs, const uint8_t *p, const uint8_t *end) { trace_loop(gs, p, end, 0); }

static void InitPhaseControl(uint8_t *e) {
    for (int i = 0; i < dep_limit(e); i++) {
        int16_t *r = DEP(e, i);
        r[0] = r[1] = r[2] = -1;
        DEP_FLAGS(e, i) = 0;
        DEP_SHIFT(e, i) = 0;
    }
    AT(e, 0x60, uint8_t) = 0;
}

/* itrp_Execute: runs one program (fpgm, prep or a glyph's) on copies of the two zone structs,
 * with a fresh local state: vectors along x, budgets from G + 0x198..0x1a0, the CVT
 * functions for the program kind (G + 0x16b: 0 fpgm, 1 prep, 2 glyph) and, for glyphs in
 * ClearType compatibility mode, the phase control around the program. Returns the
 * interpreter error. */
TT_EXPORT int32_t tt_itrp_Execute(const uint8_t *twilight, const uint8_t *glyph, const uint8_t *p,
                                  const uint8_t *end, uint8_t *G, void *trace) {
    _Alignas(16) uint8_t frame[0xe0 + 2 * 0x70];
    uint8_t *gs = frame, *zones = frame + 0xe0, *ge = zones + 0x70;
    memset(gs, 0, 0xd8);
    memcpy(zones, twilight, 0x70);
    memcpy(ge, glyph, 0x70);
    GS_CE0(gs) = GS_CE1(gs) = GS_CE2(gs) = ge;
    GS_ELEMENTS(gs) = zones;
    AT(gs, 0x18, uint32_t) = AT(gs, 0x1c, uint32_t) = AT(gs, 0x20, uint32_t) = 0x4000;
    GS_PFPROJ(gs) = 0x4000;
    GS_MOVEPOINT(gs) = (void *)itrp_XMovePoint;
    GS_PROJECT(gs) = GS_OLDPROJECT(gs) = (void *)itrp_XProject;
    GS_JUMPS(gs) = AT(G, 0x198, int32_t);
    AT(gs, 0xc4, int32_t) = AT(G, 0x19c, int32_t);
    GS_BUDGET(gs) = AT(G, 0x1a0, int32_t);
    GS_G(gs) = G;
    AT(gs, 0xd0, uint16_t) = AT(gs, 0xce, uint16_t) = 0xffff;
    GS_A4(gs) = AT(G, 0x28, int32_t) == 0;
    uint8_t kind = AT(G, 0x16b, uint8_t);
    int fast = 1;
    if (kind != 1) {
        G_COMPAT1C2(G) &= 0xffe4;
        uint16_t c = G_COMPAT1C0(G);
        int sp = 0;
        if ((c & 1) && !(c & 4)) {
            GS_SPFLAG(gs) = 1;
            sp = (AT(G, 0x88, uint8_t) & 4) || kind != 0;
        }
        G_ROUNDFN(G) = round_table[G_ROUNDSTATE(G) + (sp ? 8 : 0)];
        if (AT(G, 0x20, uint16_t) <= 1) return 0;  /* ppem 0 or 1: nothing to run */
        if (AT(G, 0x170, uint8_t) == 0) { fast = 0; GS_A4(gs) = 0; }
    }
    GS_GETCVT(gs) = fast ? (void *)itrp_GetCVTEntryFast : (void *)itrp_GetCVTEntrySlow;
    GS_GETSW(gs) = fast ? (void *)itrp_GetSingleWidthFast : (void *)itrp_GetSingleWidthSlow;
    GS_CHANGECVT(gs) = fast ? (void *)itrp_ChangeCvtFast : (void *)itrp_ChangeCvtSlow;
    if (kind != 1 && AT(G, 0xae, int16_t) != 0) {
        AT(G, 0x80, int32_t) = SCALE_P(G, AT(G, 0xae, int16_t));
        GS_A4(gs) = 0;
    }
    GS_SP(gs) = G_STACKBASE(G);
    GS_TRACE(gs) = trace;
    GS_ERROR(gs) = 0;
    interp_fn run = trace ? tt_InnerTraceExecute : tt_InnerExecute;
    GS_INTERP_FN(gs) = (void *)run;
    int ct = kind == 2 && (AT(G, 0x1c0, uint8_t) & 3) == 3;
    if (ct) InitPhaseControl(ge);
    run(gs, p, end);
    if (ct && AT(ge, 0x60, uint8_t) == 0) ExecutePhaseControl(gs, ge);
    AT(G, 0x1a0, int32_t) = GS_BUDGET(gs);
    return GS_ERROR(gs);
}

static int interp_ok(void *f) {
    return f == (void *)tt_InnerExecute || f == (void *)tt_InnerTraceExecute || f == (void *)inner_trace_nohook;
}

/* the body of a definition record, checked against its program; 0 when out of bounds */
static int def_body(uint8_t *G, uint8_t *rec, int pgm, int loopcall, const uint8_t **start, const uint8_t **stop) {
    uintptr_t base = (uintptr_t)G_PGM(G, pgm), s = base + (intptr_t)AT(rec, 0, int32_t);
    uintptr_t e = loopcall ? base + (intptr_t)(int32_t)((uint32_t)AT(rec, 0, int32_t) + AT(rec, 4, uint16_t))
                           : s + AT(rec, 4, uint16_t);
    if (s < base || e < s || e > base + G_PGMLEN(G, pgm)) return 0;
    *start = (const uint8_t *)s;
    *stop = (const uint8_t *)e;
    return 1;
}

static const uint8_t *itrp_CALL(uint8_t *gs, const uint8_t *p, int op) {
    (void)op;
    uint8_t *G = GS_G(gs);
    NEED(1);
    int32_t f = pop(gs);
    if (!(f < MAXP_FDEFS(G_MAXP(G)) && f >= 0)) return fail(gs, ERR_FDEF);
    uint8_t *rec = G_FDEFS(G) + 8 * (int64_t)f;
    uint16_t pgm = AT(rec, 6, uint16_t);
    if (pgm >= 2) return fail(gs, ERR_PGM);
    const uint8_t *start, *stop;
    if (!def_body(G, rec, pgm, 0, &start, &stop)) return fail(gs, ERR_JUMP_BEFORE);
    const uint8_t *saved_end = GS_ENDPTR(gs), *saved_start = GS_STARTPTR(gs);
    /* ClearType compatibility: functions FDEF recognised get part of the state neutralised
     * while they run (G + 0x1c2 bits 0x100, 0x200, 0x800, list at G + 0x1c4) */
    int compat = (AT(G, 0x1c0, uint8_t) & 1) && !(AT(G, 0x88, uint8_t) & 4);
    int st24 = 0, st22 = 0, cutin = 0, listed = 0;
    int32_t v24 = 0, v22 = 0, vcut = 0;
    if (f < 0x40) {
        if ((f <= 2 || f == 4 || f == 7 || f == 8) && compat && (G_COMPAT1C2(G) & 0x200)) {
            st24 = 1; v24 = G_STORAGE(G)[24]; G_STORAGE(G)[24] = 0;
        }
        if (f == 0x3a && compat && (G_COMPAT1C2(G) & 0x800)) {
            cutin = 1; vcut = AT(G, 0x78, int32_t); AT(G, 0x78, int32_t) = 0x7fffffff;
        }
    } else if (f <= 0x42 && compat && (G_COMPAT1C2(G) & 0x100)) {
        st22 = 1; v22 = G_STORAGE(G)[22]; G_STORAGE(G)[22] = 0;
    }
    for (uint16_t i = 0; i < AT(G, 0x1c4, uint16_t); i++) {
        if (AT(G, 0x1c6 + 2 * i, int16_t) != (int16_t)f) continue;
        if (compat) { G_COMPAT1C2(G) |= 0x10; listed = 1; }
        break;
    }
    void *run = GS_INTERP_FN(gs);
    if (!interp_ok(run)) return fail(gs, ERR_INTERP);
    if (--GS_CALLS(gs) == 0) return fail(gs, ERR_NESTING);
    ((interp_fn)run)(gs, start, stop);
    GS_CALLS(gs)++;
    if (listed) G_COMPAT1C2(G) &= 0xffef;
    if (cutin) AT(G, 0x78, int32_t) = vcut;
    if (st24) G_STORAGE(G)[24] = v24;
    if (st22) G_STORAGE(G)[22] = v22;
    GS_ENDPTR(gs) = saved_end;
    GS_STARTPTR(gs) = saved_start;
    return GS_ERROR(gs) ? saved_end : p;
}

static const uint8_t *itrp_LOOPCALL(uint8_t *gs, const uint8_t *p, int op) {
    (void)op;
    uint8_t *G = GS_G(gs);
    NEED(2);
    int32_t f = pop(gs);
    if (!(f < MAXP_FDEFS(G_MAXP(G)) && f >= 0)) return fail(gs, ERR_FDEF);
    uint8_t *rec = G_FDEFS(G) + 8 * (int64_t)f;
    uint16_t pgm = AT(rec, 6, uint16_t);
    if (pgm >= 2) return fail(gs, ERR_PGM);
    const uint8_t *start, *stop;
    if (!def_body(G, rec, pgm, 1, &start, &stop)) return fail(gs, ERR_JUMP_BEFORE);
    void *run = GS_INTERP_FN(gs);
    if (!interp_ok(run)) return fail(gs, ERR_INTERP);
    const uint8_t *saved_end = GS_ENDPTR(gs), *saved_start = GS_STARTPTR(gs);
    int32_t n = pop(gs);
    if (--GS_CALLS(gs) == 0) { GS_ERROR(gs) = ERR_NESTING; return saved_end; }
    for (int32_t k = (int32_t)((uint32_t)n - 1); k >= 0 && GS_ERROR(gs) == 0; k = (int32_t)((uint32_t)k - 1))
        ((interp_fn)run)(gs, start, stop);
    GS_CALLS(gs)++;
    GS_ENDPTR(gs) = saved_end;
    GS_STARTPTR(gs) = saved_start;
    return GS_ERROR(gs) ? saved_end : p;
}

static int code_at(uint8_t *gs, const uint8_t *p, const char *pat, size_t n) {
    if (p < GS_STARTPTR(gs) || p + n > GS_ENDPTR(gs)) return 0;
    for (size_t i = 0; i < n; i++) if (p[i] != (uint8_t)pat[i]) return 0;
    return 1;
}

static const uint8_t *itrp_FDEF(uint8_t *gs, const uint8_t *p, int op) {
    (void)op;
    uint8_t *G = GS_G(gs);
    NEED(1);
    int32_t f = pop(gs);
    if (!(f < MAXP_FDEFS(G_MAXP(G)) && f >= 0)) return fail(gs, ERR_FDEF);
    uint8_t pgm = AT(G, 0x16b, uint8_t);
    if (pgm >= 2) return fail(gs, ERR_FDEF_PGM);
    uint8_t *rec = G_FDEFS(G) + 8 * (int64_t)f;
    AT(rec, 6, uint16_t) = pgm;
    AT(rec, 0, int32_t) = (int32_t)((uint32_t)(uintptr_t)p - (uint32_t)(uintptr_t)G_PGM(G, pgm));
    /* functions the ClearType compatibility mode recognises by their first instructions */
    if (f < 0x40) {
        if ((f <= 2 || f == 4 || f == 7 || f == 8) &&
            (code_at(gs, p, "\x01\xb0\x18\x43\x58", 5) || code_at(gs, p, "\x01\x18\xb0\x18\x43\x58", 6)))
            G_COMPAT1C2(G) |= 0x200;
        if (f == 0 && code_at(gs, p, "\x45\x23\x46\x60\x20\xb0\x26", 7)) G_COMPAT1C2(G) |= 0x400;
        if (f == 0x3a && code_at(gs, p, "\x20\x20\xb0\x01\x60\x46\xb0\x40\x23\x42", 10)) G_COMPAT1C2(G) |= 0x800;
    } else if (f <= 0x42 && code_at(gs, p, "\xb0\x16\x43\x58", 4)) {
        G_COMPAT1C2(G) |= 0x100;
    }
    if (code_at(gs, p, "\x4b", 1) &&
        (code_at(gs, p, "\x4b\x54\x58\x38\x1b\x21\x21\x59\x2d", 9) ||
         code_at(gs, p, "\x4b\x53\x23\x4b\x51\x5a\x58\x38\x1b\x21\x21\x59\x2d", 13))) {
        uint16_t n = AT(G, 0x1c4, uint16_t);
        if (n < 4) { AT(G, 0x1c6 + 2 * n, int16_t) = (int16_t)f; AT(G, 0x1c4, uint16_t) = n + 1; }
    }
    /* skip to ENDF */
    const uint8_t *q = p;
    if (q < GS_STARTPTR(gs) || q + 1 > GS_ENDPTR(gs)) return fail(gs, ERR_INSPTR);
    for (;;) {
        if (*q == 0x2d || q + 1 >= GS_ENDPTR(gs)) {
            if (*q != 0x2d) GS_ERROR(gs) = ERR_NO_ENDF;
            AT(rec, 4, uint16_t) = (uint16_t)(q - p);
            return q + 1;
        }
        int n = inline_len(*q);
        const uint8_t *next = q + 1;
        if (n == 0x15 || n == 0x16) {
            if (q + 2 > GS_ENDPTR(gs)) return fail(gs, ERR_INSPTR);
            n = n == 0x15 ? q[1] + 1 : q[1] * 2 + 1;
        }
        if (n) {
            next = q + 1 + n;
            if (next < GS_STARTPTR(gs)) { GS_ERROR(gs) = ERR_JUMP_BEFORE; next = GS_ENDPTR(gs); }
            else if (next > GS_ENDPTR(gs)) next = GS_ENDPTR(gs);
        }
        if (next < GS_STARTPTR(gs) || next + 1 > GS_ENDPTR(gs)) return fail(gs, ERR_INSPTR);
        q = next;
    }
}

static uint8_t *FindIDef(uint8_t *gs, uint8_t op) {  /* itrp_FindIDef */
    uint8_t *G = GS_G(gs), *r = G_IDEFS(G);
    for (int32_t n = G_NIDEFS(G) - 1; n >= 0; n--, r += 8)
        if (r[7] == op) return r;
    return 0;
}

static const uint8_t *SkipPushData(uint8_t *gs, const uint8_t *p) {  /* itrp_SkipPushData */
    if (p - 1 < GS_STARTPTR(gs) || p > GS_ENDPTR(gs)) return fail(gs, ERR_INSPTR);
    return skip_inline(gs, p, p[-1]);
}

static const uint8_t *itrp_IDEF(uint8_t *gs, const uint8_t *p, int op) {
    (void)op;
    uint8_t *G = GS_G(gs);
    uint8_t pgm = AT(G, 0x16b, uint8_t);
    if (pgm >= 2) return fail(gs, ERR_IDEF_PGM);
    NEED(1);
    uint32_t v = (uint32_t)pop(gs);
    if (v & 0xffffff00u) return fail(gs, ERR_IDEF_OP);
    uint8_t *rec = FindIDef(gs, (uint8_t)v);
    if (!rec) {
        int32_t n = G_NIDEFS(G);
        if (n >= MAXP_IDEFS(G_MAXP(G))) return fail(gs, ERR_IDEF_COUNT);
        rec = G_IDEFS(G) + 8 * (int64_t)n;
        G_NIDEFS(G) = n + 1;
    }
    rec[6] = pgm;
    rec[7] = (uint8_t)v;
    AT(rec, 0, int32_t) = (int32_t)((uint32_t)(uintptr_t)p - (uint32_t)(uintptr_t)G_PGM(G, pgm));
    const uint8_t *q = p;
    for (;;) {
        if (q < GS_STARTPTR(gs) || q + 1 > GS_ENDPTR(gs)) return fail(gs, ERR_INSPTR);
        if (*q == 0x2d || q + 1 >= GS_ENDPTR(gs)) {
            if (*q != 0x2d) GS_ERROR(gs) = ERR_NO_ENDF;
            AT(rec, 4, uint16_t) = (uint16_t)(q - p);
            return q + 1;
        }
        q = SkipPushData(gs, q + 1);
    }
}

/* the handler of every opcode without a meaning of its own: runs the font's IDEF for it */
static const uint8_t *itrp_IDefPatch(uint8_t *gs, const uint8_t *p, int op) {
    uint8_t *G = GS_G(gs);
    const uint8_t *saved_end = GS_ENDPTR(gs), *saved_start = GS_STARTPTR(gs);
    uint8_t *rec = FindIDef(gs, (uint8_t)op);
    if (!rec) return fail(gs, 0x1101);
    if (rec[6] >= 2) return fail(gs, ERR_PGM);
    const uint8_t *start, *stop;
    if (!def_body(G, rec, rec[6], 0, &start, &stop)) return fail(gs, ERR_JUMP_BEFORE);
    if (--GS_CALLS(gs) == 0) return fail(gs, ERR_NESTING);
    ((interp_fn)GS_INTERP_FN(gs))(gs, start, stop);
    GS_CALLS(gs)++;
    GS_ENDPTR(gs) = saved_end;
    GS_STARTPTR(gs) = saved_start;
    return GS_ERROR(gs) ? saved_end : p;
}

static const uint8_t *itrp_INSTCTRL(uint8_t *gs, const uint8_t *p, int op) {
    (void)op;
    uint8_t *G = GS_G(gs);
    NEED(2);
    int32_t sel = pop(gs);
    uint32_t v = (uint32_t)pop(gs);
    if (AT(G, 0x16a, uint8_t) != 0) {
        if (sel == 1) G_FLAGS88(G) &= ~1u;
        else if (sel == 2) G_FLAGS88(G) &= ~2u;
        else if (sel == 3) G_FLAGS88(G) &= ~4u;
        G_FLAGS88(G) |= v;  /* for any selector, as the original does */
    }
    return p;
}

/* RAW (0x28): the advance width of the glyph, from the original phantom points */
static const uint8_t *itrp_RAW(uint8_t *gs, const uint8_t *p, int op) {
    (void)op;
    if (AT(GS_G(gs), 0x16b, uint8_t) != 2) return fail(gs, ERR_RAW);
    uint8_t *e = GS_ELEMENTS(gs) + 0x70;
    int32_t last = E_EP(e)[E_NC(e) - 1];
    if (GS_SP(gs) + 1 > stack_limit(gs)) return fail(gs, ERR_STACK_OVER);
    *GS_SP(gs)++ = E_OX(e)[last + 2] - E_OX(e)[last + 1];
    return p;
}

static const uint8_t *itrp_GETVARIATION(uint8_t *gs, const uint8_t *p, int op) {
    uint8_t *G = GS_G(gs);
    int32_t n = AT(G, 0x1d8, int32_t);
    const int16_t *coords = AT(G, 0x1e0, const int16_t *);
    if (n == 0) return itrp_IDefPatch(gs, p, op);
    int set = AT(G, 0x1d5, uint8_t) != 0;
    do {
        if (GS_SP(gs) + 1 > stack_limit(gs)) return fail(gs, ERR_STACK_OVER);
        *GS_SP(gs)++ = set ? *coords++ : 0;
        n = (int32_t)((uint32_t)n - 1);
    } while (n != 0);
    return p;
}

/* handlers that run other code (the lockstep harness compares them when they return) */
int tt_nests(int op) {
    tt_handler h = tt_opcodes[op];
    return h == itrp_CALL || h == itrp_LOOPCALL || h == itrp_IDefPatch || h == itrp_GETVARIATION;
}

/* Function slots the original stores in the state (gs + 0x68..0x98, G + 0x90): RVA of the
 * original, port equivalent. The lockstep harness translates between them. */
const tt_fnmap tt_fn_map[] = {
    {0xd6120, (void *)itrp_XMovePoint}, {0xcf860, (void *)itrp_YMovePoint}, {0xcfd30, (void *)itrp_MovePoint},
    {0xd82b0, (void *)itrp_XProject}, {0xd5a40, (void *)itrp_YProject}, {0xd81f0, (void *)itrp_Project},
    {0xdce60, (void *)itrp_OldProject},
    {0xacc10, (void *)itrp_GetCVTEntryFast}, {0x66620, (void *)itrp_GetCVTEntrySlow},
    {0xf0ed0, (void *)itrp_GetSingleWidthFast}, {0xf2c40, (void *)itrp_GetSingleWidthSlow},
    {0xd81d0, (void *)itrp_ChangeCvtFast}, {0xea920, (void *)itrp_ChangeCvtSlow},
    {0xad020, (void *)itrp_RoundToGrid}, {0xe56c0, (void *)itrp_RoundToHalfGrid},
    {0xeaaf0, (void *)itrp_RoundToDoubleGrid}, {0xd6540, (void *)itrp_RoundDownToGrid},
    {0xde130, (void *)itrp_RoundUpToGrid}, {0xdacc0, (void *)itrp_RoundOff},
    {0xcc4b0, (void *)itrp_SuperRound}, {0x150f90, (void *)itrp_Super45Round},
    {0xce080, (void *)itrp_RoundToGridSP}, {0xe07b0, (void *)itrp_RoundToHalfGridSP},
    {0xf0f40, (void *)itrp_RoundToDoubleGridSP}, {0xdd340, (void *)itrp_RoundDownToGridSP},
    {0xeba40, (void *)itrp_RoundUpToGridSP}, {0x9a770, (void *)itrp_RoundOffSP},
    {0x3940, (void *)tt_InnerExecute}, {0x150b80, (void *)inner_trace_nohook},
    {0, 0},
};

tt_handler tt_opcodes[256] = {
    [0x00] = itrp_SVTCA_0, [0x01] = itrp_SVTCA_1, [0x02] = itrp_SPVTCA_0, [0x03] = itrp_SPVTCA_1,
    [0x04] = itrp_SFVTCA_0, [0x05] = itrp_SFVTCA_1,
    [0x0a] = itrp_WPV, [0x0b] = itrp_WFV, [0x0c] = itrp_RPV, [0x0d] = itrp_RFV, [0x0e] = itrp_SFVTPV,
    [0x13] = itrp_SZP, [0x14] = itrp_SZP, [0x15] = itrp_SZP, [0x16] = itrp_SZP,
    [0x17] = itrp_SLOOP, [0x18] = itrp_RTG, [0x19] = itrp_RTHG, [0x1a] = itrp_SMD, [0x1b] = itrp_ELSE,
    [0x1d] = itrp_SCVTCI, [0x1e] = itrp_SSWCI, [0x1f] = itrp_SSW,
    [0x2d] = itrp_IllegalInstruction, [0x3d] = itrp_RTDG,
    [0x44] = itrp_WCVTP, [0x45] = itrp_RCVT, [0x4b] = itrp_MPPEM, [0x4c] = itrp_MPS,
    [0x4f] = itrp_AA, [0x58] = itrp_IF,
    [0x68] = itrp_ROUND, [0x69] = itrp_ROUND, [0x6a] = itrp_ROUND, [0x6b] = itrp_ROUND,
    [0x6c] = itrp_NROUND, [0x6d] = itrp_NROUND, [0x6e] = itrp_NROUND, [0x6f] = itrp_NROUND,
    [0x70] = itrp_WCVTF, [0x76] = itrp_SROUND, [0x77] = itrp_S45ROUND, [0x7a] = itrp_ROFF,
    [0x7b] = itrp_IllegalInstruction, [0x7c] = itrp_RUTG, [0x7d] = itrp_RDTG, [0x7e] = itrp_SANGW,
    [0x7f] = itrp_AA, [0x85] = itrp_SCANCTRL, [0x8a] = itrp_ROLL, [0x8d] = itrp_SCANTYPE,
    [0x29] = itrp_UTP, [0x2e] = itrp_MDAP, [0x2f] = itrp_MDAP, [0x3e] = itrp_MIAP, [0x3f] = itrp_MIAP,
    [0x46] = itrp_GC, [0x47] = itrp_GC, [0x48] = itrp_SCFS, [0x80] = itrp_FLIPPT,
    [0x81] = itrp_FLIPRGON, [0x82] = itrp_FLIPRGOFF, [0x88] = itrp_GETINFO, [0x39] = itrp_IP,
    [0x30] = itrp_IUP, [0x31] = itrp_IUP, [0x32] = itrp_SHP, [0x33] = itrp_SHP, [0x34] = itrp_SHC,
    [0x35] = itrp_SHC, [0x36] = itrp_SHZ, [0x37] = itrp_SHZ, [0x38] = itrp_SHPIX,
    [0x3a] = itrp_MSIRP, [0x3b] = itrp_MSIRP, [0x3c] = itrp_ALIGNRP, [0x49] = itrp_MD, [0x4a] = itrp_MD,
    [0x5d] = itrp_DELTA, [0x71] = itrp_DELTA, [0x72] = itrp_DELTA, [0x73] = itrp_DELTA, [0x74] = itrp_DELTA,
    [0x75] = itrp_DELTA, [0x06] = itrp_SPVTL, [0x07] = itrp_SPVTL, [0x08] = itrp_SFVTL, [0x09] = itrp_SFVTL,
    [0x86] = itrp_SDPVTL, [0x87] = itrp_SDPVTL, [0x0f] = itrp_ISECT, [0x27] = itrp_ALIGNPTS,
    [0x28] = itrp_RAW, [0x2a] = itrp_LOOPCALL, [0x2b] = itrp_CALL, [0x2c] = itrp_FDEF, [0x89] = itrp_IDEF,
    [0x8e] = itrp_INSTCTRL, [0x91] = itrp_GETVARIATION,
    [0x83] = itrp_IDefPatch, [0x84] = itrp_IDefPatch, [0x8f] = itrp_IDefPatch, [0x90] = itrp_IDefPatch, [0x92] = itrp_IDefPatch,
    [0x93] = itrp_IDefPatch, [0x94] = itrp_IDefPatch, [0x95] = itrp_IDefPatch, [0x96] = itrp_IDefPatch, [0x97] = itrp_IDefPatch,
    [0x98] = itrp_IDefPatch, [0x99] = itrp_IDefPatch, [0x9a] = itrp_IDefPatch, [0x9b] = itrp_IDefPatch, [0x9c] = itrp_IDefPatch,
    [0x9d] = itrp_IDefPatch, [0x9e] = itrp_IDefPatch, [0x9f] = itrp_IDefPatch, [0xa0] = itrp_IDefPatch, [0xa1] = itrp_IDefPatch,
    [0xa2] = itrp_IDefPatch, [0xa3] = itrp_IDefPatch, [0xa4] = itrp_IDefPatch, [0xa5] = itrp_IDefPatch, [0xa6] = itrp_IDefPatch,
    [0xa7] = itrp_IDefPatch, [0xa8] = itrp_IDefPatch, [0xa9] = itrp_IDefPatch, [0xaa] = itrp_IDefPatch, [0xab] = itrp_IDefPatch,
    [0xac] = itrp_IDefPatch, [0xad] = itrp_IDefPatch, [0xae] = itrp_IDefPatch, [0xaf] = itrp_IDefPatch,
#define R4(n) [n] = itrp_MDRP, [n + 1] = itrp_MDRP, [n + 2] = itrp_MDRP, [n + 3] = itrp_MDRP
    R4(0xc0), R4(0xc4), R4(0xc8), R4(0xcc), R4(0xd0), R4(0xd4), R4(0xd8), R4(0xdc),
#undef R4
#define R4(n) [n] = itrp_MIRP, [n + 1] = itrp_MIRP, [n + 2] = itrp_MIRP, [n + 3] = itrp_MIRP
    R4(0xe0), R4(0xe4), R4(0xe8), R4(0xec), R4(0xf0), R4(0xf4), R4(0xf8), R4(0xfc),
#undef R4
    [0x10] = itrp_SRP0, [0x11] = itrp_SRP1, [0x12] = itrp_SRP2,
    [0x1c] = itrp_JMPR,
    [0x20] = itrp_DUP, [0x21] = itrp_POP, [0x22] = itrp_CLEAR, [0x23] = itrp_SWAP,
    [0x24] = itrp_DEPTH, [0x25] = itrp_CINDEX, [0x26] = itrp_MINDEX,
    [0x40] = itrp_NPUSHB, [0x41] = itrp_NPUSHW, [0x42] = itrp_WS, [0x43] = itrp_RS,
    [0x4d] = itrp_FLIPON, [0x4e] = itrp_FLIPOFF,
    [0x50] = itrp_LT, [0x51] = itrp_LTEQ, [0x52] = itrp_GT, [0x53] = itrp_GTEQ,
    [0x54] = itrp_EQ, [0x55] = itrp_NEQ, [0x56] = itrp_ODD, [0x57] = itrp_EVEN,
    [0x59] = itrp_EIF, [0x5a] = itrp_AND, [0x5b] = itrp_OR, [0x5c] = itrp_NOT,
    [0x5e] = itrp_SDB, [0x5f] = itrp_SDS,
    [0x60] = itrp_ADD, [0x61] = itrp_SUB, [0x62] = itrp_DIV, [0x63] = itrp_MUL,
    [0x64] = itrp_ABS, [0x65] = itrp_NEG, [0x66] = itrp_FLOOR, [0x67] = itrp_CEILING,
    [0x78] = itrp_JROT, [0x79] = itrp_JROF,
    [0x8b] = itrp_MAX, [0x8c] = itrp_MIN,
    [0xb0] = itrp_PUSHB1,
    [0xb1] = itrp_PUSHB, [0xb2] = itrp_PUSHB, [0xb3] = itrp_PUSHB, [0xb4] = itrp_PUSHB,
    [0xb5] = itrp_PUSHB, [0xb6] = itrp_PUSHB, [0xb7] = itrp_PUSHB,
    [0xb8] = itrp_PUSHW1,
    [0xb9] = itrp_PUSHW, [0xba] = itrp_PUSHW, [0xbb] = itrp_PUSHW, [0xbc] = itrp_PUSHW,
    [0xbd] = itrp_PUSHW, [0xbe] = itrp_PUSHW, [0xbf] = itrp_PUSHW,
};
