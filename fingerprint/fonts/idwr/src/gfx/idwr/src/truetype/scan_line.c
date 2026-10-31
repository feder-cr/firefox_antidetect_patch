/*
 * Scan converter, line rasterization and edge insertion (DWrite.dll 10.0.26100.9711):
 *   fsc_CalcLine          0x18006f640  walks a line segment, adds one crossing per scanline
 *   AddHoriz/VertSimpleScan 0x1800965e0 / 0x180095d90  insert a crossing (int16 lists)
 *   AddHoriz/VertSmartScan  0x1800a5570 / 0x1800a53d0  same, with a point tag (smart dropout)
 *   CalcHoriz/VertLineSubpix 0x1800e4c30 / 0x18006da50 line intercept for smart dropout
 *
 * StateVars (sv) fields used, by offset:
 *   0x18/0x1c  x range [lo, hi)  (inclusive hi for AddHoriz*)
 *   0x24/0x20  y range [lo, hi]  (exclusive hi for AddHoriz*)
 *   0x30..0x50 horizontal-scan list tables (start/end/limit per scanline, two orientations)
 *   0x58/0x60/0x68  current horizontal tables: start, end (write cursor), limit; indexed by y-lo
 *   0x70..0x90 vertical-scan list tables; 0x98/0xa0/0xa8 current ones, indexed by x-lo
 *   0xb0  AddHoriz*Scan (crossing of a horizontal scanline: stores x)
 *   0xb8  AddVert*Scan  (crossing of a vertical scanline: stores y)
 *   0xf8/0x100 point x/y arrays for smart dropout, 0x108 their limit, 0x10c count
 *   0x110 tag stored with every smart crossing: (point index << 2) | kind (0 = line)
 *
 * Coordinates are 26.6; scanlines sit at pixel centres (k * 64 + 32).
 */
#include "tt_state.h"
#include "orig.h"

#define U32(x) ((uint32_t)(x))
#define I32(x) ((int32_t)(x))

typedef int (*add_scan_fn)(uint8_t *sv, int x, int y);

/* SafeIntExceptionHandler<SafeIntRasterizerException>::SafeIntOnOverflow: throws */
#define SAFEINT_OVERFLOW() ORIG(void (*)(void), 0x1090b0)()

/* CompDiv 0x18006da90: rounded int64 / int32 division (not ported) */
#define COMPDIV(d, n) ORIG(int64_t (*)(int, int64_t), 0x6da90)(d, n)

/* ------------------------------------------------------------------------------------------ */

/* fsc_CalcLine: flags bit 2 = no dropout control (only horizontal-scanline crossings),
 * bit 4 = smart dropout (records the end point and tags the crossings). */
TT_EXPORT int tt_fsc_CalcLine(uint8_t *sv, int x1, int y1, int x2, int y2, uint16_t flags)
{
    int ystep, ybias, ycell, yfrac, dy, ycount, quad, err0;
    int xstep, xbias, xcell, xfrac, dx, xcount, err;
    uint32_t t;

    /* first horizontal scanline at or beyond y1 in the direction of travel */
    if (y2 < y1) {
        t = ((U32(y1) - 0x21) & 0xffffffc0u) + 0x20;
        yfrac = I32(U32(y1) - t);
        ycell = I32(t) >> 6;
        dy = I32(U32(y1) - U32(y2));
        ycount = I32(U32(ycell) - U32(I32(U32(y2) + 0x20) >> 6));
        ystep = -1; ybias = 1; quad = 4; err0 = 1;
    } else {
        t = ((U32(y1) + 0x20) & 0xffffffc0u) + 0x20;
        yfrac = I32(t - U32(y1));
        ycell = I32(t) >> 6;
        dy = I32(U32(y2) - U32(y1));
        ycount = I32(U32(I32(U32(y2) - 0x21) >> 6) - U32(ycell));
        ystep = 1; ybias = 0; quad = 1; err0 = 0;
    }
    ycount = I32(U32(ycount) + 1);
    if (y2 == y1) {
        if (flags & 2)
            return 0;
        ycell = I32(U32(x2 >= x1 ? y1 : I32(U32(y1) - 1)) + 0x20) >> 6;
        ycount = 0;
    }

    if (x2 < x1) {
        t = ((U32(x1) - 0x21) & 0xffffffc0u) + 0x20;
        xbias = 1;
        err = 1 - err0;
        xfrac = I32(U32(x1) - t);
        xcell = I32(t) >> 6;
        dx = I32(U32(x1) - U32(x2));
        quad = (quad != 1) + 2;
        xcount = I32(U32(xcell) - U32(I32(U32(x2) + 0x20) >> 6));
        xstep = -1;
    } else {
        t = ((U32(x1) + 0x20) & 0xffffffc0u) + 0x20;
        xbias = 0;
        dx = I32(U32(x2) - U32(x1));
        xfrac = I32(t - U32(x1));
        xcell = I32(t) >> 6;
        xcount = I32(U32(I32(U32(x2) - 0x21) >> 6) - U32(xcell));
        xstep = 1;
        err = err0;
    }
    xcount = I32(U32(xcount) + 1);
    if (x2 == x1) {
        xcell = I32(U32(y2 > y1 ? I32(U32(x1) - 1) : x1) + 0x20) >> 6;
        xcount = 0;
    }

    add_scan_fn horiz = AT(sv, 0xb0, add_scan_fn);
    add_scan_fn vert = AT(sv, 0xb8, add_scan_fn);

    /* select the list tables by quadrant (1: +x+y, 2: -x+y, 3: -x-y, 4: +x-y) */
    if (quad == 1 || quad == 2) {
        AT(sv, 0x58, void *) = AT(sv, 0x30, void *);
        AT(sv, 0x60, void *) = AT(sv, 0x40, void *);
        AT(sv, 0x68, void *) = AT(sv, 0x38, void *);
    } else {
        AT(sv, 0x58, void *) = AT(sv, 0x38, void *);
        AT(sv, 0x60, void *) = AT(sv, 0x48, void *);
        AT(sv, 0x68, void *) = AT(sv, 0x50, void *);
    }

    int i = 0, r;
    if (flags & 2) {
        /* no dropout control: only horizontal-scanline crossings (y1 != y2 here) */
        if (x1 == x2) {
            for (; i < ycount; i++) {
                if ((r = horiz(sv, xcell, ycell)) != 0)
                    return r;
                ycell = I32(U32(ycell) + U32(ystep));
            }
            return 0;
        }
        err = I32(U32(err) + (U32(dx) * U32(yfrac) - U32(dy) * U32(xfrac)));
        xcell = I32(U32(xcell) + U32(xbias));
        int total = I32(U32(xcount) + U32(ycount));
        uint32_t xinc = U32(-(int64_t)dy) << 6, yinc = U32(dx) << 6;
        for (; i < total; i++) {
            if (err <= 0) {
                if ((r = horiz(sv, xcell, ycell)) != 0)
                    return r;
                ycell = I32(U32(ycell) + U32(ystep));
                err = I32(U32(err) + yinc);
            } else {
                xcell = I32(U32(xcell) + U32(xstep));
                err = I32(U32(err) + xinc);
            }
        }
        return 0;
    }

    if (quad == 2 || quad == 3) {
        AT(sv, 0x98, void *) = AT(sv, 0x70, void *);
        AT(sv, 0xa0, void *) = AT(sv, 0x80, void *);
        AT(sv, 0xa8, void *) = AT(sv, 0x78, void *);
    } else {
        AT(sv, 0x98, void *) = AT(sv, 0x78, void *);
        AT(sv, 0xa0, void *) = AT(sv, 0x88, void *);
        AT(sv, 0xa8, void *) = AT(sv, 0x90, void *);
    }

    if (flags & 4) {
        /* smart dropout: record the end point; crossings get tag (start index << 2) | 0 */
        int n = AT(sv, 0x10c, int32_t);
        if (I32(U32(n) - 1) > 0x3fff)
            return 0x1305;
        AT(sv, 0x110, uint16_t) = (uint16_t)((U32(n) - 1) << 2);
        AT(sv, 0xf8, int32_t *)[n] = x2;
        AT(sv, 0x100, int32_t *)[AT(sv, 0x10c, int32_t)] = y2;
        n = I32(U32(AT(sv, 0x10c, int32_t)) + 1);
        AT(sv, 0x10c, int32_t) = n;
        if (n > AT(sv, 0x108, int32_t))
            return 0x1a00;
    }

    if (y1 == y2) {
        for (; i < xcount; i++) {
            if ((r = vert(sv, xcell, ycell)) != 0)
                return r;
            xcell = I32(U32(xcell) + U32(xstep));
        }
        return 0;
    }
    if (x1 == x2) {
        for (; i < ycount; i++) {
            if ((r = horiz(sv, xcell, ycell)) != 0)
                return r;
            ycell = I32(U32(ycell) + U32(ystep));
        }
        return 0;
    }
    /* err > 0: the next crossing is a vertical scanline, else a horizontal one */
    err = I32(U32(err) + (U32(dx) * U32(yfrac) - U32(dy) * U32(xfrac)));
    int total = I32(U32(ycount) + U32(xcount));
    uint32_t xinc = U32(-(int64_t)dy) << 6, yinc = U32(dx) << 6;
    for (; i < total; i++) {
        if (err > 0) {
            if ((r = vert(sv, xcell, I32(U32(ycell) + U32(ybias)))) != 0)
                return r;
            xcell = I32(U32(xcell) + U32(xstep));
            err = I32(U32(err) + xinc);
        } else {
            if ((r = horiz(sv, I32(U32(xbias) + U32(xcell)), ycell)) != 0)
                return r;
            ycell = I32(U32(ycell) + U32(ystep));
            err = I32(U32(err) + yinc);
        }
    }
    return 0;
}

/* ------------------------------------------------------------------------------------------ */

/* CalcHorizLineSubpix: x where the line xs[0..1], ys[0..1] crosses the scanline y (26.6) */
TT_EXPORT int tt_CalcHorizLineSubpix(int y, const int32_t *xs, const int32_t *ys)
{
    int32_t x0 = xs[0];
    int64_t n = (int64_t)I32((U32(y) << 6) - U32(ys[0]) + 0x20) * (int64_t)I32(U32(xs[1]) - U32(x0));
    int32_t q = (int32_t)COMPDIV(I32(U32(ys[1]) - U32(ys[0])), n);
    return I32(U32(q) + U32(x0));
}

/* CalcVertLineSubpix: y where the line crosses the vertical scanline x */
TT_EXPORT int tt_CalcVertLineSubpix(int x, const int32_t *xs, const int32_t *ys)
{
    int32_t y0 = ys[0];
    int64_t n = (int64_t)I32((U32(x) << 6) - U32(xs[0]) + 0x20) * (int64_t)I32(U32(ys[1]) - U32(y0));
    int32_t q = (int32_t)COMPDIV(I32(U32(xs[1]) - U32(xs[0])), n);
    return I32(U32(q) + U32(y0));
}

/* ------------------------------------------------------------------------------------------ */

/* Insertion of one crossing into the sorted list of scanline `line - lo`. The write cursor is
 * advanced before the limit check, so on overflow (0x1302) it stays advanced, as in the
 * original. `size` is 2 (Simple: int16 coordinate) or 4 (Smart: int16 coordinate + uint16 tag);
 * the sort key is the signed int16 coordinate. */
static int insert_scan(uint8_t *sv, int line, int lo, int tables, int16_t v, int size)
{
    int64_t d = (int64_t)line - lo;
    if (d > 0x7fffffff || d < -0x80000000LL)
        SAFEINT_OVERFLOW();
    int64_t o = (int64_t)(int32_t)d * 8;
    uintptr_t start = *(uintptr_t *)(AT(sv, tables, uint8_t *) + o);
    uintptr_t *endp = (uintptr_t *)(AT(sv, tables + 8, uint8_t *) + o);
    uintptr_t p = *endp;
    *endp = p + size;
    if (p + size > *(uintptr_t *)(AT(sv, tables + 0x10, uint8_t *) + o))
        return 0x1302;
    uintptr_t q = p - size;
    while (q >= start && *(int16_t *)q > v) {
        if (size == 2)
            *(int16_t *)p = *(int16_t *)q;
        else
            *(uint32_t *)p = *(uint32_t *)q;
        q -= size;
        p -= size;
    }
    *(int16_t *)p = v;
    if (size == 4)
        *(uint16_t *)(p + 2) = AT(sv, 0x110, uint16_t);
    return 0;
}

/* AddVertSimpleScan: crossing of vertical scanline x at y; x in [lo, hi), y in [lo, hi] */
TT_EXPORT int tt_AddVertSimpleScan(uint8_t *sv, int x, int y)
{
    if (x < AT(sv, 0x18, int32_t) || x >= AT(sv, 0x1c, int32_t) ||
        y < AT(sv, 0x24, int32_t) || y > AT(sv, 0x20, int32_t))
        return 0x1302;
    return insert_scan(sv, x, AT(sv, 0x18, int32_t), 0x98, (int16_t)y, 2);
}

/* AddHorizSimpleScan: crossing of horizontal scanline y at x; x in [lo, hi], y in [lo, hi) */
TT_EXPORT int tt_AddHorizSimpleScan(uint8_t *sv, int x, int y)
{
    if (x < AT(sv, 0x18, int32_t) || x > AT(sv, 0x1c, int32_t) ||
        y < AT(sv, 0x24, int32_t) || y >= AT(sv, 0x20, int32_t))
        return 0x1302;
    return insert_scan(sv, y, AT(sv, 0x24, int32_t), 0x58, (int16_t)x, 2);
}

/* AddVertSmartScan: as AddVertSimpleScan, entries carry the tag at sv+0x110 */
TT_EXPORT int tt_AddVertSmartScan(uint8_t *sv, int x, int y)
{
    if (x < AT(sv, 0x18, int32_t) || x >= AT(sv, 0x1c, int32_t) ||
        y < AT(sv, 0x24, int32_t) || y > AT(sv, 0x20, int32_t))
        return 0x1302;
    return insert_scan(sv, x, AT(sv, 0x18, int32_t), 0x98, (int16_t)y, 4);
}

/* AddHorizSmartScan: as AddHorizSimpleScan, entries carry the tag at sv+0x110 */
TT_EXPORT int tt_AddHorizSmartScan(uint8_t *sv, int x, int y)
{
    if (x < AT(sv, 0x18, int32_t) || x > AT(sv, 0x1c, int32_t) ||
        y < AT(sv, 0x24, int32_t) || y >= AT(sv, 0x20, int32_t))
        return 0x1302;
    return insert_scan(sv, y, AT(sv, 0x24, int32_t), 0x58, (int16_t)x, 4);
}
