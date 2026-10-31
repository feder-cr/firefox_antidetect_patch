/*
 * Scan conversion of quadratic splines, ported from DWrite.dll 10.0.26100.9711:
 *   EvaluateSpline      splits a contour's quadratic at its x and y extrema, subdivides it
 *                       until it spans at most 0xc80 (50 px) in x and y, keeps the
 *                       contour's previous/current point state and hands each piece to
 *                       fsc_CalcSpline (or fsc_CalcLine when it is straight)
 *   fsc_CalcSpline      walks a monotonic quadratic across the scanline centres with an
 *                       incremental implicit-curve decision function, calling the
 *                       horizontal (StateVars +0xb0) and vertical (+0xb8) crossing callbacks
 *   Calc*SpSubpix       exact crossing of a quadratic with one scanline centre (bisection),
 *   Calc*EpSubpix       the same at an end point; reached through StateVars +0xc8, +0xe0,
 *                       +0xd0, +0xe8, stored by fsc_FillGlyph
 *
 * StateVars (sv) fields used here, by offset:
 *   0x00/0x04 previous point x/y (0x7fffffff: none yet), 0x08/0x0c current point x/y,
 *   0x10/0x14 first point after the contour start, 0x30..0x50 and 0x70..0x90 per-quadrant
 *   callback sets copied to 0x58..0x68 and 0x98..0xa8, 0xb0/0xb8 crossing callbacks,
 *   0xf8/0x100 x/y point arrays, 0x108 their capacity, 0x10c their count, 0x110 a word.
 *
 * All arithmetic is 32-bit two's complement as in the original (MSVC wraps).
 */
#include "tt_state.h"
#include "orig.h"

typedef int (*scan_cb)(void *sv, int32_t a, int32_t b);

int32_t tt_CompDiv(int32_t d, int64_t n); /* reimpl/truetype/interp.c */

/* callees outside this group, called in the original DLL */
#define fsc_CalcLine       ORIG(int (*)(void *, int32_t, int32_t, int32_t, int32_t, uint32_t), 0x6f640)
#define CheckHorizTopology ORIG(int (*)(void *, int32_t, int32_t, uint32_t), 0x6dc10)
#define AddVertOn          ORIG(int (*)(void *, uint32_t), 0x6dae8)
#define AddVertOff         ORIG(int (*)(void *, uint32_t), 0xce508)
/* EvaluateSpline reaches fsc_CalcSpline through the original's entry, so that each of the two
 * can be swapped alone (oracle.py --swap); with both swapped it lands in tt_fsc_CalcSpline. */
#define fsc_CalcSpline     ORIG(int (*)(void *, int32_t, int32_t, int32_t, int32_t, int32_t, int32_t, uint32_t), 0x95040)

static inline int32_t add(int32_t a, int32_t b) { return (int32_t)((uint32_t)a + (uint32_t)b); }
static inline int32_t sub(int32_t a, int32_t b) { return (int32_t)((uint32_t)a - (uint32_t)b); }
static inline int32_t mul(int32_t a, int32_t b) { return (int32_t)((uint32_t)a * (uint32_t)b); }
static inline int32_t shl(int32_t a, int n) { return (int32_t)((uint32_t)a << (n & 31)); }
static inline int32_t sar(int32_t a, int n) { return a >> (n & 31); }  /* SAR, count masked */
/* NEG + CMOVcc: abs that leaves INT32_MIN as it is */
static inline int32_t abs32(int32_t v) { int32_t n = sub(0, v); return n < 0 ? v : n; }

/* ======================================================================================= */

/* EvaluateSpline (0x18006f0f0). Flags: bit 1 = no vertical (dropout) scan, bit 2 = record
 * the points. The original recurses on the first piece of each split and loops on the
 * second; this keeps that order, since the point state in sv is order dependent. */
TT_EXPORT int tt_EvaluateSpline(void *sv, int32_t x0, int32_t y0, int32_t x1, int32_t y1,
                                int32_t x2, int32_t y2, uint32_t flags_arg) {
    uint16_t flags = (uint16_t)flags_arg;  /* passed as a word on the stack */
    int32_t *s = (int32_t *)sv;
    int r;
    for (;;) {
        int32_t dx01 = sub(x1, x0), dx12 = sub(x2, x1);
        int32_t dy01 = sub(y1, y0), dy12 = sub(y2, y1);
        if ((dy01 > 0 && dy12 < 0) || (dy01 < 0 && dy12 > 0)) {
            /* y extremum at t = dy01 / (dy01 - dy12): split there */
            int32_t den = sub(dy01, dy12);
            int64_t m = dy01;
            int32_t xa = add(x0, tt_CompDiv(den, (int64_t)dx01 * m));
            int32_t xb = add(x1, tt_CompDiv(den, (int64_t)dx12 * m));
            int32_t xm = add(xa, tt_CompDiv(den, (int64_t)sub(xb, xa) * m));
            int32_t ym = add(y0, tt_CompDiv(den, m * m));
            if ((r = tt_EvaluateSpline(sv, x0, y0, xa, ym, xm, ym, flags)) != 0) return r;
            x0 = xm; y0 = ym; x1 = xb; y1 = ym;
            continue;
        }
        if ((dx01 > 0 && dx12 < 0) || (dx01 < 0 && dx12 > 0)) {
            /* x extremum */
            int32_t den = sub(dx01, dx12);
            int64_t m = dx01;
            int32_t ya = add(y0, tt_CompDiv(den, (int64_t)dy01 * m));
            int32_t yb = add(y1, tt_CompDiv(den, (int64_t)dy12 * m));
            int32_t ym = add(ya, tt_CompDiv(den, (int64_t)sub(yb, ya) * m));
            int32_t xm = add(x0, tt_CompDiv(den, m * m));
            if ((r = tt_EvaluateSpline(sv, x0, y0, xm, ya, xm, ym, flags)) != 0) return r;
            x0 = xm; y0 = ym; x1 = xm; y1 = yb;
            continue;
        }
        if (abs32(sub(x2, x0)) > 0xc80 || abs32(sub(y2, y0)) > 0xc80) {
            /* too long for fsc_CalcSpline's 32-bit decision function: halve (de Casteljau) */
            int32_t ya = sar(add(y1, y0), 1), yb = sar(add(y1, y2), 1);
            int32_t xa = sar(add(x0, x1), 1), xb = sar(add(x1, x2), 1);
            int32_t ym = sar(add(yb, ya), 1), xm = sar(add(xb, xa), 1);
            if ((r = tt_EvaluateSpline(sv, x0, y0, xa, ya, xm, ym, flags)) != 0) return r;
            x0 = xm; y0 = ym; x1 = xb; y1 = yb;
            continue;
        }
        break;
    }

    /* the current point lies on a horizontal scanline centre: topology check */
    if ((s[3] & 0x3f) == 0x20) {
        if (s[2] == x2 && s[3] == y2) goto calc;
        if (s[0] == 0x7fffffff) { s[4] = x2; s[5] = y2; }
        else if ((r = CheckHorizTopology(sv, x2, y2, flags)) != 0) return r;
    }
    /* ... on a vertical one (only when scanning vertically too) */
    if (!(flags & 2) && (s[2] & 0x3f) == 0x20) {
        int32_t cx = s[2];
        if (cx == x2 && s[3] == y2) goto calc;
        int32_t px = s[0];
        if (px == 0x7fffffff) { s[4] = x2; s[5] = y2; }
        else {
            r = 0;
            if (x2 < cx) {
                if (cx < px) r = AddVertOn(sv, flags);
                else if (cx > px) { if ((r = AddVertOn(sv, flags)) == 0) r = AddVertOff(sv, flags); }
                else if (s[3] < s[1]) r = AddVertOn(sv, flags);
            } else if (x2 == cx) {
                int32_t cy = s[3];
                if (cx < px) { if (y2 > cy) r = AddVertOn(sv, flags); }
                else if (cx > px) { if (y2 < cy) r = AddVertOff(sv, flags); }
                else if (cy > s[1]) { if (y2 < cy) r = AddVertOff(sv, flags); }
                else if (cy < s[1]) { if (y2 > cy) r = AddVertOn(sv, flags); }
            } else {
                if (cx < px) { if ((r = AddVertOn(sv, flags)) == 0) r = AddVertOff(sv, flags); }
                else if (cx > px) r = AddVertOff(sv, flags);
                else if (s[3] > s[1]) r = AddVertOff(sv, flags);
            }
            if (r) return r;
        }
    }
    s[0] = s[2]; s[1] = s[3];
    s[3] = y2; s[2] = x2;
calc:
    /* collinear control point (32-bit cross product): a line */
    if (mul(sub(y2, y1), sub(x1, x0)) == mul(sub(y1, y0), sub(x2, x1)))
        return fsc_CalcLine(sv, x0, y0, x2, y2, flags);
    return fsc_CalcSpline(sv, x0, y0, x1, y1, x2, y2, flags);
}

/* ======================================================================================= */

static const int32_t bits4[16] = {0, 1, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 4, 4, 4, 4};  /* 0x1801d7f20 */
/* precision shift by bits(max delta) + bits(cross), 0x1801e0ca0 */
static const int32_t prec_shift[38] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                                       0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 2, 2, 2, 3, 3};

/* bit length via the nibble table, as the original's compare tree */
static int32_t bitlen(uint32_t u) {
    if (u < 0x10000) {
        if (u < 0x100) return u < 0x10 ? bits4[u] : bits4[u >> 4] + 4;
        return u < 0x1000 ? bits4[u >> 8] + 8 : bits4[u >> 12] + 12;
    }
    if (u < 0x1000000) return u < 0x100000 ? bits4[u >> 16] + 16 : bits4[u >> 20] + 20;
    return u < 0x10000000 ? bits4[u >> 24] + 24 : bits4[u >> 28] + 28;
}

/* fsc_CalcSpline (0x180095040): one monotonic quadratic (x0,y0) (x1,y1) (x2,y2), 26.6.
 * Scanline index i has its centre at i*64 + 32. In the curve's own quadrant (deltas made
 * positive) it steps from centre to centre in x or y, choosing by the sign of the implicit
 * function F and the second differences; a y step reports a horizontal crossing
 * (cb_h(sv, x index + xflag, y index)), an x step a vertical one (cb_v(sv, x index, y index +
 * yflag)). Returns 0x1306 when the curve is too large for 32-bit arithmetic, 0x1305/0x1a00
 * for the point record overflowing. */
TT_EXPORT int tt_fsc_CalcSpline(void *sv, int32_t x0, int32_t y0, int32_t x1, int32_t y1,
                                int32_t x2, int32_t y2, uint32_t flags_arg) {
    uint8_t *p = (uint8_t *)sv;
    uint16_t flags = (uint16_t)flags_arg;
    int32_t yi, yend, ystep, yflag, yf, dy01, dy02;
    int32_t xi, xend, xstep, xflag, xf, dx01, dx02, bias;
    int dir, r;

    if (y2 > y0) {
        int32_t c = add(add(y0, 0x20) & ~0x3f, 0x20);  /* first centre above y0 */
        yf = sub(c, y0); yi = sar(c, 6); yend = sar(sub(y2, 0x21), 6) + 1;
        dy01 = sub(y1, y0); dy02 = sub(y2, y0); ystep = 1; yflag = 0; dir = 1;
    } else {
        int32_t c = add(sub(y0, 0x21) & ~0x3f, 0x20);  /* first centre below y0 */
        yf = sub(y0, c); yi = sar(c, 6); yend = sar(add(y2, 0x20), 6) - 1;
        dy01 = sub(y0, y1); dy02 = sub(y0, y2); ystep = -1; yflag = 1; dir = 4;
    }
    if (x2 > x0) {
        int32_t c = add(add(x0, 0x20) & ~0x3f, 0x20);
        xf = sub(c, x0); xi = sar(c, 6); xend = sar(sub(x2, 0x21), 6) + 1;
        dx01 = sub(x1, x0); dx02 = sub(x2, x0); xstep = 1; xflag = 0; bias = yflag;
    } else {
        int32_t c = add(sub(x0, 0x21) & ~0x3f, 0x20);
        xf = sub(x0, c); xi = sar(c, 6); xend = sar(add(x2, 0x20), 6) - 1;
        dx01 = sub(x0, x1); dx02 = sub(x0, x2); xstep = -1; xflag = 1; bias = 1 - yflag;
        dir = (dir != 1) + 2;
    }
    /* dir: quadrant 1 (+x+y), 2 (-x+y), 3 (-x-y), 4 (+x-y) */
    scan_cb cb_h = AT(p, 0xb0, scan_cb), cb_v = AT(p, 0xb8, scan_cb);
    {
        uint64_t a, b, c;
        if ((unsigned)(dir - 1) <= 1) { a = AT(p, 0x30, uint64_t); b = AT(p, 0x40, uint64_t); c = AT(p, 0x38, uint64_t); }
        else                          { a = AT(p, 0x38, uint64_t); b = AT(p, 0x48, uint64_t); c = AT(p, 0x50, uint64_t); }
        AT(p, 0x58, uint64_t) = a; AT(p, 0x60, uint64_t) = b; AT(p, 0x68, uint64_t) = c;
    }
    if (flags & 2) {
        if (yi == yend) return 0;
        if (xi == xend) {
            for (xi += xflag; yi != yend; yi += ystep)
                if ((r = cb_h(sv, xi, yi)) != 0) return r;
            return 0;
        }
    } else {
        uint64_t a, b, c;
        if ((unsigned)(dir - 2) <= 1) { a = AT(p, 0x70, uint64_t); b = AT(p, 0x80, uint64_t); c = AT(p, 0x78, uint64_t); }
        else                          { a = AT(p, 0x78, uint64_t); b = AT(p, 0x88, uint64_t); c = AT(p, 0x90, uint64_t); }
        AT(p, 0x98, uint64_t) = a; AT(p, 0xa0, uint64_t) = b; AT(p, 0xa8, uint64_t) = c;
        if (flags & 4) {
            /* record the control and end points */
            int32_t n = AT(p, 0x10c, int32_t);
            if (sub(n, 1) > 0x3fff) return 0x1305;
            AT(p, 0x110, uint16_t) = (uint16_t)((uint16_t)(sub(n, 1) << 2) | 1);
            const int32_t px[2] = {x1, x2}, py[2] = {y1, y2};
            for (int k = 0; k < 2; k++) {
                AT(p, 0xf8, int32_t *)[AT(p, 0x10c, int32_t)] = px[k];
                AT(p, 0x100, int32_t *)[AT(p, 0x10c, int32_t)] = py[k];
                if (++AT(p, 0x10c, int32_t) > AT(p, 0x108, int32_t)) return 0x1a00;
            }
        }
        if (xi == xend) {
            for (xi += xflag; yi != yend; yi += ystep)
                if ((r = cb_h(sv, xi, yi)) != 0) return r;
            return 0;
        }
        if (yi == yend) {
            for (yi += yflag; xi != xend; xi += xstep)
                if ((r = cb_v(sv, xi, yi)) != 0) return r;
            return 0;
        }
    }

    /* twice the signed area of the control triangle */
    int32_t cross = shl(sub(mul(dy02, dx01), mul(dx02, dy01)), 1);
    int32_t bc = bitlen((uint32_t)abs32(cross));
    int32_t bm = bitlen((uint32_t)abs32(dx02 <= dy02 ? dy02 : dx02));
    if (bm > 12 || bc > 25) return 0x1306;
    int32_t t = prec_shift[bm + bc];
    int32_t sh = 6 - t;
    if (t > 0) {
        /* drop t bits of precision, rounding */
        int32_t half = shl(1, t - 1);
        xf = sar(add(xf, half), t);
        dx02 = sar(add(dx02, half), t);
        dy02 = sar(add(dy02, half), t);
        dx01 = sar(add(dx01, half), t);
        dy01 = sar(add(dy01, half), t);
        yf = sar(add(yf, half), t);
        cross = shl(sub(mul(dy02, dx01), mul(dx02, dy01)), 1);
    }
    if (abs32(mul(cross, dx02)) >= 0x23000000) return 0x1306;
    if (abs32(mul(cross, dy02)) >= 0x23000000) return 0x1306;

    int32_t ax = sub(dx02, shl(dx01, 1)), ay = sub(dy02, add(dy01, dy01));
    int32_t Dy = mul(cross, dy01), Dx = mul(cross, dx01);
    int32_t A = mul(ay, ay), B = mul(ax, ax), C = sub(0, mul(ay, ax));
    int32_t one = shl(1, sh);
    int32_t F, Gx, Gy, Cs, As, Bs, Fa, Fb;
    if (bm > 7) {
        int32_t h1 = sar(one, 1), Cy = mul(C, yf), Cx = mul(C, xf);
        As = shl(A, sh - 1);
        Bs = shl(B, sh - 1);
        Cs = shl(C, sh);
        Gx = add(add(mul(add(xf, h1), A), Cy), Dy);
        Gy = sub(add(mul(add(h1, yf), B), Cx), Dx);
        Fa = sar(add(add(mul(sar(A, 1), xf), Cy), Dy), sh);
        Fb = sar(sub(mul(sar(B, 1), yf), Dx), sh);
    } else {
        int32_t C2 = add(C, C), C2y = mul(C2, yf), nDx2 = add(sub(0, Dx), sub(0, Dx)), Dy2 = add(Dy, Dy);
        Gx = shl(add(add(mul(add(one, shl(xf, 1)), A), Dy2), C2y), sh);
        Gy = shl(add(mul(C2, xf), add(mul(add(one, shl(yf, 1)), B), nDx2)), sh);
        As = shl(A, 2 * sh);
        Bs = shl(B, 2 * sh);
        Cs = shl(C2, 2 * sh);
        Fa = add(add(mul(A, xf), Dy2), C2y);
        Fb = add(mul(B, yf), nDx2);
    }
    F = add(add(mul(Fa, xf), mul(Fb, yf)), bias);
    int32_t A2 = add(As, As), B2 = add(Bs, Bs);

    if (!(flags & 2)) {
        if (cross > 0) {
            for (;;) {
                if (xi == xend) goto finish_h;
                if (yi == yend) goto finish_v;
                if (F < 0 || Gy > Bs) {
                    if ((r = cb_v(sv, xi, add(yflag, yi))) != 0) return r;
                    xi = add(xi, xstep); F = add(F, Gx); Gx = add(Gx, A2); Gy = add(Gy, Cs);
                } else {
                    if ((r = cb_h(sv, add(xflag, xi), yi)) != 0) return r;
                    yi = add(yi, ystep); F = add(F, Gy); Gx = add(Gx, Cs); Gy = add(Gy, B2);
                }
            }
        } else {
            for (;;) {
                if (xi == xend) goto finish_h;
                if (yi == yend) goto finish_v;
                if (F < 0 || Gx > As) {
                    if ((r = cb_h(sv, add(xflag, xi), yi)) != 0) return r;
                    yi = add(yi, ystep); F = add(F, Gy); Gx = add(Gx, Cs); Gy = add(Gy, B2);
                } else {
                    if ((r = cb_v(sv, xi, add(yflag, yi))) != 0) return r;
                    xi = add(xi, xstep); F = add(F, Gx); Gx = add(Gx, A2); Gy = add(Gy, Cs);
                }
            }
        }
    finish_v:
        for (; xi != xend; xi = add(xi, xstep))
            if ((r = cb_v(sv, xi, add(yflag, yi))) != 0) return r;
    finish_h:
        for (; yi != yend; yi = add(yi, ystep))
            if ((r = cb_h(sv, add(xflag, xi), yi)) != 0) return r;
        return 0;
    }

    /* horizontal crossings only: x steps just move the decision function */
    xi = add(xi, xflag); xend = add(xend, xflag);
    if (cross > 0) {
        for (;;) {
            if (xi == xend) break;
            if (yi == yend) return 0;
            if (F < 0 || Gy > Bs) {
                xi = add(xi, xstep); F = add(F, Gx); Gx = add(Gx, A2); Gy = add(Gy, Cs);
            } else {
                if ((r = cb_h(sv, xi, yi)) != 0) return r;
                yi = add(yi, ystep); F = add(F, Gy); Gx = add(Gx, Cs); Gy = add(Gy, B2);
            }
        }
    } else {
        for (;;) {
            if (xi == xend) break;
            if (yi == yend) return 0;
            if (F < 0 || Gx > As) {
                if ((r = cb_h(sv, xi, yi)) != 0) return r;
                yi = add(yi, ystep); F = add(F, Gy); Gx = add(Gx, Cs); Gy = add(Gy, B2);
            } else {
                xi = add(xi, xstep); F = add(F, Gx); Gx = add(Gx, A2); Gy = add(Gy, Cs);
            }
        }
    }
    for (; yi != yend; yi = add(yi, ystep))
        if ((r = cb_h(sv, xi, yi)) != 0) return r;
    return 0;
}

/* ======================================================================================= */

/* CalcHorizSpSubpix (0x1800ddd40): x where the quadratic px/py (3 points) crosses the
 * horizontal scanline centre y = ys*64 + 32, by bisection on rounded midpoints until the
 * midpoint's y is exactly the centre (the original loops forever if it never is). */
TT_EXPORT int32_t tt_CalcHorizSpSubpix(int32_t ys, const int32_t *px, const int32_t *py) {
    int32_t c = add(shl(ys, 6), 0x20);
    int32_t x1 = px[1], y1 = py[1], xl, yl, xh, yh;
    if (py[0] < py[2]) { xl = px[0]; yl = py[0]; xh = px[2]; yh = py[2]; }
    else               { xl = px[2]; yl = py[2]; xh = px[0]; yh = py[0]; }
    for (;;) {
        int32_t xm = sar(add(add(add(shl(x1, 1), 1), xh), xl), 2);
        int32_t ym = sar(add(add(yl, 1), add(yh, shl(y1, 1))), 2);
        if (ym > c)      { x1 = sar(add(x1, xl), 1); y1 = sar(add(y1, yl), 1); xh = xm; yh = ym; }
        else if (ym < c) { x1 = sar(add(x1, xh), 1); y1 = sar(add(y1, yh), 1); xl = xm; yl = ym; }
        else return xm;
    }
}

/* CalcVertSpSubpix (0x1800d91c0): y where the quadratic crosses x = xs*64 + 32 */
TT_EXPORT int32_t tt_CalcVertSpSubpix(int32_t xs, const int32_t *px, const int32_t *py) {
    int32_t c = add(shl(xs, 6), 0x20);
    int32_t x1 = px[1], y1 = py[1], xl, yl, xh, yh;
    if (px[0] < px[2]) { xl = px[0]; yl = py[0]; xh = px[2]; yh = py[2]; }
    else               { xl = px[2]; yl = py[2]; xh = px[0]; yh = py[0]; }
    for (;;) {
        int32_t xm = sar(add(add(add(shl(x1, 1), 1), xh), xl), 2);
        int32_t ym = sar(add(add(add(shl(y1, 1), 1), yh), yl), 2);
        if (xm > c)      { x1 = sar(add(x1, xl), 1); y1 = sar(add(y1, yl), 1); xh = xm; yh = ym; }
        else if (xm < c) { x1 = sar(add(x1, xh), 1); y1 = sar(add(y1, yh), 1); xl = xm; yl = ym; }
        else return ym;
    }
}

/* CalcHorizEpSubpix (0x1800efcf0), CalcVertEpSubpix (0x1800ef400): the crossing is the
 * spline's first point */
TT_EXPORT int32_t tt_CalcHorizEpSubpix(int32_t s, const int32_t *px, const int32_t *py) {
    (void)s; (void)py;
    return px[0];
}

TT_EXPORT int32_t tt_CalcVertEpSubpix(int32_t s, const int32_t *px, const int32_t *py) {
    (void)s; (void)px;
    return py[0];
}
