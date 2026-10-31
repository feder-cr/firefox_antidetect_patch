/*
 * Scan converter driver and contour topology (dropout control), ported from DWrite.dll
 * 10.0.26100.9711: fsc_FillGlyph walks the contours, splits them into lines and quadratic
 * splines and, for dropout control, adds the extra crossings at local extrema of a contour
 * (CheckHorizTopology / CheckVertTopology -> AddHoriz* / AddVert*).
 *
 * Calls between the functions of this file go through the ORIGINAL entry points (ORIG): when
 * the oracle swaps a function, its entry jumps to the port, otherwise the original runs, so
 * each port can be checked alone or together with the others (reimpl/oracle.py --swap).
 *
 * StateVars (WorkScan + 0x20), offsets used here:
 *   +0x00/+0x04  x, y of the point before the previous one (0x7fffffff = none yet)
 *   +0x08/+0x0c  x, y of the previous point (26.6, subpixel)
 *   +0x10/+0x14  first point of the contour where a topology check was deferred
 *   +0x30..+0x50 line/spline callbacks of the current direction, copied by Add* to +0x58..+0x68
 *   +0x70..+0x90 dropout callbacks, copied by Add* to +0x98..+0xa8
 *   +0xb0/+0xb8  horizontal / vertical crossing adders (set by fsc_SetupScan)
 *   +0xc0..+0xe8 Calc{Horiz,Vert}{Line,Sp,Ep}Subpix
 *   +0xf8/+0x100 point arrays (x, y) with count +0x10c and capacity +0x108, +0x110 a word
 *   +0x118..+0x130 work area pointers
 */
#include "tt_state.h"
#include "orig.h"

#define RVA_SafeIntOnOverflow   0x1090b0
#define RVA_fsc_SetupScan       0x6fb90
#define RVA_EvaluateSpline      0x6f0f0
#define RVA_fsc_CalcLine        0x6f640
#define RVA_fsc_FillBitMap      0x70890
#define RVA_CheckHorizTopology  0x6dc10
#define RVA_CheckVertTopology   0x6dcb8
#define RVA_AddVertOn           0x6dae8
#define RVA_AddVertOff          0xce508
#define RVA_AddHorizOff         0xd695c
#define RVA_AddHorizOn          0xd6b30

/* functions fsc_FillGlyph stores into StateVars: the originals' addresses */
#define RVA_CalcHorizLineSubpix 0xe4c30
#define RVA_CalcVertLineSubpix  0x6da50
#define RVA_CalcHorizSpSubpix   0xddd40
#define RVA_CalcVertSpSubpix    0xd91c0
#define RVA_CalcHorizEpSubpix   0xefcf0
#define RVA_CalcVertEpSubpix    0xef400

typedef int (*add_fn)(uint8_t *sv, uint16_t sc);
typedef int (*check_fn)(uint8_t *sv, int32_t x, int32_t y, uint16_t sc);
typedef int (*cross_fn)(uint8_t *sv, int32_t a, int32_t b);

#define CALL_ADD(rva, sv, sc)          ORIG(add_fn, rva)(sv, sc)
#define CALL_CHECK(rva, sv, x, y, sc)  ORIG(check_fn, rva)(sv, x, y, sc)

static inline int32_t add32(int32_t a, int32_t b) { return (int32_t)((uint32_t)a + (uint32_t)b); }

/* ---- Add{Vert,Horiz}{On,Off} ----
 * Select the callbacks for the crossing, then call the adder at +0xb0 (horizontal) or +0xb8
 * (vertical) with the previous point rounded to the pixel grid (26.6 >> 6). The originals
 * also contain an inlined copy loop over a zero-length point list (dead: count is the
 * constant 0); only its guard on the point count remains observable. */
static int add_crossing(uint8_t *sv, uint16_t sc, uint32_t s58, uint32_t s60, uint32_t s68,
                        uint32_t s98, uint32_t sa0, uint32_t sa8, uint32_t adder,
                        int32_t a, int32_t b) {
    cross_fn fn = AT(sv, adder, cross_fn);
    AT(sv, 0x58, uint64_t) = AT(sv, s58, uint64_t);
    AT(sv, 0x60, uint64_t) = AT(sv, s60, uint64_t);
    AT(sv, 0x68, uint64_t) = AT(sv, s68, uint64_t);
    if (!(sc & 2)) {
        AT(sv, 0x98, uint64_t) = AT(sv, s98, uint64_t);
        AT(sv, 0xa0, uint64_t) = AT(sv, sa0, uint64_t);
        AT(sv, 0xa8, uint64_t) = AT(sv, sa8, uint64_t);
        if (sc & 4) {
            int32_t n = AT(sv, 0x10c, int32_t);
            if (add32(n, -1) > 0x3fff) return 0x1305;
            AT(sv, 0x110, uint16_t) = (uint16_t)((uint16_t)((uint16_t)n - 1) << 2 | 2);
        }
    }
    return fn(sv, a, b);
}

TT_EXPORT int tt_AddVertOn(uint8_t *sv, uint16_t sc) {  /* AddVertOn */
    return add_crossing(sv, sc, 0x30, 0x40, 0x38, 0x70, 0x80, 0x78, 0xb8,
                        AT(sv, 0x8, int32_t) >> 6, add32(AT(sv, 0xc, int32_t), 0x1f) >> 6);
}

TT_EXPORT int tt_AddVertOff(uint8_t *sv, uint16_t sc) {  /* AddVertOff */
    return add_crossing(sv, sc, 0x30, 0x40, 0x38, 0x78, 0x88, 0x90, 0xb8,
                        AT(sv, 0x8, int32_t) >> 6, add32(AT(sv, 0xc, int32_t), 0x20) >> 6);
}

TT_EXPORT int tt_AddHorizOff(uint8_t *sv, uint16_t sc) {  /* AddHorizOff */
    return add_crossing(sv, sc, 0x38, 0x48, 0x50, 0x78, 0x88, 0x90, 0xb0,
                        add32(AT(sv, 0x8, int32_t), 0x20) >> 6, AT(sv, 0xc, int32_t) >> 6);
}

TT_EXPORT int tt_AddHorizOn(uint8_t *sv, uint16_t sc) {  /* AddHorizOn */
    return add_crossing(sv, sc, 0x30, 0x40, 0x38, 0x78, 0x88, 0x90, 0xb0,
                        add32(AT(sv, 0x8, int32_t), 0x1f) >> 6, AT(sv, 0xc, int32_t) >> 6);
}

/* ---- topology checks ----
 * (x0,y0) = +0x00/+0x04, (x1,y1) = +0x08/+0x0c, (x, y) = the next point: the previous point
 * is a local extremum of the contour when the path turns there. */

/* CheckHorizTopology: extremum in y at the previous point */
TT_EXPORT int tt_CheckHorizTopology(uint8_t *sv, int32_t x, int32_t y, uint16_t sc) {
    int32_t x0 = AT(sv, 0x0, int32_t), y0 = AT(sv, 0x4, int32_t);
    int32_t x1 = AT(sv, 0x8, int32_t), y1 = AT(sv, 0xc, int32_t);
    int r;
    if (y > y1) {
        if (y1 > y0) return CALL_ADD(RVA_AddHorizOn, sv, sc);
        if (y1 == y0) return x1 < x0 ? CALL_ADD(RVA_AddHorizOn, sv, sc) : 0;
        goto on_off;
    }
    if (y < y1) {
        if (y1 > y0) goto on_off;
        if (y1 == y0) return x1 > x0 ? CALL_ADD(RVA_AddHorizOff, sv, sc) : 0;
        return CALL_ADD(RVA_AddHorizOff, sv, sc);
    }
    /* y == y1 */
    if (y1 > y0) return x > x1 ? CALL_ADD(RVA_AddHorizOn, sv, sc) : 0;
    if (y1 == y0) {
        if (x1 == x0) return 0;
        if (x1 < x0) return x > x1 ? CALL_ADD(RVA_AddHorizOn, sv, sc) : 0;
    }
    return x < x1 ? CALL_ADD(RVA_AddHorizOff, sv, sc) : 0;
on_off:
    r = CALL_ADD(RVA_AddHorizOn, sv, sc);
    if (r) return r;
    return CALL_ADD(RVA_AddHorizOff, sv, sc);
}

/* CheckVertTopology: extremum in x at the previous point */
TT_EXPORT int tt_CheckVertTopology(uint8_t *sv, int32_t x, int32_t y, uint16_t sc) {
    int32_t x0 = AT(sv, 0x0, int32_t), y0 = AT(sv, 0x4, int32_t);
    int32_t x1 = AT(sv, 0x8, int32_t), y1 = AT(sv, 0xc, int32_t);
    int r;
    if (x < x1) {
        if (x1 < x0) return CALL_ADD(RVA_AddVertOn, sv, sc);
        if (x1 == x0) return y1 < y0 ? CALL_ADD(RVA_AddVertOn, sv, sc) : 0;
        goto on_off;
    }
    if (x > x1) {
        if (x1 < x0) goto on_off;
        if (x1 == x0) return y1 > y0 ? CALL_ADD(RVA_AddVertOff, sv, sc) : 0;
        return CALL_ADD(RVA_AddVertOff, sv, sc);
    }
    /* x == x1 */
    if (x1 < x0) return y > y1 ? CALL_ADD(RVA_AddVertOn, sv, sc) : 0;
    if (x1 == x0) {
        if (y1 == y0) return 0;
        if (y1 < y0) return y > y1 ? CALL_ADD(RVA_AddVertOn, sv, sc) : 0;
    }
    return y < y1 ? CALL_ADD(RVA_AddVertOff, sv, sc) : 0;
on_off:
    r = CALL_ADD(RVA_AddVertOn, sv, sc);
    if (r) return r;
    return CALL_ADD(RVA_AddVertOff, sv, sc);
}

/* ---- fsc_FillGlyph ---- */

static void overflow(void) { ORIG(void (*)(void), RVA_SafeIntOnOverflow)(); }

/* SafeInt<pointer> + int: the original checks |v| against the pointer range */
static uint8_t *safe_ptr_add(uint8_t *p, int64_t v) {
    uintptr_t base = (uintptr_t)p, r;
    if (v < 0) {
        uint64_t m = (uint64_t)0 - (uint64_t)v;
        if (m > base) overflow();
        r = base - m;
    } else {
        r = base + (uint64_t)v;
        if (r < base) overflow();
    }
    return (uint8_t *)r;
}

/* vertical topology check inlined in fsc_FillGlyph for an on-curve point (x2, y2): same
 * decisions as CheckVertTopology, with the "no previous point yet" case */
static int vert_topology(uint8_t *sv, int32_t x2, int32_t y2, uint16_t sc) {
    int32_t x0 = AT(sv, 0x0, int32_t), y0 = AT(sv, 0x4, int32_t);
    int32_t x1 = AT(sv, 0x8, int32_t), y1 = AT(sv, 0xc, int32_t);
    int r;
    if (x0 == 0x7fffffff) {  /* first point of the contour: check it at the end */
        AT(sv, 0x10, int32_t) = x2;
        AT(sv, 0x14, int32_t) = y2;
        return 0;
    }
    if (x2 < x1) {
        if (x1 < x0) return CALL_ADD(RVA_AddVertOn, sv, sc);
        if (x1 == x0) return y1 < y0 ? CALL_ADD(RVA_AddVertOn, sv, sc) : 0;
        goto on_off;
    }
    if (x2 > x1) {
        if (x1 < x0) goto on_off;
        if (x1 == x0) return y1 > y0 ? CALL_ADD(RVA_AddVertOff, sv, sc) : 0;
        return CALL_ADD(RVA_AddVertOff, sv, sc);
    }
    if (x1 < x0) return y2 > y1 ? CALL_ADD(RVA_AddVertOn, sv, sc) : 0;
    if (x1 == x0) {
        if (y1 == y0) return 0;
        if (y1 < y0) return y2 > y1 ? CALL_ADD(RVA_AddVertOn, sv, sc) : 0;
    }
    return y2 < y1 ? CALL_ADD(RVA_AddVertOff, sv, sc) : 0;
on_off:
    r = CALL_ADD(RVA_AddVertOn, sv, sc);
    if (r) return r;
    return CALL_ADD(RVA_AddVertOff, sv, sc);
}

static inline int is_center(int32_t v) { return (v & 0x3f) == 0x20; }  /* on a pixel center */

/*
 * ContourList: +0 ushort count, +8 short *sp, +0x10 short *ep, +0x18 int *x, +0x20 int *y,
 *              +0x28 uint8 *onCurve
 * GlyphBitMap: +0 short (fsc_FillBitMap), +2 rect (shorts; +2 > +6 or empty), +0xa byte
 *              (nonzero clears scanControl bit 0), +0x20 bitmap
 * WorkScan:    +4 int, +8 int, +0xc/+0x10/+0x14 ints and +0x18 pointer for fsc_SetupScan,
 *              +0x20 StateVars *, +0x28 pointer
 */
TT_EXPORT int tt_fsc_FillGlyph(uint8_t *cl, uint8_t *gbm, uint8_t *ws, uint16_t scanControl, uint8_t reverse) {
    uint8_t *sv = AT(ws, 0x20, uint8_t *);
    if (AT(gbm, 2, int16_t) <= AT(gbm, 6, int16_t)) return 0;
    uint16_t sc = AT(gbm, 0xa, uint8_t) ? (uint16_t)(scanControl & 0xfffe) : scanControl;

    /* work area pointers, with the original's SafeInt checks */
    int64_t v = (int64_t)AT(ws, 4, int32_t) - 0x138;
    if ((uint64_t)(v + 0x80000000LL) > 0xffffffffULL) overflow();  /* int32 result */
    uint8_t *base = sv + 0x138;
    if ((uintptr_t)base < (uintptr_t)sv) overflow();
    AT(sv, 0x118, uint8_t *) = base;
    uint8_t *p128 = safe_ptr_add(base, (int32_t)v);
    AT(sv, 0x128, uint8_t *) = p128;
    uint8_t *w28 = AT(ws, 0x28, uint8_t *);
    AT(sv, 0x120, uint8_t *) = w28;
    AT(sv, 0x130, uint8_t *) = safe_ptr_add(w28, AT(ws, 8, int32_t));

    AT(sv, 0xc0, void *) = ORIG(void *, RVA_CalcHorizLineSubpix);
    AT(sv, 0xd8, void *) = ORIG(void *, RVA_CalcVertLineSubpix);
    AT(sv, 0xc8, void *) = ORIG(void *, RVA_CalcHorizSpSubpix);
    AT(sv, 0xe0, void *) = ORIG(void *, RVA_CalcVertSpSubpix);
    AT(sv, 0xd0, void *) = ORIG(void *, RVA_CalcHorizEpSubpix);
    AT(sv, 0xe8, void *) = ORIG(void *, RVA_CalcVertEpSubpix);

    int r = ORIG(int (*)(uint8_t *, void *, uint16_t, uint8_t, int32_t, int32_t, int32_t, void *),
                 RVA_fsc_SetupScan)(sv, gbm + 2, sc, reverse, AT(ws, 0xc, int32_t),
                                    AT(ws, 0x10, int32_t), AT(ws, 0x14, int32_t), AT(ws, 0x18, void *));
    if (r) return r;

    int (*spline)(uint8_t *, int32_t, int32_t, int32_t, int32_t, int32_t, int32_t, uint16_t) =
        ORIG(int (*)(uint8_t *, int32_t, int32_t, int32_t, int32_t, int32_t, int32_t, uint16_t), RVA_EvaluateSpline);
    int (*line)(uint8_t *, int32_t, int32_t, int32_t, int32_t, uint16_t) =
        ORIG(int (*)(uint8_t *, int32_t, int32_t, int32_t, int32_t, uint16_t), RVA_fsc_CalcLine);

    for (uint16_t ci = 0; ci < AT(cl, 0, uint16_t); ci++) {
        int64_t sp = AT(cl, 8, int16_t *)[ci], ep = AT(cl, 0x10, int16_t *)[ci];
        if ((int16_t)sp == (int16_t)ep) continue;  /* single point: nothing to fill */
        const int32_t *X = AT(cl, 0x18, int32_t *), *Y = AT(cl, 0x20, int32_t *);
        const uint8_t *ON = AT(cl, 0x28, uint8_t *);
        int64_t c, e;            /* current point index, end index (exclusive) */
        int32_t px, py;          /* previous on-curve point (segment start) */
        int32_t cx, cy;          /* current point */
        uint8_t con;             /* current point's flags */
        if (ON[ep] & 1) {        /* start at the last point */
            px = X[ep]; py = Y[ep];
            e = ep + 1;
            c = sp; cx = X[sp]; cy = Y[sp]; con = ON[sp];
        } else {                 /* last point off-curve: it is the current point, at sp - 1 */
            px = X[ep - 1]; py = Y[ep - 1];
            cy = Y[ep]; cx = X[ep];
            if (!(ON[ep - 1] & 1)) {
                px = add32(add32(px, 1), cx) >> 1;
                py = add32(add32(py, 1), cy) >> 1;
            }
            con = 0;
            e = ep;
            c = sp - 1;
        }
        AT(sv, 0x8, int32_t) = px;
        AT(sv, 0xc, int32_t) = py;
        AT(sv, 0x0, int32_t) = 0x7fffffff;
        if ((sc & 6) == 4) {     /* record the contour start in the point list */
            AT(sv, 0xf8, int32_t *)[AT(sv, 0x10c, int32_t)] = px;
            AT(sv, 0x100, int32_t *)[AT(sv, 0x10c, int32_t)] = py;
            AT(sv, 0x10c, int32_t)++;
            if (AT(sv, 0x10c, int32_t) > AT(sv, 0x108, int32_t)) return 0x1a00;
        }
        while (c < e) {
            int32_t nx, ny;
            if (!(con & 1)) {    /* spline: (px,py) control (cx,cy) end (nx,ny) */
                c++;
                nx = X[c]; ny = Y[c];
                if (!(ON[c] & 1)) {  /* two off-curve points: implied midpoint */
                    nx = add32(add32(nx, 1), cx) >> 1;
                    ny = add32(add32(ny, 1), cy) >> 1;
                } else {
                    c++;
                }
                r = spline(sv, px, py, cx, cy, nx, ny, sc);
                if (r) return r;
            } else {             /* line to (cx, cy) */
                if (is_center(AT(sv, 0xc, int32_t))) {
                    if (AT(sv, 0x8, int32_t) == cx && AT(sv, 0xc, int32_t) == cy) goto calc_line;
                    if (AT(sv, 0x0, int32_t) == 0x7fffffff) {
                        AT(sv, 0x10, int32_t) = cx;
                        AT(sv, 0x14, int32_t) = cy;
                    } else {
                        r = CALL_CHECK(RVA_CheckHorizTopology, sv, cx, cy, sc);
                        if (r) return r;
                    }
                }
                if (!(sc & 2) && is_center(AT(sv, 0x8, int32_t))) {
                    if (AT(sv, 0x8, int32_t) == cx && AT(sv, 0xc, int32_t) == cy) goto calc_line;
                    r = vert_topology(sv, cx, cy, sc);
                    if (r) return r;
                }
                AT(sv, 0x0, int32_t) = AT(sv, 0x8, int32_t);
                AT(sv, 0x4, int32_t) = AT(sv, 0xc, int32_t);
                AT(sv, 0xc, int32_t) = cy;
                AT(sv, 0x8, int32_t) = cx;
            calc_line:
                r = line(sv, px, py, cx, cy, sc);
                if (r) return r;
                c++;
                nx = cx; ny = cy;
            }
            px = nx; py = ny;
            if (c == e) break;
            cx = X[c]; cy = Y[c]; con = ON[c];
        }
        /* close the contour: check the deferred first point */
        if (is_center(AT(sv, 0xc, int32_t))) {
            r = CALL_CHECK(RVA_CheckHorizTopology, sv, AT(sv, 0x10, int32_t), AT(sv, 0x14, int32_t), sc);
            if (r) return r;
        }
        if (!(sc & 2) && is_center(AT(sv, 0x8, int32_t))) {
            r = CALL_CHECK(RVA_CheckVertTopology, sv, AT(sv, 0x10, int32_t), AT(sv, 0x14, int32_t), sc);
            if (r) return r;
        }
    }
    return ORIG(int (*)(uint8_t *, void *, int32_t, uint16_t), RVA_fsc_FillBitMap)(
        sv, AT(gbm, 0x20, void *), AT(gbm, 0, int16_t), sc);
}
