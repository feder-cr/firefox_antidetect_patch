/*
 * Outline orientation check and emboldening (bold simulation) of the TrueType scaler in
 * DWrite.dll 10.0.26100.9711, ported from the assembly:
 *
 *   fsg_CheckOutlineOrientation 0x180010280   per contour: is it wound the wrong way?
 *   Misoriented                 0x180010d80   ray-crossing test from one extreme knot
 *   CurveTransitions            0x180011bd4   ray x quadratic Bezier crossings
 *   QDiv2                       0x180012adc   rounded 64-bit divide, clamped to 16.16 [0, 1]
 *   fsg_Embold                  0x180064acc   emboldens the glyph outline
 *   EmboldPoint                 0x180064ea8   offsets one point (or a run of equal points)
 *   Intersect26Dot6             0x180065188   intersection of two lines, 26.6
 *
 * fsg_ExecuteGlyph calls fsg_CheckOutlineOrientation for every simple glyph, right after
 * sfac_ReadOutlineData. Its only output is bit 0 of the per-contour flag byte (zone + 0x58),
 * read by fsg_Embold (direction of the offset) and by DoubleCheckLinkColor (0x180099d30, from
 * itrp_MIRP/itrp_MSIRP: link colour under ClearType compatibility, visible in ClearType
 * gdi_classic without bold; found with a hardware watchpoint). fsg_Embold runs from fs__Contour and
 * fsg_GridFit when the transformation has an emboldening amount (fs input + 0xc2/0xc4,
 * nonzero = 0x14 when RenderingFlags bit 1, bold simulation, is set).
 *
 * Mul26Dot6 (0x180064a20) and itrp_Normalize (0x180066018) are copied from interp.c.
 */
#include "tt_state.h"
#include "orig.h"

#define RVA_CompDiv 0x6da90

/* zone fields used here besides tt_state.h's */
#define E_FC(e) AT(e, 0x58, uint8_t *)  /* per-contour flags, bit 0 = misoriented */

typedef struct { int32_t x, y; } pt;  /* passed by value in one 64-bit register, like the original */

#ifdef _WIN32
__declspec(dllimport) void __stdcall OutputDebugStringA(const char *);
#endif

/* ---- wrapping integer helpers (the original's 32/64-bit register arithmetic) ---- */
static inline int32_t add32(int32_t a, int32_t b) { return (int32_t)((uint32_t)a + (uint32_t)b); }
static inline int32_t sub32(int32_t a, int32_t b) { return (int32_t)((uint32_t)a - (uint32_t)b); }
static inline int32_t mul32(int32_t a, int32_t b) { return (int32_t)((uint32_t)a * (uint32_t)b); }
static inline int32_t neg32(int32_t a) { return (int32_t)(0u - (uint32_t)a); }
static inline int32_t abs32(int32_t v) { return v < 0 ? neg32(v) : v; }  /* NEG/CMOVS: INT_MIN stays */
static inline int64_t add64(int64_t a, int64_t b) { return (int64_t)((uint64_t)a + (uint64_t)b); }
static inline int64_t sub64(int64_t a, int64_t b) { return (int64_t)((uint64_t)a - (uint64_t)b); }
static inline int64_t mul64(int64_t a, int64_t b) { return (int64_t)((uint64_t)a * (uint64_t)b); }
static inline int64_t neg64(int64_t a) { return (int64_t)(0u - (uint64_t)a); }
static inline int64_t shl64(int64_t a, int n) { return (int64_t)((uint64_t)a << n); }

/* ---- copied from interp.c (Mul26Dot6, itrp_Normalize), kept identical ---- */
int32_t tt_FracSqrt(int32_t x); /* interp.c, DWRITE_FracSqrt */

static int32_t sat32(int64_t v) {  /* CHKOVF */
    if (v >= 0x80000000LL) return 0x7fffffff;
    if (v < -0x80000000LL) return INT32_MIN;
    return (int32_t)v;
}

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

static int32_t CompDiv(int32_t d, int64_t n) { return ORIG(int32_t (*)(int32_t, int64_t), RVA_CompDiv)(d, n); }

/* Calls inside the group go through the original entry points: with one function swapped
 * the others are the originals, and a swapped callee is reached through its hook. */
#define QDiv2(n, d) ORIG(int64_t (*)(int64_t, int64_t), 0x12adc)(n, d)
#define CurveTransitions ORIG(int32_t (*)(pt, pt, pt, pt, pt), 0x11bd4)
#define Misoriented ORIG(uint32_t (*)(int32_t, int16_t, uint16_t, pt, uint8_t *), 0x10d80)
#define Intersect26Dot6 ORIG(void (*)(pt, pt, pt, pt, int32_t *), 0x65188)
#define EmboldPoint ORIG(void (*)(uint32_t, uint32_t, uint8_t, uint8_t, pt, pt, pt, int32_t, int32_t,                                   int32_t, int32_t, int32_t, uint8_t, uint8_t *), 0x64ea8)


/* ======================================================================================
 * Orientation (fsglue.c: FindExtremaKnot, CheckBoundingBoxSegment/Curve)
 * ====================================================================================== */

/* QDiv2: n / d rounded, as a 16.16 parameter clamped to just outside [0, 1]: 1 or -1 when
 * |n| < |d|, +-0x10001 when |n| > |d| << 16 (callers only accept 1..0x10000) */
TT_EXPORT int64_t tt_QDiv2(int64_t n, int64_t d) {
    if ((n < 0) == (d < 0)) {
        int64_t an = n < 0 ? neg64(n) : n, ad = n < 0 ? neg64(d) : d;
        if (an < ad) return 1;
        if (an > shl64(ad, 16)) return 0x10001;
        return add64(ad >> 1, an) / ad;
    }
    int64_t an = n < 0 ? neg64(n) : n, ad = n < 0 ? d : neg64(d);
    if (an < ad) return -1;
    if (an <= shl64(ad, 16)) return neg64(add64(ad >> 1, an) / ad);
    return -0x10001;
}

/* CurveTransitions: crossings of the ray A -> B with the quadratic Bezier p0, p1 (control),
 * p2, all in doubled font units. Solves (B(t) - A) x (B - A) = 0 for t in 16.16 and counts
 * the roots in (0, 1] whose point lies on the ray side 0 < s <= 1. */
TT_EXPORT int32_t tt_CurveTransitions(pt A, pt B, pt p0, pt p1, pt p2) {
    int32_t rx = sub32(B.x, A.x), ry = sub32(B.y, A.y);
    int32_t ax = add32(sub32(p0.x, add32(p1.x, p1.x)), p2.x);
    int32_t ay = add32(sub32(p0.y, add32(p1.y, p1.y)), p2.y);
    int32_t bx = add32(sub32(p1.x, p0.x), sub32(p1.x, p0.x));
    int32_t by = add32(sub32(p1.y, p0.y), sub32(p1.y, p0.y));
    int32_t c0 = add32(sub32(sub32(mul32(rx, p0.y), mul32(rx, A.y)), mul32(p0.x, ry)), mul32(A.x, ry));
    int64_t c1 = sub32(mul32(by, rx), mul32(bx, ry));
    int32_t c2 = sub32(mul32(ay, rx), mul32(ax, ry));
    int64_t t[2] = {0, 0};
    int n;
    if (c2 == 0) {
        if (c1 == 0) return 0;
        t[0] = QDiv2(neg64(shl64(c0, 16)), c1);
        n = 1;
    } else {
        int64_t a2 = add64(c2, c2);
        int64_t disc = sub64(mul64(c1, c1), add64(mul64(a2, c0), mul64(a2, c0)));
        if (disc <= 0) {
            if (disc != 0) return 0;
            t[0] = QDiv2(neg64(shl64(c1, 16)), a2);
            n = 1;
        } else {
            /* square root in 16.16, bit by bit (the remainder doubles every step) */
            uint64_t rem = (uint64_t)disc, root = 0, bit = 0x4000000000000000ull;
            do {
                if (root + bit <= rem) { rem -= root + bit; root |= bit * 2; }
                rem += rem;
                bit >>= 1;
            } while (bit >= 0x8000);
            int64_t s = (int64_t)(root + 0x8000) >> 16, c1s = shl64(c1, 16);
            t[0] = QDiv2(sub64(s, c1s), a2);
            t[1] = QDiv2(neg64(add64(c1s, s)), a2);
            n = 2;
        }
    }
    int32_t count = 0;
    for (int i = 0; i < n; i++) {
        int64_t u = t[i], v;
        if ((uint64_t)(u - 1) > 0xffff) continue;
        if (abs32(rx) > abs32(ry)) {
            v = add64(mul64(ax, u), shl64(bx, 16));
            v = add64(mul64(v, u), shl64(sub32(p0.x, A.x), 32));
            if (rx >= 0 ? v > 0 && v <= shl64(rx, 32) : shl64(rx, 32) <= v && v < 0) count++;
        } else {
            v = add64(mul64(ay, u), shl64(by, 16));
            v = add64(mul64(v, u), shl64(sub32(p0.y, A.y), 32));
            if (ry >= 0 ? v > 0 && v <= shl64(ry, 32) : shl64(ry, 32) <= v && v < 0) count++;
        }
    }
    return count;
}

/* ray A -> B against the segment p -> c (CheckBoundingBoxSegment passed): 1 if it crosses */
static int32_t segment_crossing(pt A, pt B, pt p, pt c) {
    int32_t rdx = sub32(B.x, A.x), rdy = sub32(B.y, A.y), sdx = sub32(c.x, p.x), sdy = sub32(c.y, p.y);
    int32_t den32 = sub32(mul32(rdx, sdy), mul32(rdy, sdx));
    if (den32 == 0) return 0;
    int64_t den = den32, t;
    int32_t num = add32(sub32(sub32(mul32(A.x, rdy), mul32(rdx, A.y)), mul32(p.x, rdy)), mul32(rdx, p.y));
    int64_t nn = shl64(-(int64_t)num, 16), an = nn < 0 ? neg64(nn) : nn;
    /* t = nn / den rounded, as QDiv2 inlined (|nn| < |den| gives 1 without the range check) */
    if ((nn < 0) == (den < 0)) {
        if (nn < 0) den = neg64(den);
        if (an < den) t = 1;
        else {
            if (an > shl64(den, 16)) return 0;
            t = add64(den >> 1, an) / den;
            if ((uint64_t)(t - 1) > 0xffff) return 0;
        }
    } else {
        if (nn >= 0) den = neg64(den);
        if (an < den || an > shl64(den, 16)) return 0;
        t = neg64(add64(den >> 1, an) / den);
        if ((uint64_t)(t - 1) > 0xffff) return 0;
    }
    int64_t s;
    if (abs32(rdx) > abs32(rdy)) {
        s = add64(shl64(sub32(p.x, A.x), 16), mul64(sdx, t));
        return rdx >= 0 ? s > 0 && s <= shl64(rdx, 16) : shl64(rdx, 16) <= s && s < 0;
    }
    s = add64(shl64(sub32(p.y, A.y), 16), mul64(sdy, t));
    return rdy >= 0 ? s > 0 && s <= shl64(rdy, 16) : shl64(rdy, 16) <= s && s < 0;
}

/* bounding box of the ray start A against a segment or curve box, per ray direction */
static int box_hit(int32_t which, pt A, int32_t minx, int32_t miny, int32_t maxx, int32_t maxy, const char *what) {
    switch (which) {
    case 0: return miny <= A.y && maxy >= A.y && minx <= A.x;   /* ray towards -x */
    case 1: return miny <= A.y && maxy >= A.y && maxx >= A.x;   /* towards +x */
    case 2: return maxx >= A.x && miny <= A.y && minx <= A.x;   /* towards -y */
    case 3: return minx <= A.x && maxx >= A.x && maxy >= A.y;   /* towards +y */
    default: OutputDebugStringA(what); return 1;
    }
}

/* curve box: min/max of L, p, q; the x comparisons use the separate copy of L.x (stack
 * + 0x50 in the original, always equal to L.x) */
static void curve_box(int32_t lx, pt L, pt p, pt q, int32_t *minx, int32_t *miny, int32_t *maxx, int32_t *maxy) {
    int32_t m = L.x, cmp = lx;
    if (p.x < lx) { m = p.x; cmp = p.x; }
    *minx = q.x < cmp ? q.x : m;
    int32_t y = p.y < L.y ? p.y : L.y;
    *miny = q.y < y ? q.y : y;
    int32_t M = L.x; cmp = lx;
    if (p.x > lx) { M = p.x; cmp = p.x; }
    *maxx = q.x > cmp ? q.x : M;
    y = p.y > L.y ? p.y : L.y;
    *maxy = q.y > y ? q.y : y;
}

/* Misoriented: casts a ray from the extreme knot of contour c away from the outline (which:
 * 0 = min x, 1 = max x, 2 = min y, 3 = max y; coordinates doubled) and counts crossings with
 * every other contour. The contour is misoriented when the parity of the crossings disagrees
 * with its winding at the knot (2 = turning one way found before the scan ran out). */
TT_EXPORT uint32_t tt_Misoriented(int32_t c, int16_t which, uint16_t idx, pt knot, uint8_t *E) {
    const int32_t *X = E_OOX(E), *Y = E_OOY(E);
    const uint8_t *ON = E_ONCURVE(E);
    const int16_t *SP = E_SP(E), *EP = E_EP(E);
    int32_t sp = SP[c];
    uint16_t len = (uint16_t)((uint16_t)EP[c] - (uint16_t)sp);
    int16_t count16 = (int16_t)(uint16_t)(len + 1u);
    int32_t count32 = (int32_t)(int16_t)len + 1;
    pt A = {0, 0}, B = {0, 0};  /* the illegal case leaves them uninitialized in the original */
    switch (which) {
    case 0: A.x = add32(knot.x, 1); A.y = knot.y; B.x = -0x8000; B.y = sub32(knot.y, 1); break;
    case 1: A.x = sub32(knot.x, 1); A.y = knot.y; B.x = 0x7fff; B.y = sub32(knot.y, 1); break;
    case 2: A.x = knot.x; A.y = add32(knot.y, 1); B.x = sub32(knot.x, 1); B.y = -0x8000; break;
    case 3: A.x = knot.x; A.y = sub32(knot.y, 1); B.x = sub32(knot.x, 1); B.y = 0x7fff; break;
    default: OutputDebugStringA("fsglue.c, FindExtremaKnot, illegal case"); break;
    }

    /* winding at the knot: sign of the first nonzero turn after it (FindExtremaKnot) */
    uint16_t bp = (uint16_t)(idx - (uint16_t)sp);
    int32_t rel = (int16_t)bp;
    int32_t pi = (int16_t)((add32(rel, -1) + count32) % count32) + sp;
    pt before = {X[pi], Y[pi]};
    int32_t ki = sp + rel;
    pt cur = {X[ki], Y[ki]};
    int32_t ddx = sub32(cur.x, before.x), ddy = sub32(cur.y, before.y);
    int32_t dir = 0;
    if (0 < count16) {
        int16_t sign = 0, j = 0;
        int32_t r = bp;
        for (;;) {
            if (sign != 0) {
                if (j < count16 && sign > 0) dir = 2;  /* a turn found on the last step does not count */
                break;
            }
            r = (int16_t)r;
            int32_t ni = (r + 1) % count32 + sp;
            pt nx = {X[ni], Y[ni]};
            if (cur.x != nx.x || cur.y != nx.y) {
                int32_t dx = sub32(nx.x, cur.x), dy = sub32(nx.y, cur.y);
                int32_t cr = sub32(mul32(dy, ddx), mul32(dx, ddy));
                sign = cr < 0 ? -1 : cr > 0;
                cur = nx;
                ddx = dx;
                ddy = dy;
            }
            r = (r + 1) % (int32_t)count16;
            j++;
            if (!(j < count16)) break;
        }
    }

    /* crossings with the other contours, walked as lines and quadratic curves in doubled
     * units; L = start of the current curve (CheckBoundingBoxSegment/Curve) */
    int32_t trans = 0;
    int16_t nc = E_NC(E);
    int32_t lx = before.x;  /* the original starts L from the point before the knot, unused */
    pt L = before;
    for (int16_t k = 0; k < nc; k++) {
        if ((int32_t)k == c) continue;
        int64_t ksp = SP[k], kep = EP[k];
        if ((int16_t)(uint16_t)((uint16_t)kep - (uint16_t)ksp + 1u) <= 2) continue;
        uint32_t prev_on = ON[ksp];
        pt P = {add32(X[ksp], X[ksp]), add32(Y[ksp], Y[ksp])};
        if (prev_on == 0) {
            pt e = {add32(X[kep], X[kep]), add32(Y[kep], Y[kep])};
            lx = e.x;
            L = e;
            if (ON[kep] == 0) {
                L.x = add32(e.x, P.x) >> 1;
                L.y = add32(e.y, P.y) >> 1;
                lx = L.x;
            }
        }
        uint16_t si = (uint16_t)ksp;
        for (;;) {
            if (si == (uint16_t)kep) si = (uint16_t)ksp;
            else si++;
            int64_t n = (int16_t)si;
            pt C = {add32(X[n], X[n]), add32(Y[n], Y[n])};
            uint32_t con = ON[n];
            int32_t minx, miny, maxx, maxy;
            switch (prev_on * 2 | con) {
            case 0: {  /* off, off: curve L, P, midpoint */
                pt M = {add32(P.x, C.x) >> 1, add32(P.y, C.y) >> 1};
                if (lx != M.x || L.y != M.y) {
                    curve_box(lx, L, P, M, &minx, &miny, &maxx, &maxy);
                    if (box_hit(which, A, minx, miny, maxx, maxy, "fsglue.c, CheckBoundingBoxCurve, illegal case"))
                        trans += CurveTransitions(A, B, L, P, M);
                }
                L = M;
                lx = M.x;
                break;
            }
            case 1:  /* off, on: curve L, P, C */
                if (lx == C.x && L.y == C.y) break;
                curve_box(lx, L, P, C, &minx, &miny, &maxx, &maxy);
                if (box_hit(which, A, minx, miny, maxx, maxy, "fsglue.c, CheckBoundingBoxCurve, illegal case"))
                    trans += CurveTransitions(A, B, L, P, C);
                break;
            case 2:  /* on, off: a curve starts at P */
                L = P;
                lx = P.x;
                break;
            case 3:  /* on, on: line P, C */
                if (P.x == C.x && P.y == C.y) break;
                minx = C.x < P.x ? C.x : P.x;
                miny = C.y < P.y ? C.y : P.y;
                maxx = C.x > P.x ? C.x : P.x;
                maxy = C.y > P.y ? C.y : P.y;
                if (box_hit(which, A, minx, miny, maxx, maxy, "fsglue.c, CheckBoundingBoxSegment, illegal case"))
                    trans += segment_crossing(A, B, P, C);
                break;
            default: break;  /* onCurve bytes other than 0/1 */
            }
            P = C;
            prev_on = con;
            if (si == (uint16_t)ksp) break;
        }
    }
    return (uint32_t)((~trans & 1) * 2 == dir);
}

/* extreme knot on an off-curve point: the extremum of the quadratic through it, in doubled
 * units (prev/next off-curve points give midpoints). kind: 0 min x, 1 max x, 2 min y, 3 max y.
 * The extremal axis is divided down (min) or up (max), the other rounded away from zero. */
static pt curve_knot(int kind, const int32_t *X, const int32_t *Y, const uint8_t *ON, uint16_t k,
                     uint16_t sp, uint16_t ep, int64_t p) {
    int64_t next = (int16_t)(k == ep ? sp : (uint16_t)(k + 1));
    int64_t prev = (int16_t)(k == sp ? ep : (uint16_t)(k - 1));
    int32_t cx = X[p], cy = Y[p];
    pt p0, p2, r;
    if (ON[prev] & 1) { p0.x = add32(X[prev], X[prev]); p0.y = add32(Y[prev], Y[prev]); }
    else { p0.x = add32(X[prev], cx); p0.y = add32(Y[prev], cy); }
    if (ON[next] & 1) { p2.x = add32(X[next], X[next]); p2.y = add32(Y[next], Y[next]); }
    else { p2.x = add32(X[next], cx); p2.y = add32(Y[next], cy); }
    int32_t c2x = add32(cx, cx), c2y = add32(cy, cy), c4x = add32(c2x, c2x), c4y = add32(c2y, c2y);
    int ya = kind >= 2;
    int32_t d = ya ? add32(sub32(p0.y, c4y), p2.y) : add32(sub32(p0.x, c4x), p2.x);
    if (d == 0) { r.x = c2x; r.y = c2y; return r; }
    int64_t a = ya ? sub32(p0.y, c2y) : sub32(p0.x, c2x);
    int64_t b = ya ? sub32(p2.y, c2y) : sub32(p2.x, c2x);
    int64_t d2 = (int64_t)d * d;
    /* point at t = a / d: (p0 b^2 + 2 p1 a b + p2 a^2) / d^2, exact in 64-bit wraparound */
    int64_t nx = add64(add64(mul64(mul64(p0.x, b), b), mul64(mul64(c4x, a), b)), mul64(mul64(p2.x, a), a));
    int64_t ny = add64(add64(mul64(mul64(p0.y, b), b), mul64(mul64(c4y, a), b)), mul64(mul64(p2.y, a), a));
    int64_t on = ya ? nx : ny, ex = ya ? ny : nx;
    int32_t ext = (kind & 1) ? (int32_t)(add64(d2 - 1, ex) / d2) : (int32_t)(ex / d2);
    int32_t oth = on < 0 ? neg32((int32_t)(sub64(d2, on) / d2)) : (int32_t)(add64(on, d2) / d2);
    if (ya) { r.x = oth; r.y = ext; } else { r.x = ext; r.y = oth; }
    return r;
}

/* fsg_CheckOutlineOrientation: for each contour of more than 2 points, find its extreme
 * points (min/max x/y in font units, ties broken towards on-curve points), and test the
 * extreme knot (and, on disagreement, a third one) with Misoriented. Sets zone + 0x58 bit 0. */
TT_EXPORT void tt_fsg_CheckOutlineOrientation(uint8_t *E) {
    const int32_t *X = E_OOX(E), *Y = E_OOY(E);
    const uint8_t *ON = E_ONCURVE(E);
    const int16_t *SP = E_SP(E), *EP = E_EP(E);
    pt knot[4] = {{0, 0}, {0, 0}, {0, 0}, {0, 0}};  /* stack, not reset per contour in the original */
    int16_t nc = E_NC(E);
    for (int32_t c = 0; c < nc; c++) {
        E_FC(E)[c] = 0;
        int32_t sp = SP[c];
        uint16_t ep = (uint16_t)EP[c];
        int16_t count = (int16_t)(uint16_t)(ep - (uint16_t)sp + 1u);
        if (count <= 2) continue;
        int32_t minx = INT32_MAX, maxx = INT32_MIN, miny = INT32_MAX, maxy = INT32_MIN;
        int16_t ix[4] = {-1, -1, -1, -1};
        uint16_t k = (uint16_t)sp;
        int64_t p = sp;  /* the original's running point pointer; equals ci barring 16-bit wrap */
        do {
            int64_t ci = (int16_t)(uint16_t)(k - (uint16_t)sp) + sp;
            int32_t x = X[ci], y;
            int set;
            set = x < minx || (x == minx && (!ON[ix[0]] || (ON[ci] && Y[ci] < Y[ix[0]])));
            if (set) {
                ix[0] = (int16_t)k;
                minx = X[p];
                if (ON[p]) { knot[0].x = add32(minx, minx); knot[0].y = add32(Y[p], Y[p]); }
                else knot[0] = curve_knot(0, X, Y, ON, k, (uint16_t)sp, ep, p);
            }
            set = x > maxx || (x == maxx && (!ON[ix[1]] || (ON[ci] && Y[ci] > Y[ix[1]])));
            if (set) {
                ix[1] = (int16_t)k;
                maxx = X[p];
                if (ON[p]) { knot[1].x = add32(maxx, maxx); knot[1].y = add32(Y[p], Y[p]); }
                else knot[1] = curve_knot(1, X, Y, ON, k, (uint16_t)sp, ep, p);
            }
            y = Y[ci];
            set = y < miny || (y == miny && (!ON[ix[2]] || (ON[ci] && x > X[ix[2]])));
            if (set) {
                ix[2] = (int16_t)k;
                miny = Y[p];
                if (ON[p]) { knot[2].x = add32(X[p], X[p]); knot[2].y = add32(miny, miny); }
                else knot[2] = curve_knot(2, X, Y, ON, k, (uint16_t)sp, ep, p);
            }
            set = y > maxy || (y == maxy && (!ON[ix[3]] || (ON[ci] && x < X[ix[3]])));
            if (set) {
                ix[3] = (int16_t)k;
                maxy = Y[p];
                if (ON[p]) { knot[3].x = add32(X[p], X[p]); knot[3].y = add32(maxy, maxy); }
                else knot[3] = curve_knot(3, X, Y, ON, k, (uint16_t)sp, ep, p);
            }
            k++;
            p++;
        } while ((int16_t)(uint16_t)(k - (uint16_t)sp) < count);

        /* second knot: the farther (Manhattan, from the min x knot) of min y / max y; the third
         * decides when the two disagree */
        int32_t x0 = X[ix[0]], y0 = Y[ix[0]];
        int32_t d1 = add32(abs32(sub32(X[ix[1]], x0)), abs32(sub32(Y[ix[1]], y0)));
        int32_t d2 = add32(abs32(sub32(X[ix[2]], x0)), abs32(sub32(Y[ix[2]], y0)));
        int32_t d3 = add32(abs32(sub32(X[ix[3]], x0)), abs32(sub32(Y[ix[3]], y0)));
        int16_t second, third;
        if (d2 > d3) { second = 2; third = d3 <= d1 ? 1 : 3; }
        else { second = 3; third = d2 > d1 ? 2 : 1; }
        uint8_t mis = (uint8_t)Misoriented(c, 0, (uint16_t)ix[0], knot[0], E);
        if (mis != (uint8_t)Misoriented(c, second, (uint16_t)ix[second], knot[second], E))
            mis = (uint8_t)Misoriented(c, third, (uint16_t)ix[third], knot[third], E);
        if (mis) E_FC(E)[c] |= 1;
    }
}


/* ======================================================================================
 * Emboldening
 * ====================================================================================== */

/* Intersect26Dot6: intersection of line p1-p2 with line p3-p4; nearly parallel lines give
 * the midpoint of p2 and p3 */
TT_EXPORT void tt_Intersect26Dot6(pt p1, pt p2, pt p3, pt p4, int32_t *out) {
    int32_t dxa = sub32(p2.x, p1.x), dya = sub32(p2.y, p1.y);
    int32_t dxb = sub32(p4.x, p3.x), dyb = sub32(p4.y, p3.y);
    int32_t u, den;
    if (dya == 0) {
        if (dxb == 0) { out[0] = p3.x; out[1] = p1.y; return; }
        u = sub32(p3.y, p1.y);
        den = neg32(dyb);
    } else if (dxa == 0) {
        if (dyb == 0) { out[0] = p1.x; out[1] = p3.y; return; }
        u = sub32(p3.x, p1.x);
        den = neg32(dxb);
    } else if (abs32(dxa) < abs32(dya)) {
        u = add32(sub32(CompDiv(dya, (int64_t)sub32(p3.y, p1.y) * dxa), p3.x), p1.x);
        den = sub32(dxb, CompDiv(dya, (int64_t)dyb * dxa));
    } else {
        u = sub32(sub32(p3.y, p1.y), CompDiv(dxa, (int64_t)sub32(p3.x, p1.x) * dya));
        den = sub32(CompDiv(dxa, (int64_t)dxb * dya), dyb);
    }
    if (abs32(den) <= 0x10) {
        out[0] = add32(p3.x, p2.x) >> 1;
        out[1] = add32(p2.y, p3.y) >> 1;
        return;
    }
    out[0] = add32(CompDiv(den, (int64_t)dxb * u), p3.x);
    out[1] = add32(CompDiv(den, (int64_t)dyb * u), p3.y);
}

/* EmboldPoint: moves point i (and the equal points up to j) outwards: both adjacent edges
 * prev-cur and cur-next are offset along their normals (axis-aligned when axis is set) and
 * the point goes to their intersection, limited to the offsets; then everything shifts by
 * (left, down). rev = contour misoriented. dx_pos/dx_neg/dy_pos/dy_neg are the offsets per
 * direction, 26.6. */
TT_EXPORT void tt_EmboldPoint(uint32_t i, uint32_t j, uint8_t axis, uint8_t rev, pt prev, pt cur, pt next,
                              int32_t dx_pos, int32_t dx_neg, int32_t dy_pos, int32_t dy_neg,
                              int32_t ymin, uint8_t clamp_y, uint8_t *E) {
    int32_t *X = E_X(E), *Y = E_Y(E);
    int32_t ex = sub32(next.x, cur.x), ey = sub32(next.y, cur.y);
    int32_t px = sub32(cur.x, prev.x), py = sub32(cur.y, prev.y);
    int32_t n2x = rev ? neg32(ex) : ex;  /* second argument of the second normal */
    int32_t n2y = rev ? ey : neg32(ey);
    int32_t n1x = rev ? neg32(px) : px;
    int32_t n1y = rev ? py : neg32(py);
    pt a = cur;  /* end of the first offset edge; cur becomes the start of the second */
    if (axis) {
        if (n1y > 0) a.x = add32(cur.x, dx_pos);
        if (n2y > 0) cur.x = add32(cur.x, dx_pos);
    } else {
        int16_t v[2] = {0, 0};
        Normalize(n1y, n1x, v);
        int32_t v1x = v[0] >> 8, v1y = v[1] >> 8;
        v[0] = v[1] = 0;
        Normalize(n2y, n2x, v);
        int32_t v2x = v[0] >> 8, v2y = v[1] >> 8;
        int32_t m1x = Mul26Dot6(v1x, v1x <= 0 ? dx_neg : dx_pos);
        int32_t m1y = Mul26Dot6(v1y, v1y >= 0 ? dy_pos : dy_neg);
        prev.y = add32(prev.y, m1y);
        a.x = add32(m1x, cur.x);
        a.y = add32(cur.y, m1y);
        prev.x = add32(prev.x, m1x);
        int32_t m2x = Mul26Dot6(v2x, v2x <= 0 ? dx_neg : dx_pos);
        int32_t m2y = Mul26Dot6(v2y, v2y >= 0 ? dy_pos : dy_neg);
        next.y = add32(next.y, m2y);
        cur.x = add32(cur.x, m2x);
        next.x = add32(next.x, m2x);
        cur.y = add32(cur.y, m2y);
    }
    if (a.x == cur.x && a.y == cur.y) {
        X[i] = cur.x;
        Y[i] = cur.y;
    } else {
        int32_t q[2] = {0, 0};
        Intersect26Dot6(prev, a, cur, next, q);
        int32_t ox = X[i], oy = Y[i], dx = sub32(q[0], ox), dy = sub32(q[1], oy);
        int32_t nx = q[0], ny = q[1];
        if (dx > dx_pos) nx = add32(ox, dx_pos);
        if (dx < neg32(dx_neg)) nx = sub32(ox, dx_neg);
        if (dy < neg32(dy_neg)) ny = sub32(oy, dy_neg);
        if (dy > dy_pos) ny = add32(oy, dy_neg);  /* the original adds dy_neg here, not dy_pos */
        X[i] = nx;
        Y[i] = ny;
    }
    X[i] = add32(X[i], dx_neg);
    Y[i] = add32(Y[i], dy_neg);
    if (clamp_y && Y[i] < ymin) Y[i] = ymin;
    for (uint32_t k = i + 1; i != j && k <= j; k++) {
        X[k] = X[i];
        Y[k] = Y[i];
    }
}

/* fsg_Embold: bold simulation on the scaled outline (zone 1 of key + 0x1d8). gs + 0x1b4 and
 * + 0x1b6 are the emboldening amounts in pixels; half splits them between both sides,
 * otherwise each side gets amount * 32 (26.6, half a pixel per unit). Advance phantom points
 * move by one pixel when not degenerate. skip = only the phantom points. */
TT_EXPORT void tt_fsg_Embold(uint8_t *key, uint8_t *gs, uint8_t half, uint8_t skip, uint8_t le1) {
    uint8_t *E = AT(key, 0x10, uint8_t *);
    uint16_t ay = AT(gs, 0x1b6, uint16_t), ax = AT(gs, 0x1b4, uint16_t);
    uint8_t axis = le1 ? ay <= 1 : ay == 1;
    int32_t *X = E_X(E), *Y = E_Y(E);
    int16_t nc = E_NC(E);
    uint16_t last = (uint16_t)E_EP(E)[nc - 1];
    if (X[(uint16_t)(last + 2)] != X[(uint16_t)(last + 1)]) X[(uint16_t)(last + 2)] += 0x40;
    last = (uint16_t)E_EP(E)[nc - 1];
    if (Y[(uint16_t)(last + 4)] != Y[(uint16_t)(last + 3)]) Y[(uint16_t)(last + 4)] -= 0x40;
    if (skip) return;
    int32_t p8, p9, p10, p11;  /* EmboldPoint's dx_pos, dx_neg, dy_pos, dy_neg */
    if (half) {
        uint32_t hx = (uint32_t)ax >> 1, hy = (uint32_t)ay >> 1;
        p11 = (int32_t)((ax - hx) << 6);
        p9 = (int32_t)(hy << 6);
        p10 = (int32_t)(hx << 6);
        p8 = (int32_t)((ay - hy) << 6);
    } else {
        p9 = p8 = (int32_t)((uint32_t)ay << 5);
        p10 = p11 = (int32_t)((uint32_t)ax << 5);
    }
    for (int32_t c = 0; (int16_t)c < nc; c++) {
        uint32_t sp = (uint32_t)(int32_t)E_SP(E)[c], ep = (uint32_t)(int32_t)E_EP(E)[c];
        if (ep - sp < 2) continue;
        uint8_t rev = E_FC(E)[c] & 1;
        pt first = {X[sp], Y[sp]}, prev = {X[ep], Y[ep]}, cur = first, next = {X[sp + 1], Y[sp + 1]};
        if (sp > ep) continue;
        uint32_t i = sp;
        for (;;) {
            uint32_t j = i;
            if (next.x == cur.x) {  /* run of points equal to cur: one offset for all */
                while (j < ep && next.y == cur.y) {
                    j++;
                    if (j < ep) { next.x = X[j + 1]; next.y = Y[j + 1]; }
                    else next = first;
                    if (next.x != cur.x) break;
                }
            }
            EmboldPoint(i, j, axis, rev, prev, cur, next, p8, p9, p10, p11,
                           AT(gs, 0x1b8, int32_t), AT(gs, 0x1bc, uint8_t), E);
            prev = cur;
            cur = next;
            i = j + 1;
            if (i < ep) { next.x = X[i + 1]; next.y = Y[i + 1]; continue; }
            next = first;
            if (i > ep) break;
        }
    }
}
