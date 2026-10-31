/*
 * Measuring the hinted outline, DWrite.dll 10.0.26100.9711: fs_FindBitMapSize sizes the glyph
 * bitmap and the scan converter's memory before fs_ContourScan runs.
 *
 *   fs_FindBitMapSize            0x18006ad78  bitmap rect, metrics and memory sizes (fs_GlyphInfoType)
 *   fsc_RemoveDups               0x18006c5cc  drops repeated points from each contour
 *   fsc_OverScaleOutline         0x180069a38  scales the outline by the overscale factor
 *   fsc_MeasureGlyph             0x18006c710  bitmap rect, reversal lists, work memory estimate
 *   fsc_CheckYReversalInSpline   0x18006d3c8  y reversals of one quadratic spline (split at extrema)
 *   fsc_AddXReversal             0x18006d990  closes the x reversal list of a contour
 *   fsc_AddYReversal             0x18006d9f0  closes the y reversal list of a contour
 *
 * The ported functions call each other through ORIG() at the original RVA, like every other
 * callee: with one function swapped (reimpl/oracle.py --swap) only that one runs ported, with
 * all of them swapped the entry jumps lead to the ports.
 *
 * Layouts (offsets from the code):
 *   ContourList (0x38): nc u16 @0, sp i16* @8, ep i16* @0x10, x i32* @0x18, y i32* @0x20,
 *                       onCurve u8* @0x28, contour flags @0x30
 *   GlyphBitMap: rowBytes u16 @0, top @2, left @4, bottom @6, right @8 (i16), size i32 @0x1c
 *   WorkScan: memory i32 @0, then the sizes measured @4..0x14, RevRoots* @0x18
 *   RevRoots: y list @0, x list @8, next free node @0x10, limit @0x18, base @0x20,
 *             sentinel node @0x28 (0x7fff), nodes from @0x38
 *   reversal node (0x10): coordinate i16 @0, direction i16 @2, next @8
 */
#include <string.h>
#include "tt_state.h"
#include "orig.h"

#define CL_NC(c) AT(c, 0x00, uint16_t)
#define CL_SP(c) AT(c, 0x08, int16_t *)
#define CL_EP(c) AT(c, 0x10, int16_t *)
#define CL_X(c)  AT(c, 0x18, int32_t *)
#define CL_Y(c)  AT(c, 0x20, int32_t *)
#define CL_OC(c) AT(c, 0x28, uint8_t *)

#define BM_ROWBYTES(b) AT(b, 0x0, uint16_t)
#define BM_TOP(b)      AT(b, 0x2, int16_t)
#define BM_LEFT(b)     AT(b, 0x4, int16_t)
#define BM_BOTTOM(b)   AT(b, 0x6, int16_t)
#define BM_RIGHT(b)    AT(b, 0x8, int16_t)

#define RR_Y(r)     AT(r, 0x00, uint8_t *)
#define RR_X(r)     AT(r, 0x08, uint8_t *)
#define RR_FREE(r)  AT(r, 0x10, uint8_t *)
#define RR_LIMIT(r) AT(r, 0x18, uint8_t *)
#define RR_BASE(r)  AT(r, 0x20, uint8_t *)
#define RN_V(n)     AT(n, 0x0, int16_t)
#define RN_DIR(n)   AT(n, 0x2, int16_t)
#define RN_NEXT(n)  AT(n, 0x8, uint8_t *)

#define ERR_REVERSALS   0x1a00 /* reversal list full */
#define ERR_SPLINE_DIV  0x1306
#define ERR_RETRY_NODO  0x1305 /* fsc_MeasureGlyph: too many crossings for dropout control */

/* original RVAs */
#define RVA_FindBitMapSize       0x6ad78
#define RVA_RemoveDups           0x6c5cc
#define RVA_OverScaleOutline     0x69a38
#define RVA_MeasureGlyph         0x6c710
#define RVA_CheckYReversal       0x6d3c8
#define RVA_AddXReversal         0x6d990
#define RVA_AddYReversal         0x6d9f0

static void overflow(void) { ORIG(void (*)(void), 0x1090b0)(); } /* SafeIntOnOverflow: throws */
static int64_t CompDiv(int32_t d, int64_t n) { return ORIG(int64_t (*)(int32_t, int64_t), 0x6da90)(d, n); }

/* int64 value must fit in int32 (the SafeInt<int> checks: v + 0x80000000 > 0xffffffff) */
static int32_t fit32(int64_t v) {
    if ((uint64_t)v + 0x80000000u > 0xffffffffu) overflow();
    return (int32_t)v;
}

/* Insert a reversal node in a list kept sorted by coordinate; link is &list head. */
static void rev_insert(uint8_t *rr, uint8_t **link, uint8_t *node, int16_t v, int16_t dir) {
    while (v > RN_V(*link)) link = &RN_NEXT(*link);
    RN_V(node) = v;
    RN_DIR(node) = dir;
    RN_NEXT(node) = *link;
    *link = node;
    RR_FREE(rr) += 0x10;
}

/* ---------------------------------------------------------------------------------------------
 * fsc_AddXReversal / fsc_AddYReversal: the reversal that closes a contour, at
 * (v + 0x20 + (dir >> 1)) >> 6, with the opposite direction.
 */
static int add_reversal(uint8_t *rr, uint8_t **root, int32_t v, int16_t dir) {
    uint8_t *node = RR_FREE(rr);
    if (node + 0x10 > RR_LIMIT(rr)) return ERR_REVERSALS;
    int16_t key = (int16_t)((int32_t)((uint32_t)v + 0x20u + (uint32_t)((int32_t)dir >> 1)) >> 6);
    rev_insert(rr, root, node, key, (int16_t)-dir);
    return 0;
}

TT_EXPORT int tt_fsc_AddXReversal(uint8_t *rr, int32_t v, int16_t dir) { return add_reversal(rr, &RR_X(rr), v, dir); }
TT_EXPORT int tt_fsc_AddYReversal(uint8_t *rr, int32_t v, int16_t dir) { return add_reversal(rr, &RR_Y(rr), v, dir); }

/* ---------------------------------------------------------------------------------------------
 * fsc_CheckYReversalInSpline: the spline (x0,y0) (x1,y1) (x2,y2) is split at its y extremum,
 * then at its x extremum, until it is monotonic; a monotonic piece updates the running y
 * direction and adds a y reversal where it turns. The second half of a split is handled by
 * the loop, the first half by recursion (as the original).
 */
TT_EXPORT int tt_fsc_CheckYReversalInSpline(uint8_t *rr, int16_t *dir, int16_t *first, int16_t *flat, int32_t x0,
                                            int32_t y0, int32_t x1, int32_t y1, int32_t x2, int32_t y2) {
    for (;;) {
        int32_t dx1 = (int32_t)((uint32_t)x1 - (uint32_t)x0), dy1 = (int32_t)((uint32_t)y1 - (uint32_t)y0);
        int32_t dx2 = (int32_t)((uint32_t)x2 - (uint32_t)x1), dy2 = (int32_t)((uint32_t)y2 - (uint32_t)y1);
        int r;
        if ((dy1 > 0 && dy2 < 0) || (dy1 < 0 && dy2 > 0)) {
            int32_t den = (int32_t)((uint32_t)dy1 - (uint32_t)dy2);
            if (den == 0) return ERR_SPLINE_DIV;
            int64_t b = dy1;
            int32_t xa = (int32_t)((uint32_t)x0 + (uint32_t)CompDiv(den, (int64_t)dx1 * b));
            int32_t xb = (int32_t)((uint32_t)CompDiv(den, (int64_t)dx2 * b) + (uint32_t)x1);
            int32_t xm = (int32_t)((uint32_t)CompDiv(den, (int64_t)(int32_t)((uint32_t)xb - (uint32_t)xa) * b) + (uint32_t)xa);
            int32_t ym = (int32_t)((uint32_t)y0 + (uint32_t)CompDiv(den, b * b));
            r = ORIG(int (*)(uint8_t *, int16_t *, int16_t *, int16_t *, int32_t, int32_t, int32_t, int32_t, int32_t, int32_t),
                     RVA_CheckYReversal)(rr, dir, first, flat, x0, y0, xa, ym, xm, ym);
            if (r) return r;
            x0 = xm; y0 = ym; x1 = xb; y1 = ym;
        } else if ((dx1 > 0 && dx2 < 0) || (dx1 < 0 && dx2 > 0)) {
            int32_t den = (int32_t)((uint32_t)dx1 - (uint32_t)dx2);
            if (den == 0) return ERR_SPLINE_DIV;
            int64_t b = dx1;
            int32_t ya = (int32_t)((uint32_t)y0 + (uint32_t)CompDiv(den, (int64_t)dy1 * b));
            int32_t yb = (int32_t)((uint32_t)CompDiv(den, (int64_t)dy2 * b) + (uint32_t)y1);
            int32_t ym = (int32_t)((uint32_t)CompDiv(den, (int64_t)(int32_t)((uint32_t)yb - (uint32_t)ya) * b) + (uint32_t)ya);
            int32_t xm = (int32_t)((uint32_t)CompDiv(den, b * b) + (uint32_t)x0);
            r = ORIG(int (*)(uint8_t *, int16_t *, int16_t *, int16_t *, int32_t, int32_t, int32_t, int32_t, int32_t, int32_t),
                     RVA_CheckYReversal)(rr, dir, first, flat, x0, y0, xm, ya, xm, ym);
            if (r) return r;
            x0 = xm; y0 = ym; x1 = xm; y1 = yb;
        } else {
            if (*dir == 0) {
                if (y2 > y0) { *first = 1; *dir = 1; }
                else if (y2 == y0) (*flat)++;
                else { *dir = -1; *first = -1; }
                return 0;
            }
            uint8_t *node;
            if (*dir == 1) {
                if (y2 > y0) return 0;
                node = RR_FREE(rr);
                if (node + 0x10 > RR_LIMIT(rr)) return ERR_REVERSALS;
                rev_insert(rr, &RR_Y(rr), node, (int16_t)((int32_t)((uint32_t)y0 + 0x20u) >> 6), -1);
                *dir = -1;
                return 0;
            }
            if (y2 < y0) return 0;
            node = RR_FREE(rr);
            if (node + 0x10 > RR_LIMIT(rr)) return ERR_REVERSALS;
            rev_insert(rr, &RR_Y(rr), node, (int16_t)((int32_t)((uint32_t)y0 + 0x1fu) >> 6), 1);
            *dir = 1;
            return 0;
        }
    }
}

/* ---------------------------------------------------------------------------------------------
 * fsc_RemoveDups: a point equal to the next one is removed by shifting the points before it
 * one place up (the contour start moves up by one); the kept point becomes on-curve. Then a
 * last point equal to the first moves the start up too.
 */
TT_EXPORT void tt_fsc_RemoveDups(uint8_t *cl) {
    for (uint16_t c = 0; c < CL_NC(cl); c++) {
        int16_t *sp = CL_SP(cl);
        int32_t *x = CL_X(cl), *y = CL_Y(cl);
        int16_t s = sp[c], e = CL_EP(cl)[c], i = s;
        int32_t *px = x + s, *py = y + s;
        if (s < e) {
            do {
                int32_t ax = *px++, ay = *py++;
                if (*px == ax && *py == ay) {
                    if (i > s) {
                        uint64_t n = (uint16_t)(i - s);
                        int64_t j = i;
                        do {
                            CL_X(cl)[j] = CL_X(cl)[j - 1];
                            CL_Y(cl)[j] = CL_Y(cl)[j - 1];
                            CL_OC(cl)[j] = CL_OC(cl)[j - 1];
                            j--;
                        } while (--n);
                    }
                    s = (int16_t)(s + 1);
                    CL_SP(cl)[c] = s;
                    CL_OC(cl)[(int64_t)i + 1] |= 1;
                }
                i = (int16_t)(i + 1);
            } while (i < e);
            sp = CL_SP(cl); x = CL_X(cl); y = CL_Y(cl);
        }
        /* px, py: the end point (or the start when s >= e on entry) */
        if (s != e && *px == x[s] && *py == y[s]) {
            sp[c] = (int16_t)(sp[c] + 1);
            CL_OC(cl)[e] |= 1;
        }
    }
}

/* ---------------------------------------------------------------------------------------------
 * fsc_OverScaleOutline: x and y times the overscale (a shift for 1, 2, 4, 8). Always 0.
 */
TT_EXPORT int tt_fsc_OverScaleOutline(uint8_t *cl, uint16_t ov) {
    uint32_t m = ov;
    int16_t sh = m == 1 ? 0 : m == 2 ? 1 : m == 4 ? 2 : m == 8 ? 3 : -1;
    for (uint16_t c = 0; c < CL_NC(cl); c++) {
        int16_t s = CL_SP(cl)[c];
        uint16_t e = (uint16_t)CL_EP(cl)[c];
        int32_t *px = CL_X(cl) + s, *py = CL_Y(cl) + s;
        if (s > (int16_t)e) continue;
        uint64_t n = (uint16_t)(e - (uint16_t)s + 1);
        if (sh >= 0) {
            do {
                *px = (int32_t)((uint32_t)*px << (sh & 31)); px++;
                *py = (int32_t)((uint32_t)*py << (sh & 31)); py++;
            } while (--n);
        } else {
            do {
                *px = (int32_t)(m * (uint32_t)*px); px++;
                *py = (int32_t)(m * (uint32_t)*py); py++;
            } while (--n);
        }
    }
    return 0;
}

/* ---------------------------------------------------------------------------------------------
 * fsc_MeasureGlyph
 *   flags (fs key + 0x170): 2 = no x reversals (no horizontal dropout pass),
 *                           4 = dropout control with the crossing estimate (local count)
 *   ov: overscale, the left edge is aligned down to it; adjx, adjy widen the rect
 */
/* |v| as NEG + CMOVS: INT32_MIN stays INT32_MIN */
static int32_t neg_abs(int32_t v) {
    int32_t n = (int32_t)(0u - (uint32_t)v);
    return n < 0 ? v : n;
}

TT_EXPORT int tt_fsc_MeasureGlyph(uint8_t *cl, uint8_t *bm, uint8_t *ws, uint16_t flags, uint16_t ov, int16_t adjx,
                                  int16_t adjy) {
    int r = ORIG(int (*)(uint8_t *, uint8_t *, uint8_t *, uint16_t), 0x6d780)(cl, bm, ws, flags); /* FindExtrema */
    if (r) return r;
    uint16_t left = (uint16_t)(0u - ov) & (uint16_t)BM_LEFT(bm);
    BM_LEFT(bm) = (int16_t)left;
    uint16_t top = (uint16_t)BM_TOP(bm), bottom = (uint16_t)BM_BOTTOM(bm);
    if (top != bottom) {
        uint16_t right = (uint16_t)BM_RIGHT(bm);
        if (left != right) {
            if (adjx > 0) BM_RIGHT(bm) = (int16_t)(right + adjx);
            else BM_LEFT(bm) = (int16_t)(left + adjx);
            if (adjy > 0) BM_BOTTOM(bm) = (int16_t)(bottom - adjy);
            else BM_TOP(bm) = (int16_t)(top - adjy);
        }
    }

    int64_t wsz = AT(ws, 0, int32_t);
    uint8_t *rr = AT(ws, 0x18, uint8_t *);
    if ((uint64_t)wsz < 0x38) return ERR_REVERSALS;
    uint8_t *sent = rr + 0x28;
    AT(sent, 0, int32_t) = 0x7fff;
    RR_Y(rr) = sent;
    RR_X(rr) = sent;
    RN_NEXT(sent) = NULL;
    RR_FREE(rr) = rr + 0x38;
    RR_LIMIT(rr) = rr + wsz;
    RR_BASE(rr) = rr;
    int32_t est = 0; /* crossing estimate for flags & 4 */

    for (uint16_t ci = 0; ci < CL_NC(cl); ci++) {
        int64_t s = CL_SP(cl)[ci], e = CL_EP(cl)[ci];
        if ((int16_t)s == (int16_t)e) continue;
        int32_t *X = CL_X(cl), *Y = CL_Y(cl);
        uint8_t *OC = CL_OC(cl);
        int32_t *qx = X + s, *qy = Y + s, *xend = X + e;
        uint8_t *qf = OC + s;
        int32_t px, py, cx, cy;
        uint8_t f;

        /* ---- y reversals ---- */
        if (OC[e] & 1) {
            px = X[e];
            py = Y[e];
            xend++;
            cx = *qx; cy = *qy; f = *qf;
        } else {
            /* off-curve end: it stands before the start, the previous point is the end-1
             * point or the midpoint */
            cx = X[e]; cy = Y[e];
            px = X[e - 1]; py = Y[e - 1];
            if (!(OC[e - 1] & 1)) {
                px = (int32_t)((uint32_t)px + 1u + (uint32_t)cx) >> 1;
                py = (int32_t)((uint32_t)py + 1u + (uint32_t)cy) >> 1;
            }
            f = 0;
            qx--; qy--; qf--;
        }
        int16_t dir = 0, flat = 0, first = 1;
        while (qx < xend) {
            if (!(f & 1)) {
                uint8_t nf = qf[1] & 1;
                qf++;
                int32_t nx = qx[1], ny = qy[1];
                int step;
                if (!nf) {
                    nx = (int32_t)((uint32_t)nx + (uint32_t)cx + 1u) >> 1;
                    ny = (int32_t)((uint32_t)ny + (uint32_t)cy + 1u) >> 1;
                    step = 1;
                } else {
                    qf++;
                    step = 2;
                }
                r = ORIG(int (*)(uint8_t *, int16_t *, int16_t *, int16_t *, int32_t, int32_t, int32_t, int32_t, int32_t,
                                 int32_t), RVA_CheckYReversal)(rr, &dir, &first, &flat, px, py, cx, cy, nx, ny);
                if (r) return r;
                px = nx; py = ny;
                qx += step;
                qy += nf;
            } else {
                if (dir == 0) {
                    if (cy > py) { dir = 1; first = 1; }
                    else if (cy == py) flat++;
                    else { dir = -1; first = -1; }
                } else if (dir == 1) {
                    if (cy <= py) {
                        uint8_t *node = RR_FREE(rr);
                        if (node + 0x10 > RR_LIMIT(rr)) return ERR_REVERSALS;
                        rev_insert(rr, &RR_Y(rr), node, (int16_t)((int32_t)((uint32_t)py + 0x20u) >> 6), -1);
                        dir = -1;
                    }
                } else {
                    if (cy >= py) {
                        uint8_t *node = RR_FREE(rr);
                        if (node + 0x10 > RR_LIMIT(rr)) return ERR_REVERSALS;
                        rev_insert(rr, &RR_Y(rr), node, (int16_t)((int32_t)((uint32_t)py + 0x1fu) >> 6), 1);
                        dir = 1;
                    }
                }
                qx++; qf++;
                px = cx; py = cy;
            }
            qy++;
            if (qx != xend) { cx = *qx; cy = *qy; f = *qf; }
        }
        while (flat > 0) { /* level contour parts: reversals at the last y */
            if (dir == 0) dir = 1;
            uint8_t *node = RR_FREE(rr);
            if (node + 0x10 > RR_LIMIT(rr)) return ERR_REVERSALS;
            int16_t key = (int16_t)((int32_t)((uint32_t)((int32_t)dir >> 1) + 0x20u + (uint32_t)py) >> 6);
            rev_insert(rr, &RR_Y(rr), node, key, (int16_t)-dir);
            dir = (int16_t)-dir;
            flat--;
        }
        if (first != dir) {
            r = ORIG(int (*)(uint8_t *, int32_t, int16_t), RVA_AddYReversal)(rr, py, dir);
            if (r) return r;
        }
        if (flags & 2) continue;

        /* ---- x reversals (on all points, on- and off-curve) ---- */
        {
            int32_t *p = CL_X(cl) + s;
            int16_t i = (int16_t)s, end = (int16_t)e;
            int32_t prev = CL_X(cl)[e];
            int16_t d = 0, fl = 0;
            while (i <= end) {
                int32_t v = *p++;
                if (v > prev) d = 1;
                else if (v < prev) d = -1;
                else fl++;
                i++;
                prev = v;
                if (d != 0) break;
            }
            int16_t xfirst = d;
            while (i <= end) {
                int32_t v = *p++;
                if (d == 1) {
                    if (v <= prev) {
                        uint8_t *node = RR_FREE(rr);
                        if (node + 0x10 > RR_LIMIT(rr)) return ERR_REVERSALS;
                        rev_insert(rr, &RR_X(rr), node, (int16_t)((int32_t)((uint32_t)prev + 0x20u) >> 6), -1);
                        d = -1;
                    }
                } else if (v >= prev) {
                    uint8_t *node = RR_FREE(rr);
                    if (node + 0x10 > RR_LIMIT(rr)) return ERR_REVERSALS;
                    rev_insert(rr, &RR_X(rr), node, (int16_t)((int32_t)((uint32_t)prev + 0x1fu) >> 6), 1);
                    d = 1;
                }
                prev = v;
                i++;
            }
            while (fl > 0) {
                if (d == 0) { d = 1; xfirst = 1; } /* unlike the y pass, the first direction too */
                uint8_t *node = RR_FREE(rr);
                if (node + 0x10 > RR_LIMIT(rr)) return ERR_REVERSALS;
                int16_t key = (int16_t)((int32_t)((uint32_t)((int32_t)d >> 1) + 0x20u + (uint32_t)prev) >> 6);
                rev_insert(rr, &RR_X(rr), node, key, (int16_t)-d);
                d = (int16_t)-d;
                fl--;
            }
            if (xfirst != d) {
                r = ORIG(int (*)(uint8_t *, int32_t, int16_t), RVA_AddXReversal)(rr, prev, d);
                if (r) return r;
            }
        }
        if (flags & 4) {
            /* crossing estimate: 2 + (points - 1), then per segment not between two on-curve
             * points 1 (both off) and 2 per halving of its longer side above 0x640 */
            int32_t *X2 = CL_X(cl), *Y2 = CL_Y(cl);
            uint8_t *O2 = CL_OC(cl);
            int32_t lx = X2[e], ly = Y2[e];
            uint8_t lf = O2[e];
            int32_t acc = (int32_t)((uint32_t)est + 2u + (uint32_t)((int32_t)e - (int32_t)s));
            est = acc;
            if (!((int16_t)s > (int16_t)e)) {
                uint64_t n = (uint16_t)((uint16_t)e - (uint16_t)s + 1);
                int32_t *ax = X2 + s, *ay = Y2 + s;
                uint8_t *af = O2 + s;
                do {
                    uint8_t f2 = *af++;
                    int32_t vx = *ax++, vy = *ay++;
                    if (!(f2 & lf & 1)) {
                        int32_t base = ((f2 | lf) & 1) ? acc : (int32_t)((uint32_t)acc + 1u);
                        int32_t ddx = (int32_t)((uint32_t)vx - (uint32_t)lx), ddy = (int32_t)((uint32_t)vy - (uint32_t)ly);
                        int32_t dd = neg_abs(ddx) <= neg_abs(ddy) ? ddy : ddx;
                        uint32_t ad = (uint32_t)neg_abs(dd);
                        int32_t c = 0;
                        if ((int32_t)ad > 0x640) {
                            do { ad >>= 1; c = (int32_t)(2u + 2u * (uint32_t)c); } while (ad > 0x640u);
                        }
                        acc = (int32_t)((uint32_t)base + (uint32_t)c);
                    }
                    lx = vx; ly = vy; lf = f2;
                } while (--n);
                est = acc;
            }
        }
    }

    int32_t est2 = est;
    if ((flags & 6) == 4) {
        int64_t used = ((int64_t)(RR_FREE(rr) - rr) - 0x38) >> 4;
        est2 = (int32_t)((uint32_t)est + (uint32_t)used * 2u);
        if (est2 > 0x3fff) return ERR_RETRY_NODO;
    }
    int32_t height = (int32_t)BM_TOP(bm) - (int32_t)BM_BOTTOM(bm);
    int32_t width = (int32_t)BM_RIGHT(bm) - (int32_t)BM_LEFT(bm);
    if (width < 0) overflow();
    uint64_t t = (uint64_t)(uint32_t)width + 0x1f;
    if (t > 0xffffffffu) overflow();
    uint32_t rb = ((uint32_t)t >> 3) & 0x1ffffffc;
    if (rb > 0xffff) overflow();
    if ((uint16_t)rb > 0x7fff) overflow();
    int64_t sz = (int64_t)(int16_t)rb * (int64_t)height;
    BM_ROWBYTES(bm) = (uint16_t)rb;
    AT(bm, 0x1c, int32_t) = fit32(sz);
    if (rr != RR_BASE(rr)) ORIG(void (*)(uint8_t *), 0x10b2fc)(rr); /* FixPointers */
    int32_t yest = ORIG(int32_t (*)(uint8_t *), 0x6d958)(RR_Y(rr)); /* GetIxEstimate */
    int32_t hr = fit32((int64_t)height + 7) & ~7;
    int32_t er = fit32((int64_t)yest + 7) & ~7;
    int32_t a = fit32((int64_t)hr * 40);
    int32_t b = fit32((int64_t)er * ((flags & 4) ? 8 : 4));
    AT(ws, 4, int32_t) = fit32((int64_t)b + a);
    if (flags & 2) {
        AT(ws, 8, int32_t) = 0;
        AT(ws, 0xc, int32_t) = yest;
        AT(ws, 0x10, int32_t) = 0;
        AT(ws, 0x14, int32_t) = est2;
    } else {
        if (rr != RR_BASE(rr)) ORIG(void (*)(uint8_t *), 0x10b2fc)(rr);
        int32_t xest = ORIG(int32_t (*)(uint8_t *), 0x6d958)(RR_X(rr));
        int32_t wr = fit32((int64_t)width + 7) & ~7;
        int32_t xr = fit32((int64_t)xest + 7) & ~7;
        int32_t cr = fit32((int64_t)est2 + 7) & ~7;
        int32_t sum;
        if (!(flags & 4)) {
            int32_t a2 = fit32((int64_t)wr * 40);
            int32_t b2 = fit32((int64_t)xr * 4);
            sum = fit32((int64_t)b2 + a2);
        } else {
            int32_t a2 = fit32((int64_t)wr * 40);
            int32_t b2 = fit32((int64_t)xr * 8);
            int32_t s2 = fit32((int64_t)b2 + a2);
            int32_t c2 = fit32((int64_t)cr * 8);
            sum = fit32((int64_t)c2 + s2);
        }
        AT(ws, 8, int32_t) = sum;
        AT(ws, 0xc, int32_t) = yest;
        AT(ws, 0x10, int32_t) = xest;
        AT(ws, 0x14, int32_t) = est2;
    }
    AT(ws, 0, int32_t) = (int32_t)((uint32_t)(uintptr_t)RR_FREE(rr) - (uint32_t)(uintptr_t)rr);
    int64_t total = (int64_t)AT(ws, 4, int32_t) + 0x138;
    if (total > 0x7fffffff) overflow();
    AT(ws, 4, int32_t) = (int32_t)total;
    return 0;
}

/* ---------------------------------------------------------------------------------------------
 * fs_FindBitMapSize
 *   in  fs_GlyphInputType (key @8, memory bases @8 + 8i, callbacks @0x50.., +0x88 sbit request,
 *       +0x8c rasterization kind: 2 and 3 are special cases below, 0 elsewhere)
 *   key fsg key (fs_SplineKey): +0x1e8 glyph element, +0x2d0 bitmap, +0x2f8 WorkScan,
 *       +0x328 overscaled bitmap, +0x3c0 sbit state, +0x488 overscale, +0x49a/0x49c
 *       subpixel overscale x/y, +0x41a ClearType flags
 *   info fs_GlyphInfoType: memory sizes 5..8 @0x14..0x20, metrics @0x28..0x58 and @0xac..0xdc,
 *       bitmap @0x58 (rowBytes @0x60, rect @0x62..0x68), subpixel rect @0xf2..0xf8
 */

/* +0x8c == 3: subpixel bitmap without the ClearType flag, rowBytes from the full width */
static void fbms_subpixel_rect(uint8_t *key, uint8_t *info) {
    AT(info, 0x60, uint16_t) = ORIG(uint16_t (*)(int32_t), 0xed964)( /* ROWBYTESLONG<int> */
        (int32_t)((uint32_t)((int32_t)AT(info, 0x68, int16_t) - (int32_t)AT(info, 0x64, int16_t)) *
                  (uint32_t)AT(key, 0x49a, uint16_t)));
    AT(info, 0xf4, int16_t) = (int16_t)((int32_t)AT(key, 0x49a, int16_t) * (int32_t)AT(key, 0x2d4, int16_t));
    AT(info, 0xf2, int16_t) = AT(key, 0x2d6, int16_t);
    AT(info, 0xf8, int16_t) = (int16_t)((int32_t)AT(key, 0x49a, int16_t) * (int32_t)AT(key, 0x2d8, int16_t));
    AT(info, 0xf6, int16_t) = AT(key, 0x2d2, int16_t);
    AT(key, 0x2d0, uint16_t) = AT(info, 0x60, uint16_t);
    AT(key, 0x2ec, int32_t) = fit32((int64_t)((int32_t)AT(key, 0x2d2, int16_t) - (int32_t)AT(key, 0x2d6, int16_t)) *
                                    (int64_t)AT(info, 0x60, int16_t));
    memcpy(key + 0x328, key + 0x2d0, 0x28);
    AT(info, 0x20, int32_t) = AT(key, 0x344, int32_t);
    AT(info, 0x14, int32_t) = AT(key, 0x344, int32_t);
}

/* memory: work memory (info sizes 6 and 7) placed in the key's own memory when it fits */
static int fbms_tail(uint8_t *in, uint8_t *key, uint8_t *info) {
    if (AT(in, 0x8c, int32_t) == 2) {
        uint16_t rb = (uint16_t)((uint16_t)AT(info, 0x68, int16_t) - (uint16_t)AT(info, 0x64, int16_t) + 3) & 0xfffc;
        AT(info, 0x60, uint16_t) = rb;
        int32_t res;
        ORIG(int32_t *(*)(int32_t *, int32_t, int32_t), 0xd942c)( /* SafeInt operator*<int,int> */
            &res, (int32_t)AT(info, 0x66, int16_t) - (int32_t)AT(info, 0x62, int16_t), (int32_t)(int16_t)rb);
        AT(info, 0x14, int32_t) = res;
    }
    int64_t used = AT(key, 0x2f8, int32_t);
    uint64_t base = (uint64_t)AT(key, 0x24c, uint32_t) + (uint64_t)used;
    if (base > 0xffffffffu) overflow();
    int32_t avail = fit32((int64_t)AT(key, 0x3b4, int32_t) - used);
    uint32_t b32 = (uint32_t)base;
    AT(key, 0x250, uint64_t) = 0;
    int32_t *A = &AT(info, 0x18, int32_t), *B = &AT(info, 0x1c, int32_t);
    if (*A > *B) {
        if (*A > avail) {
            if (*B > avail || *B <= 0) goto done;
            AT(key, 0x254, uint32_t) = b32;
            *B = 0;
            goto done;
        }
        AT(key, 0x250, uint32_t) = b32;
        uint64_t nb = (uint64_t)b32 + (uint64_t)(int64_t)*A;
        if (nb > 0xffffffffu) overflow();
        int32_t rest = fit32((int64_t)avail - *A);
        if (*B > 0 && *B <= rest) {
            AT(key, 0x254, uint32_t) = (uint32_t)nb;
            *B = 0;
        }
        *A = 0;
    } else {
        if (*B > avail || *B <= 0) {
            if (*A > avail || *A <= 0) goto done;
            AT(key, 0x250, uint32_t) = b32;
            *A = 0;
            goto done;
        }
        AT(key, 0x254, uint32_t) = b32;
        uint64_t nb = (uint64_t)b32 + (uint64_t)(int64_t)*B;
        if (nb > 0xffffffffu) overflow();
        int32_t rest = fit32((int64_t)avail - *B);
        if (*A > 0 && *A <= rest) {
            AT(key, 0x250, uint32_t) = (uint32_t)nb;
            *A = 0;
        }
        *B = 0;
    }
done:
    AT(key, 0x1d0, uint32_t) = 0x3e;
    return 0;
}

/* advance width (vw = info + 0x28) or height (+0xac), outline path; adv in font units */
static int32_t fbms_scale_div(int64_t num, int64_t upem) {
    int32_t half = (int32_t)upem / 2;
    if ((int32_t)upem < 0) return (int32_t)((num - half) / upem);
    num += half;
    if (upem == 0) return (int32_t)(0x7fffffffu + (num < 0));
    return (int32_t)(num / upem);
}

static int32_t mean_div(int32_t v, uint16_t d) { return (int32_t)((uint32_t)(d >> 1) + (uint32_t)v) / (int32_t)d; }

TT_EXPORT int tt_fs_FindBitMapSize(uint8_t *in, uint8_t *info) {
    uint8_t *key = AT(in, 8, uint8_t *);
    if (!key || !AT(in, 0x20, void *) || !AT(in, 0x28, void *)) return 0x1003;
    AT(key, 0x118, uint8_t *) = in + 8;
    AT(key, 0x120, uint8_t *) = info;
    if (!info) return 0x1003;
    if (!(AT(key, 0x8, void *) = AT(in, 0x50, void *))) return 0x1008;
    if (!(AT(key, 0x10, void *) = AT(in, 0x58, void *)))
        AT(key, 0x10, void *) = (uint8_t *)idwr_orig(0xe5100); /* default ReleaseSfntFrag */
    if (!(AT(key, 0xf8, void *) = AT(in, 0x60, void *))) return 0x100d;
    if (!(AT(key, 0x100, void *) = AT(in, 0x68, void *))) return 0x100e;
    if (!(AT(key, 0x108, void *) = AT(in, 0x70, void *))) return 0x100f;
    if ((AT(key, 0x1d0, uint8_t) & 0x1e) != 0x1e) return 0x1005;
    AT(key, 0, void *) = AT(in, 0x78, void *);
    uint8_t *mem3 = AT(in, 0x20, uint8_t *);
    if (AT(key, 0x140, uint8_t *) != mem3) { /* memory bases moved: rebuild the work space addresses */
        for (int k = 0; k < 5; k++) AT(key, 0x1d8 + 8 * k, uint8_t *) = mem3 + AT(key, 0x208 + 4 * k, uint32_t);
        AT(key, 0x200, uint8_t *) = mem3 + AT(key, 0x24c, uint32_t);
        ORIG(void (*)(uint8_t *, uint8_t *), 0xcc07c)(key + 0x208, key + 0x1d8); /* fsg_UpdateWorkSpaceElement */
        memcpy(key + 0x128, AT(key, 0x118, uint8_t *), 0x48);
    }
    uint8_t *mem4 = AT(in, 0x28, uint8_t *);
    uint8_t *gs = mem4 + AT(key, 0x268, uint32_t); /* fnt_GlobalGraphicStateType */
    if ((uintptr_t)gs < (uintptr_t)mem4) overflow();
    int32_t kind = AT(in, 0x8c, int32_t);

    if (!AT(key, 0x351, uint8_t) || (AT(in, 0x88, uint8_t) && (AT(key, 0x41a, uint8_t) & 1))) {
        /* ---- outline ---- */
        if (AT(key, 0x19c, uint8_t)) { /* x + 1 on every point but the phantoms */
            uint8_t *el = AT(key, 0x1e8, uint8_t *);
            int16_t n = AT(el, 0x40, int16_t *)[AT(el, 0x50, int16_t) - 1];
            int32_t *x = AT(el, 0, int32_t *);
            if (n >= 0) do { *x = (int32_t)((uint32_t)*x + 1u); x++; } while (--n >= 0);
        }
        uint8_t *el = AT(key, 0x1e8, uint8_t *);
        uint64_t clbuf[7] = {0};
        uint8_t *cl = (uint8_t *)clbuf;
        AT(cl, 0x00, uint16_t) = AT(el, 0x50, uint16_t);
        AT(cl, 0x08, void *) = AT(el, 0x38, void *);
        AT(cl, 0x10, void *) = AT(el, 0x40, void *);
        AT(cl, 0x18, void *) = AT(el, 0x00, void *);
        AT(cl, 0x20, void *) = AT(el, 0x08, void *);
        AT(cl, 0x28, void *) = AT(el, 0x30, void *);
        AT(cl, 0x30, void *) = AT(el, 0x58, void *);
        ORIG(void (*)(uint8_t *), RVA_RemoveDups)(cl);
        uint16_t ovs = 1;
        uint8_t *bm = key + 0x2d0;
        int r;
        if (kind != 2 && AT(key, 0x488, uint16_t)) {
            r = ORIG(int (*)(uint8_t *, uint16_t), RVA_OverScaleOutline)(cl, AT(key, 0x488, uint16_t));
            if (r) return r;
            bm = key + 0x328;
            ovs = ORIG(uint16_t (*)(uint8_t *, uint8_t *), 0xd6084)(in, key); /* ScalerOverScale */
        }
        uint8_t *mem3b = AT(AT(key, 0x118, uint8_t *), 0x18, uint8_t *);
        uint8_t *rr = mem3b + AT(key, 0x24c, uint32_t);
        if ((uintptr_t)rr < (uintptr_t)mem3b) overflow();
        uint8_t *wsc = key + 0x2f8;
        AT(wsc, 0, int32_t) = AT(key, 0x3b4, int32_t);
        AT(key, 0x310, uint8_t *) = rr;
        int16_t adjx = 0, adjy = 0;
        if (kind != 0) {
            if (!ORIG(uint8_t (*)(uint8_t *, uint8_t *), 0xd3ea8)(in, key)) { /* IsGrayScale */
                adjx = AT(key, 0x416, int16_t);
                adjy = AT(key, 0x418, int16_t);
                if (AT(key, 0x41a, uint8_t) & 1) {
                    adjx = (int16_t)((int32_t)AT(key, 0x49a, int16_t) * adjx);
                    adjy = (int16_t)((int32_t)AT(key, 0x49c, int16_t) * adjy);
                }
            } else {
                adjx = (int16_t)((int32_t)(int16_t)ORIG(uint16_t (*)(uint8_t *, uint8_t *), 0xd6084)(in, key) *
                                 (int32_t)AT(key, 0x416, int16_t));
                adjy = (int16_t)((int32_t)(int16_t)ORIG(uint16_t (*)(uint8_t *, uint8_t *), 0xd6084)(in, key) *
                                 (int32_t)AT(key, 0x418, int16_t));
            }
        }
        typedef int (*measure_fn)(uint8_t *, uint8_t *, uint8_t *, uint16_t, uint16_t, int16_t, int16_t);
        r = ORIG(measure_fn, RVA_MeasureGlyph)(cl, bm, wsc, AT(key, 0x170, uint16_t), ovs, adjx, adjy);
        if (r == ERR_RETRY_NODO) { /* again without the dropout estimate */
            AT(key, 0x170, uint16_t) &= 0xfffb;
            r = ORIG(measure_fn, RVA_MeasureGlyph)(cl, bm, wsc, AT(key, 0x170, uint16_t), ovs, adjx, adjy);
        }
        if (r) return r;
        if (kind != 2 && AT(key, 0x488, uint16_t)) { /* overscaled rect back to pixels */
            uint16_t ov = ORIG(uint16_t (*)(uint8_t *, uint8_t *), 0xd6084)(in, key);
            if (ov > 0x7fff) overflow();
            int32_t (*divshift)(int32_t, int16_t) = ORIG(int32_t (*)(int32_t, int16_t), 0x10b7fc); /* mth_DivShiftLong */
            if (!AT(key, 0x350, uint8_t)) {
                AT(key, 0x2d4, int16_t) = (int16_t)divshift(AT(key, 0x32c, int16_t), (int16_t)ov);
                AT(key, 0x2d8, int16_t) = (int16_t)divshift((int16_t)(ov + AT(key, 0x330, uint16_t) - 1), (int16_t)ov);
                AT(key, 0x2d6, int16_t) = (int16_t)divshift(AT(key, 0x32e, int16_t), (int16_t)ov);
                AT(key, 0x2d2, int16_t) = (int16_t)divshift((int16_t)(ov + AT(key, 0x32a, uint16_t) - 1), (int16_t)ov);
            } else { /* from the 26.6 extrema FindExtrema kept */
                int16_t a = (int16_t)((int32_t)((uint32_t)divshift(AT(key, 0x334, int32_t), (int16_t)ov) + 0x1f) >> 6);
                AT(key, 0x2d4, int16_t) = a;
                int16_t b = (int16_t)((int32_t)((uint32_t)divshift(AT(key, 0x338, int32_t), (int16_t)ov) + 0x20) >> 6);
                AT(key, 0x2d8, int16_t) = b;
                int16_t c = (int16_t)((int32_t)((uint32_t)divshift(AT(key, 0x33c, int32_t), (int16_t)ov) + 0x1f) >> 6);
                AT(key, 0x2d6, int16_t) = c;
                int16_t d = (int16_t)((int32_t)((uint32_t)divshift(AT(key, 0x340, int32_t), (int16_t)ov) + 0x20) >> 6);
                AT(key, 0x2d2, int16_t) = d;
                if (a == b) AT(key, 0x2d8, int16_t) = (int16_t)(b + 1);
                if (c == d) AT(key, 0x2d2, int16_t) = (int16_t)(d + 1);
            }
        }

        /* metrics from the phantom points (ep + 1 .. ep + 8), 26.6 -> 16.16 */
        el = AT(key, 0x1e8, uint8_t *);
        int32_t *X = AT(el, 0, int32_t *), *Y = AT(el, 8, int32_t *);
        uint16_t ep = AT(el, 0x40, uint16_t *)[AT(el, 0x50, int16_t) - 1];
#define PH(k) ((uint16_t)(ep + (k)))
        uint32_t top64 = (uint32_t)(int32_t)AT(key, 0x2d2, int16_t) << 6;
        uint32_t left64 = (uint32_t)(int32_t)AT(key, 0x2d4, int16_t) << 6;
        uint32_t a0 = left64 - (uint32_t)X[PH(1)], a1 = top64 - (uint32_t)Y[PH(1)];
        uint32_t c0 = (uint32_t)X[PH(6)] - (uint32_t)X[PH(5)];
        uint32_t c1 = top64 - (uint32_t)Y[PH(5)] - (uint32_t)Y[PH(6)];
        AT(info, 0x4c, uint32_t) = ((uint32_t)Y[PH(2)] - (uint32_t)Y[PH(1)]) << 10;
        AT(info, 0x48, uint32_t) = ((uint32_t)X[PH(2)] - (uint32_t)X[PH(1)]) << 10;
        AT(info, 0x50, uint32_t) = a0 << 10;
        AT(info, 0x40, uint32_t) = a0 << 10;
        AT(info, 0x34, uint32_t) = c1 << 10;
        AT(info, 0x44, uint32_t) = a1 << 10;
        AT(info, 0x3c, uint32_t) = c1 << 10;
        AT(info, 0x30, uint32_t) = c0 << 10;
        AT(info, 0x38, uint32_t) = c0 << 10;
        AT(info, 0x54, uint32_t) = a1 << 10;
        el = AT(key, 0x1e8, uint8_t *);
        X = AT(el, 0, int32_t *); Y = AT(el, 8, int32_t *);
        ep = AT(el, 0x40, uint16_t *)[AT(el, 0x50, int16_t) - 1];
        top64 = (uint32_t)(int32_t)AT(key, 0x2d2, int16_t) << 6;
        left64 = (uint32_t)(int32_t)AT(key, 0x2d4, int16_t) << 6;
        uint32_t v0 = left64 - (uint32_t)X[PH(3)], v1 = top64 - (uint32_t)Y[PH(3)];
        uint32_t w0 = left64 - (uint32_t)X[PH(7)] - (uint32_t)X[PH(8)];
        uint32_t w1 = (uint32_t)Y[PH(8)] - (uint32_t)Y[PH(7)];
        AT(info, 0xc8, uint32_t) = v1 << 10;
        AT(info, 0xd0, uint32_t) = ((uint32_t)Y[PH(3)] - (uint32_t)Y[PH(4)]) << 10;
        AT(info, 0xd8, uint32_t) = v1 << 10;
        AT(info, 0xd4, uint32_t) = v0 << 10;
        AT(info, 0xc4, uint32_t) = v0 << 10;
        AT(info, 0xb4, uint32_t) = w0 << 10;
        AT(info, 0xbc, uint32_t) = w0 << 10;
        AT(info, 0xb8, uint32_t) = w1 << 10;
        AT(info, 0xc0, uint32_t) = w1 << 10;
        AT(info, 0xcc, uint32_t) = ((uint32_t)X[PH(3)] - (uint32_t)X[PH(4)]) << 10;
#undef PH

        /* linear advances (scl_ScaleAdvanceWidth/Height inlined) */
        int64_t upem = AT(key, 0x174, int16_t);
        int32_t *vw = &AT(info, 0x28, int32_t);
        AT(info, 0x2c, int32_t) = 0;
        if (AT(key, 0x19d, uint8_t)) {
            int32_t q = fbms_scale_div((int64_t)AT(gs, 0x18c, int32_t) * (int64_t)AT(key, 0x1ac, int16_t), upem);
            *vw = q;
            if (q != 0 && AT(gs, 0x1b6, int16_t) != 0) *vw = (int32_t)((uint32_t)q + 0x10000u);
        } else {
            int32_t q = ORIG(int32_t (*)(int64_t), 0xbc10)( /* CHKOVF */
                CompDiv((int32_t)upem, (int64_t)AT(key, 0x1ac, int16_t) << 16));
            *vw = q;
            int32_t m = AT(gs, 0x18c, int32_t);
            if (m != 0x10000 && AT(gs, 0x1b6, int16_t) != 0) {
                q = ORIG(int32_t (*)(int32_t, int32_t), 0x67b70)(q, m); /* DWRITE_FixMul */
                *vw = q;
                if (q != 0 && AT(gs, 0x1b6, int16_t) != 0) { q = (int32_t)((uint32_t)q + 0x10000u); *vw = q; }
                *vw = ORIG(int32_t (*)(int32_t, int32_t), 0x69800)(q, AT(gs, 0x18c, int32_t)); /* DWRITE_FixDiv */
            } else if (q != 0 && AT(gs, 0x1b6, int16_t) != 0) {
                *vw = (int32_t)((uint32_t)q + 0x10000u);
            }
            ORIG(void (*)(int32_t *, int32_t *, uint8_t *), 0x687bc)(vw, vw + 1, key + 0x178); /* mth_FixXYMul */
        }
        int32_t *vh = &AT(info, 0xac, int32_t);
        *vh = 0;
        upem = AT(key, 0x174, int16_t);
        if (AT(key, 0x19d, uint8_t)) {
            int32_t q = fbms_scale_div((int64_t)AT(gs, 0x190, int32_t) * (int64_t)AT(key, 0x1ae, int16_t), upem);
            vh[1] = q;
            if (q != 0) vh[1] = (int32_t)((uint32_t)q + 0x10000u);
        } else {
            int32_t q = ORIG(int32_t (*)(int64_t), 0xbc10)(CompDiv((int32_t)upem, (int64_t)AT(key, 0x1ae, int16_t) << 16));
            vh[1] = q;
            if (q != 0) vh[1] = (int32_t)((uint32_t)q + 0x10000u);
            ORIG(void (*)(int32_t *, int32_t *, uint8_t *), 0x687bc)(vh, vh + 1, key + 0x178);
        }

        memcpy(key + 0x354, info + 0x28, 0x30);
        memcpy(key + 0x384, info + 0xac, 0x30);
        AT(info, 0x62, int16_t) = AT(key, 0x2d6, int16_t);
        AT(info, 0x66, int16_t) = AT(key, 0x2d2, int16_t);
        AT(info, 0x58, uint64_t) = 0;
        uint16_t ct = AT(key, 0x41a, uint16_t);
        if (!(ct & 1)) {
            AT(info, 0x64, int16_t) = AT(key, 0x2d4, int16_t);
            AT(info, 0x68, int16_t) = AT(key, 0x2d8, int16_t);
            AT(info, 0x60, uint16_t) = AT(key, 0x2d0, uint16_t);
        } else {
            /* ClearType: metrics and rect from subpixels back to pixels */
            if (!(ct & 0x40)) {
                static const uint8_t offs[20] = {0x48, 0x4c, 0x50, 0x54, 0x30, 0x34, 0x40, 0x44, 0x38, 0x3c,
                                                 0xcc, 0xd0, 0xd4, 0xd8, 0xb4, 0xb8, 0xc4, 0xc8, 0xbc, 0xc0};
                for (int k = 0; k < 20; k++)
                    AT(info, offs[k], int32_t) = mean_div(AT(info, offs[k], int32_t), AT(key, (k & 1) ? 0x49c : 0x49a, uint16_t));
            }
            uint16_t dx = AT(key, 0x49a, uint16_t);
            int32_t L = AT(key, 0x2d4, int16_t);
            int32_t nl = L < 0 ? -(((int32_t)dx - L - 1) / (int32_t)dx) : L / (int32_t)dx;
            AT(info, 0x64, int16_t) = (int16_t)nl;
            dx = AT(key, 0x49a, uint16_t);
            int32_t R = AT(key, 0x2d8, int16_t), t = (int32_t)dx - 1 + R;
            if (t < 0) t = R;
            int32_t nr = t / (int32_t)dx;
            AT(info, 0x68, int16_t) = (int16_t)nr;
            uint16_t dy = AT(key, 0x49c, uint16_t);
            int32_t Bm = AT(key, 0x2d6, int16_t);
            int32_t nb = Bm < 0 ? -(((int32_t)dy - Bm - 1) / (int32_t)dy) : Bm / (int32_t)dy;
            AT(info, 0x62, int16_t) = (int16_t)nb;
            dy = AT(key, 0x49c, uint16_t);
            int32_t T = AT(key, 0x2d2, int16_t), t2 = (int32_t)dy - 1 + T;
            if (t2 < 0) t2 = T;
            AT(info, 0x66, int16_t) = (int16_t)(t2 / (int32_t)dy);
            AT(info, 0x60, uint16_t) = (uint16_t)((uint16_t)((uint16_t)nr - (uint16_t)nl) + 3) & 0xfffc;
            memcpy(key + 0x328, key + 0x2d0, 0x28);
            AT(info, 0xf4, int16_t) = AT(key, 0x32c, int16_t);
            AT(info, 0xf2, int16_t) = AT(key, 0x32e, int16_t);
            AT(info, 0xf8, int16_t) = AT(key, 0x330, int16_t);
            AT(info, 0xf6, int16_t) = AT(key, 0x32a, int16_t);
            AT(key, 0x2d0, uint16_t) = AT(info, 0x60, uint16_t);
            AT(key, 0x2d4, int16_t) = AT(info, 0x64, int16_t);
            AT(key, 0x2d8, int16_t) = AT(info, 0x68, int16_t);
            AT(key, 0x2ec, int32_t) = fit32((int64_t)((int32_t)AT(key, 0x2d2, int16_t) - (int32_t)AT(key, 0x2d6, int16_t)) *
                                            (int64_t)AT(info, 0x60, int16_t));
            AT(info, 0x20, int32_t) = AT(key, 0x344, int32_t);
        }
        if (kind != 2 && AT(key, 0x488, uint16_t)) {
            uint16_t rb = (uint16_t)((uint16_t)AT(key, 0x2d8, int16_t) - (uint16_t)AT(key, 0x2d4, int16_t) + 3) & 0xfffc;
            AT(info, 0x60, uint16_t) = rb;
            int32_t lhs = (int16_t)rb, res;
            ORIG(int32_t *(*)(int32_t *, int32_t *, int32_t), 0xc895c)( /* SafeInt<int>::operator* */
                &lhs, &res, (int32_t)AT(key, 0x2d2, int16_t) - (int32_t)AT(key, 0x2d6, int16_t));
            AT(key, 0x2ec, int32_t) = res;
            AT(info, 0x20, int32_t) = AT(key, 0x344, int32_t);
        }
        AT(key, 0x2d0, uint16_t) = AT(info, 0x60, uint16_t);
        AT(info, 0x14, int32_t) = AT(key, 0x2ec, int32_t);
        AT(info, 0x18, int32_t) = AT(key, 0x2fc, int32_t);
        AT(info, 0x1c, int32_t) = AT(key, 0x300, int32_t);
        if (!(AT(key, 0x41a, uint8_t) & 1) && kind == 3) fbms_subpixel_rect(key, info);
        return fbms_tail(in, key, info);
    }

    /* ---- embedded bitmap ---- */
    int32_t adv[2] = {0}, lsb[2] = {0}, p_c8[2] = {0}, p_b0[2] = {0}, p_c0[2] = {0}, p_b8[2] = {0};
    uint16_t rowbytes = 0;
    uint32_t size5 = 0, size6 = 0;
    int16_t s9c, sa0;
    int r = ORIG(int (*)(uint8_t *, uint8_t *, int32_t *, int32_t *, int32_t *, int32_t *, int32_t *, int32_t *, uint8_t *,
                         uint16_t *, uint32_t *, uint32_t *), 0x17c4)( /* sbit_GetMetrics */
        key + 0x3c0, key, adv, lsb, p_c8, p_b0, p_c0, p_b8, key + 0x2d2, &rowbytes, &size5, &size6);
    if (r) return r;
    AT(info, 0x48, uint32_t) = (uint32_t)adv[0] << 10;
    AT(info, 0x4c, uint32_t) = (uint32_t)adv[1] << 10;
    AT(info, 0xcc, uint32_t) = (uint32_t)p_b0[0] << 10;
    AT(info, 0xd0, uint32_t) = (uint32_t)p_b0[1] << 10;
    AT(info, 0xb8, uint32_t) = (uint32_t)p_b8[1] << 10;
    AT(info, 0xc0, uint32_t) = (uint32_t)p_b8[1] << 10;
    AT(info, 0x34, uint32_t) = (uint32_t)p_c8[1] << 10;
    AT(info, 0xd4, uint32_t) = (uint32_t)p_c0[0] << 10;
    AT(info, 0xb4, uint32_t) = (uint32_t)p_b8[0] << 10;
    AT(info, 0x3c, uint32_t) = (uint32_t)p_c8[1] << 10;
    AT(info, 0xc4, uint32_t) = (uint32_t)p_c0[0] << 10;
    AT(info, 0xbc, uint32_t) = (uint32_t)p_b8[0] << 10;
    AT(info, 0x50, uint32_t) = (uint32_t)lsb[0] << 10;
    AT(info, 0x40, uint32_t) = (uint32_t)lsb[0] << 10;
    AT(info, 0x54, uint32_t) = (uint32_t)lsb[1] << 10;
    AT(info, 0x30, uint32_t) = (uint32_t)p_c8[0] << 10;
    AT(info, 0xd8, uint32_t) = (uint32_t)p_c0[1] << 10;
    AT(info, 0x44, uint32_t) = (uint32_t)lsb[1] << 10;
    AT(info, 0x38, uint32_t) = (uint32_t)p_c8[0] << 10;
    AT(info, 0xc8, uint32_t) = (uint32_t)p_c0[1] << 10;
    r = ORIG(int (*)(uint8_t *, uint16_t, uint8_t *, uint8_t *, int16_t *, int16_t *, int16_t *, uint8_t), 0xf330)(
        key, AT(key, 0xe8, uint16_t), key + 0x1ac, key + 0x1ae, &s9c, &sa0, (int16_t *)lsb, AT(gs, 0x1bd, uint8_t));
    if (r) return r; /* sfac_ReadGlyphMetrics */
    typedef void (*scale_fn)(uint8_t *, int32_t *, uint16_t, uint8_t, uint16_t, uint8_t *);
    AT(info, 0x2c, int32_t) = 0;
    ORIG(scale_fn, 0x69868)(gs, &AT(info, 0x28, int32_t), AT(key, 0x1ac, uint16_t), AT(key, 0x19d, uint8_t),
                            AT(key, 0x174, uint16_t), key + 0x178); /* scl_ScaleAdvanceWidth */
    AT(info, 0xac, int32_t) = 0;
    ORIG(scale_fn, 0x69988)(gs, &AT(info, 0xac, int32_t), AT(key, 0x1ae, uint16_t), AT(key, 0x19d, uint8_t),
                            AT(key, 0x174, uint16_t), key + 0x178); /* scl_ScaleAdvanceHeight */
    AT(info, 0x64, int16_t) = AT(key, 0x2d4, int16_t);
    AT(info, 0x68, int16_t) = AT(key, 0x2d8, int16_t);
    AT(info, 0x62, int16_t) = AT(key, 0x2d6, int16_t);
    AT(info, 0x66, int16_t) = AT(key, 0x2d2, int16_t);
    if (rowbytes > 0x7fff) overflow();
    AT(info, 0x58, uint64_t) = 0;
    AT(info, 0x60, uint16_t) = rowbytes;
    AT(info, 0x14, uint32_t) = size5;
    AT(info, 0x1c, int32_t) = 0;
    AT(info, 0x20, int32_t) = 0;
    AT(info, 0x18, uint32_t) = size6;
    if (kind == 3) fbms_subpixel_rect(key, info);
    return fbms_tail(in, key, info);
}
