/*
 * The embedded-bitmap (EBLC/EBDT) search and metrics of DWrite.dll 10.0.26100.9711's TrueType
 * scaler, ported to C from the REA assembly (authoritative) and the Ghidra decompilation:
 *
 *   sbit_SearchForBitmap  0x6620   strike for the transform's ppem, then the glyph in it
 *   sfac_SearchForStrike  0x7dc4   EBLC BitmapSize (FindBlocStrike 0x7998), else EBSC
 *                                  (FindBscaStrike 0x14d0c4) and its substitute strike
 *   sfac_SearchForBitmap  0x6730   the index subtable (formats 1-5) and the image data
 *   GetSbitMetrics        0x1f8c   the strike glyph's metrics, hmtx/vmtx substitutes, the
 *                                  vertical bearing x, sfac_ShaveSbitMetrics
 *   sfac_ShaveSbitMetrics 0x3f70   image format 5: blank rows and columns cut off
 *   sbit_GetMetrics       0x17c4   device metrics and the bitmap rect, by rotation code
 *   SScaleY               0x2438, SEmScaleX 0x2670
 *
 * fs_NewGlyph calls sbit_SearchForBitmap when input + 0x95 is 0 (RasterizeInternal sets it to
 * !(flags & 0x40)); fs_FindBitMapSize calls sbit_GetMetrics for a strike glyph. The sbit
 * state is client + 0x3c0:
 *   +0x00 BitmapSize offset in EBLC   +0x04 metrics offset   +0x08 image data offset in EBDT
 *   +0x0c image data size   +0x10 / +0x14 bitmap sizes   +0x18 strike kind (0 unknown, 1 none,
 *   2 EBLC, 3 EBSC-scaled)   +0x1a/+0x1c ppem x/y   +0x1e/+0x20 strike ppem x/y
 *   +0x22 rotation code   +0x24 metrics layout (1 hor small, 2 vert small, 3 big)
 *   +0x26 metrics table (1 EBLC, 2 EBDT)   +0x28 image format   +0x2a/+0x2c height/width
 *   +0x2e/+0x30 advance width/height   +0x32/+0x38/+0x3a row bytes   +0x34/+0x36 scaled
 *   height/width   +0x3c..+0x42 shaved left/right/top/bottom   +0x44/+0x46 hor bearing x/y
 *   +0x48/+0x4a vert bearing x/y   +0x4c found   +0x4d metrics read
 *
 * SafeInt checks call the original SafeIntOnOverflow (native.c in standalone builds), which
 * no valid font reaches. OutputDebugStringA messages are left out: nothing observable.
 */
#include <stdint.h>
#include <string.h>
#include "tt_state.h"
#include "orig.h"

#define RVA_SafeIntOverflow      0x1090b0
#define RVA_UScaleX              0x24f8
#define RVA_SScaleX              0x25b0
#define RVA_UScaleY              0x23a4
#define RVA_UEmScaleX            0x3ed8
#define RVA_SScaleY              0x2438
#define RVA_SEmScaleX            0x2670
#define RVA_sfac_GetSbitMetrics  0x2190
#define RVA_SubstituteHorMetrics 0x10b6cc
#define RVA_SubstituteVertMetrics 0x4ccc
#define RVA_sfac_ReadGlyphHorMetrics 0xf5b0
#define RVA_sfac_ShaveSbitMetrics 0x3f70
#define RVA_GetSbitMetrics       0x1f8c
#define RVA_sfac_SearchForStrike 0x7dc4
#define RVA_sfac_SearchForBitmap 0x6730
#define RVA_FindBlocStrike       0x7998
#define RVA_FindBscaStrike       0x14d0c4

static void of(void) { ORIG(void (*)(void), RVA_SafeIntOverflow)(); }

typedef uint8_t *(*getfrag_t)(void *, int, int, void **);
typedef void (*relfrag_t)(void *);
#define GETFRAG(cl, off, len, ctx) (AT(cl, 8, getfrag_t)(AT(cl, 0, void *), (int)(off), (int)(len), (ctx)))
#define RELFRAG(cl, ctx) (AT(cl, 0x10, relfrag_t)(ctx))

typedef uint16_t *(*uscale_t)(uint16_t *, uint8_t *, uint32_t);
typedef uint16_t *(*sscale_t)(uint16_t *, uint8_t *, int16_t);

static uint16_t be16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }
static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

/* checked uint32 a + b (SafeInt<unsigned int>) */
static uint32_t add32(uint32_t a, uint32_t b) {
    uint64_t r = (uint64_t)a + b;
    if (r > 0xffffffffu) of();
    return (uint32_t)r;
}

/* ============================ scaling ============================ */

/* SScaleY 0x2438: SScaleX with UScaleY: the signed value scaled under an EBSC strike, else as is */
TT_EXPORT uint16_t *tt_SScaleY(uint16_t *out, uint8_t *st, int16_t val) {
    if (AT(st, 0x18, int16_t) == 3) {
        uint16_t sc;
        if (val < 0) {
            if (val == (int16_t)0x8000) of();
            ORIG(uscale_t, RVA_UScaleY)(&sc, st, (uint16_t)(-(int)val));
            int neg = -(int)(uint32_t)sc;
            if ((uint32_t)(neg + 0x8000) >= 0x10000) of();
            *out = (uint16_t)(int16_t)neg;
        } else {
            ORIG(uscale_t, RVA_UScaleY)(&sc, st, (uint16_t)val);
            if (sc >= 0x8000) of();
            *out = sc;
        }
    } else {
        *out = (uint16_t)val;
    }
    return out;
}

/* SEmScaleX 0x2670: a signed value in font units to the strike's pixels in x (UEmScaleX) */
TT_EXPORT uint16_t *tt_SEmScaleX(uint16_t *out, uint8_t *st, int16_t val) {
    uint16_t sc;
    if (val < 0) {
        if (val == (int16_t)0x8000) of();
        ORIG(uscale_t, RVA_UEmScaleX)(&sc, st, (uint16_t)(-(int)val));
        int neg = -(int)(uint32_t)sc;
        if ((uint32_t)(neg + 0x8000) >= 0x10000) of();
        *out = (uint16_t)(int16_t)neg;
    } else {
        ORIG(uscale_t, RVA_UEmScaleX)(&sc, st, (uint16_t)val);
        if (sc >= 0x8000) of();
        *out = sc;
    }
    return out;
}

/* ============================ strike search ============================ */

/* FindBlocStrike 0x7998: the EBLC BitmapSize with ppemX, ppemY and bitDepth 1; its offset */
TT_EXPORT uint8_t tt_FindBlocStrike(const uint8_t *p, uint32_t len, uint16_t ppx, uint16_t ppy, uint32_t *off) {
    if (len < 8) return 0;   /* "sfntaccs.c, FindBlocStrike, offset out of bounds" */
    uint32_t n = be32(p + 4);
    if (n > (len - 8) / 0x30) return 0;
    uint64_t pos = 8;
    for (; n; n--, pos += 0x30) {
        if (pos + 0x2e > 0xffffffffu) of();
        if (ppx == p[pos + 0x2c] && ppy == p[pos + 0x2d] && p[pos + 0x2e] == 1) {
            *off = (uint32_t)pos;
            return 1;
        }
        if (pos + 0x30 > 0xffffffffu) of();
    }
    return 0;
}

/* FindBscaStrike 0x14d0c4: the EBSC BitmapScale with ppemX, ppemY; its offset */
TT_EXPORT uint8_t tt_FindBscaStrike(const uint8_t *p, uint32_t len, uint16_t ppx, uint16_t ppy, uint32_t *off) {
    if (len < 8) return 0;   /* "sfntaccs.c, FindBlocStrike, offset out of bounds" */
#ifdef IDWR_NEGATIVE_CONTROL_EBSC
    return 0;   /* sbit_metrics_check.py --control ebsc: no EBSC scaled strike is found */
#endif
    uint32_t n = be32(p + 4);
    if (n > (len - 8) / 0x1c) return 0;
    uint32_t pos = 8;
    for (; n; n--, pos = add32(pos, 0x1c)) {
        if (ppx == p[add32(pos, 0x18)] && ppy == p[add32(pos, 0x19)]) {
            *off = pos;
            return 1;
        }
    }
    return 0;
}

typedef uint8_t (*findstrike_t)(const uint8_t *, uint32_t, uint16_t, uint16_t, uint32_t *);

/* sfac_SearchForStrike 0x7dc4: *kind 2 for an EBLC strike at (ppx, ppy), 3 for an EBSC scale
 * whose substitute ppem (*sx, *sy) has one, else 1; *off the BitmapSize offset */
TT_EXPORT int tt_sfac_SearchForStrike(uint8_t *cl, uint16_t ppx, uint16_t ppy, uint16_t *kind, uint16_t *sx,
                                      uint16_t *sy, uint32_t *off) {
    uint32_t len = AT(cl, 0xac, uint32_t), blen = AT(cl, 0xb4, uint32_t);
    *kind = 1;
    *off = 0;
    *sx = 0;
    *sy = 0;
    void *ctx = NULL;
    const uint8_t *bloc = NULL;
    if (len != 0) {
        if (len > 0x7fffffff || AT(cl, 0xa8, uint32_t) > 0x7fffffff) of();
        bloc = GETFRAG(cl, AT(cl, 0xa8, uint32_t), len, &ctx);
        /* NULL: "sfntaccs.c, sfac_GetDataPtr, optional table ignored due to bogus size" */
    }
    if (bloc) {
        if (ORIG(findstrike_t, RVA_FindBlocStrike)(bloc, len, ppx, ppy, off)) {
            *kind = 2;
        } else {
            void *bctx = NULL;
            const uint8_t *bsca = NULL;
            if (blen != 0) {
                if (blen > 0x7fffffff || AT(cl, 0xb0, uint32_t) > 0x7fffffff) of();
                bsca = GETFRAG(cl, AT(cl, 0xb0, uint32_t), blen, &bctx);
            }
            if (bsca && ORIG(findstrike_t, RVA_FindBscaStrike)(bsca, blen, ppx, ppy, off)) {
                *sx = bsca[*off + 0x1a];
                *sy = bsca[*off + 0x1b];
                if (ORIG(findstrike_t, RVA_FindBlocStrike)(bloc, len, *sx, (uint16_t)bsca[*off + 0x1b], off)) *kind = 3;
            }
            if (bctx) RELFRAG(cl, bctx);
        }
    }
    if (ctx) RELFRAG(cl, ctx);
    return 0;
}

/* sfac_SearchForBitmap 0x6730: the glyph in the strike at EBLC offset `strike`. *found, and:
 * *layout (1/2 small hor/vert metrics, 3 big), *mtable (1 metrics in EBLC, 2 in EBDT), *moff
 * the metrics offset, *imgfmt, *data the image data offset in EBDT, *size its size */
TT_EXPORT int tt_sfac_SearchForBitmap(uint8_t *cl, uint16_t g, uint32_t strike, uint8_t *found, uint16_t *layout,
                                      uint16_t *mtable, uint32_t *moff, uint16_t *imgfmt, uint32_t *data,
                                      uint32_t *size) {
    uint32_t L = AT(cl, 0xac, uint32_t);
    *found = 0;
    void *ctx = NULL;
    const uint8_t *D = NULL;
    if (L == 0) return 0x1409;
    if (L > 0x7fffffff || AT(cl, 0xa8, uint32_t) > 0x7fffffff) of();
    D = GETFRAG(cl, AT(cl, 0xa8, uint32_t), L, &ctx);
    if (!D) {
        if (ctx) RELFRAG(cl, ctx);
        return 0x1408;
    }
    uint64_t s = strike;
    if (s + 0x2a > 0xffffffffu) of();
    if (g < be16(D + s + 0x28) || g > be16(D + s + 0x2a)) goto done;
    if (s + 8 > 0xffffffffu) of();
    uint32_t nsub = be32(D + s + 8), arr = be32(D + s);
    if ((uint64_t)nsub << 3 > 0xffffffffu) of();
    uint32_t end = (uint32_t)((uint64_t)nsub << 3) + arr;
    if (end < arr) of();
    if (end > L) goto done;   /* "sfntaccs.c, sfac_SearchForBitmap, offset out of bounds" */
    uint64_t pos = arr;
    for (; nsub != 0 && !*found; nsub--) {
        uint16_t first = be16(D + pos);
        if (pos + 2 > 0xffffffffu) of();
        if (g < first || be16(D + pos + 2) < g) goto next;
        if (pos + 4 > 0xffffffffu) of();
        uint32_t sub = be32(D + pos + 4) + arr;
        if (sub < arr) of();
        if ((uint64_t)sub + 8 > 0xffffffffu) of();
        if (L < sub + 8 || L < sub || L < 8) goto done;   /* "... offset out of bounds" */
        uint16_t ifmt = be16(D + sub), ifm = be16(D + sub + 2);
        uint32_t ioff = be32(D + sub + 4);
        uint32_t o, sz;
        if (ifmt == 1) {
            uint32_t e = sub + ((uint32_t)(uint16_t)(g - first) + 2) * 4;
            if (e < sub) of();
            if ((uint64_t)e + 8 > 0xffffffffu) of();
            if (L < e + 8) goto done;   /* "format 1, offset out of bounds" */
            o = be32(D + e);
            uint32_t o2 = be32(D + e + 4);
            if (o2 < o) of();
            sz = o2 - o;
            if (sz == 0) goto done;
            goto ebdt;
        } else if (ifmt == 2) {
            uint32_t e = sub + 8;
            if ((uint64_t)e + 4 > 0xffffffffu) of();
            if (L < e + 4) goto done;   /* "format 2, offset out of bounds" */
            sz = be32(D + e);
            uint64_t prod = (uint64_t)(uint16_t)(g - first) * sz;
            if (prod > 0xffffffffu) of();
            o = (uint32_t)prod + ioff;
            if (o < ioff) of();
            *data = o;
            *moff = add32(sub, 0xc);
            *mtable = 1;
            *layout = 3;
            goto image;
        } else if (ifmt == 3) {
            uint32_t e = sub + ((uint32_t)(uint16_t)(g - first) + 4) * 2;
            if (e < sub) of();
            if ((uint64_t)e + 4 > 0xffffffffu) of();
            if (L < e + 4) goto done;   /* "format 3, offset out of bounds" */
            o = be16(D + e);
            uint32_t o2 = be16(D + e + 2);
            if (o2 < o) of();
            sz = o2 - o;
            if (sz == 0) goto done;
            goto ebdt;
        } else if (ifmt == 4) {
            uint32_t e = sub + 8;
            if ((uint64_t)e + 4 > 0xffffffffu) of();
            if (L < e + 4) goto done;   /* "format 4, offset out of bounds" */
            uint32_t n = be32(D + e);
            if (n == 0) of();
            uint32_t a = sub + 0xc;
            if ((uint64_t)sub + 0xc > 0xffffffffu) of();
            if (a > L) of();
            uint32_t avail = L - a ? (L - a) >> 2 : 0;
            if (avail == 0) of();
            if (avail - 1 < n) goto done;   /* "format 4, offset out of bounds for glyphArray" */
            uint32_t hit = a;
            if (be16(D + a) != g) {
                uint32_t mid = n - 1, lo = 0, hi = n - 1;
                for (;;) {
                    if ((uint64_t)mid * 4 > 0xffffffffu) of();
                    uint32_t p = mid * 4 + a;
                    if (p < a) of();
                    uint16_t code = be16(D + p);
                    if (code == g) {
                        hit = p;
                        break;
                    }
                    uint32_t nlo = mid;
                    if (g <= code) nlo = lo, hi = mid;
                    if (hi < nlo) of();
                    if (hi - nlo < 2) goto done;
                    if (hi + nlo < nlo) of();
                    mid = (hi + nlo) >> 1;
                    lo = nlo;
                }
            }
            if ((uint64_t)hit + 4 + 2 > 0xffffffffu) of();
            o = be16(D + hit + 2);
            uint32_t o2 = be16(D + hit + 6);
            if (o2 < o) of();
            sz = o2 - o;
            goto ebdt;
        } else if (ifmt == 5) {
            uint32_t a = sub + 0x18;
            if ((uint64_t)sub + 0x18 > 0xffffffffu) of();
            if (L < a) goto done;   /* "format 5, offset out of bounds" */
            sz = be32(D + sub + 8);
            *moff = add32(sub, 0xc);
            *mtable = 1;
            *layout = 3;
            if ((uint64_t)sub + 0x14 > 0xffffffffu) of();
            uint32_t n = be32(D + sub + 0x14);
            if (n == 0) of();
            if (n > (L - a ? (L - a) >> 1 : 0)) goto done;   /* "format 5, ... for glyphArray" */
            uint32_t idx = 0;
            if (be16(D + a) != g) {
                uint32_t mid = n - 1, lo = 0, hi = n - 1;
                if ((uint64_t)(n - 1) * 2 > 0xffffffffu) of();
                for (;;) {
                    uint32_t p = mid * 2 + a;
                    if (p < a) of();
                    uint16_t code = be16(D + p);
                    if (code == g) {
                        idx = mid;
                        break;
                    }
                    uint32_t nlo = mid;
                    if (g <= code) nlo = lo, hi = mid;
                    if (hi < nlo) of();
                    if (hi - nlo < 2) goto done;
                    if (hi + nlo < nlo) of();
                    mid = (hi + nlo) >> 1;
                    lo = nlo;
                }
            }
            uint64_t prod = (uint64_t)idx * sz;
            if (prod > 0xffffffffu) of();
            o = (uint32_t)prod + ioff;
            if (o < ioff) of();
            *data = o;
            goto image;
        } else {
            goto done;
        }
    ebdt:
        o += ioff;
        if (o < ioff) of();
        *moff = o;
        *mtable = 2;
    image:
        *size = sz;
        *imgfmt = ifm;
        if (s + 0x2f > 0xffffffffu) of();
        {
            uint8_t flags = D[s + 0x2f];
            switch (ifm) {
            case 1: case 2:
                *layout = (uint16_t)((flags != 1) + 1);
                *data = add32(o, 5);
                *found = 1;
                break;
            case 5:
                *found = 1;
                break;
            case 6: case 7: case 9:
                *layout = 3;
                *data = add32(o, 8);
                *found = 1;
                break;
            case 8:
                *layout = (uint16_t)((flags != 1) + 1);
                *data = add32(add32(o, 5), 1);
                *found = 1;
                break;
            default:
                break;
            }
        }
    next:
        pos += 8;
        if (pos > 0xffffffffu) of();
    }
done:
    if (ctx) RELFRAG(cl, ctx);
    return 0;
}

typedef int (*searchstrike_t)(uint8_t *, uint16_t, uint16_t, uint16_t *, uint16_t *, uint16_t *, uint32_t *);
typedef int (*searchbitmap_t)(uint8_t *, uint16_t, uint32_t, uint8_t *, uint16_t *, uint16_t *, uint32_t *,
                              uint16_t *, uint32_t *, uint32_t *);

/* sbit_SearchForBitmap 0x6620: under a multiple of 90 degrees (rotation code < 4), the strike
 * for the ppem (searched once per transform) and the glyph in it. *found: 1 from an EBLC
 * strike, 2 from an EBSC-scaled one, 0 none */
TT_EXPORT int tt_sbit_SearchForBitmap(uint8_t *st, uint8_t *cl, uint16_t g, uint16_t *found) {
    *found = 0;
    if (AT(st, 0x22, uint16_t) >= 4) return 0;
    if (AT(st, 0x18, uint16_t) == 0) {
        int e = ORIG(searchstrike_t, RVA_sfac_SearchForStrike)(cl, AT(st, 0x1a, uint16_t), AT(st, 0x1c, uint16_t),
                                                              (uint16_t *)(st + 0x18), (uint16_t *)(st + 0x1e),
                                                              (uint16_t *)(st + 0x20), (uint32_t *)st);
        if (e) return e;
    }
    if ((uint16_t)(AT(st, 0x18, uint16_t) - 2) < 2) {
        int e = ORIG(searchbitmap_t, RVA_sfac_SearchForBitmap)(
            cl, g, AT(st, 0, uint32_t), st + 0x4c, (uint16_t *)(st + 0x24), (uint16_t *)(st + 0x26),
            (uint32_t *)(st + 4), (uint16_t *)(st + 0x28), (uint32_t *)(st + 8), (uint32_t *)(st + 0xc));
        if (e) return e;
        if (st[0x4c]) {
            *found = (uint16_t)((AT(st, 0x18, uint16_t) != 2) + 1);
            st[0x4d] = 0;
        }
    }
    return 0;
}

/* ============================ metrics ============================ */

/* sfac_ShaveSbitMetrics 0x3f70: for image format 5 (bit-aligned, metrics from the index
 * subtable) up to 128 pixels wide, the blank rows above and below the ink and the blank
 * columns left and right of it are cut off: width, height and the four bearings follow */
TT_EXPORT int tt_sfac_ShaveSbitMetrics(uint8_t *cl, uint16_t fmt, uint32_t off, uint32_t size, uint16_t *ph,
                                       uint16_t *pw, uint16_t *pl, uint16_t *pr, uint16_t *pt, uint16_t *pb,
                                       int16_t *hbx, int16_t *hby, int16_t *vbx, int16_t *vby) {
    static const uint8_t mask_of[9] = {0x00, 0x80, 0xc0, 0xe0, 0xf0, 0xf8, 0xfc, 0xfe, 0xff};   /* 0x1801c1100 */
    *pl = 0;
    *pr = 0;
    *pt = 0;
    *pb = 0;
    if (fmt != 5 || *pw > 0x80) return 0;
    uint8_t cols[16];
    memset(cols, 0, sizeof cols);
    if (size == 0 || *pw == 0 || *ph == 0) return 0;
    void *ctx = NULL;
    const uint8_t *p = NULL;
    int err;
    if (AT(cl, 0xa4, uint32_t) == 0) {
        err = 0x1409;
    } else {
        if (size > 0x7fffffff) of();
        uint32_t o = AT(cl, 0xa0, uint32_t) + off;
        if (o < off || o > 0x7fffffff) of();
        p = GETFRAG(cl, o, size, &ctx);
        err = p ? 0 : 0x1408;
    }
    if (err) {
        if (ctx) RELFRAG(cl, ctx);   /* AutoReleaseSfntFrag::Release */
        return err;
    }
    uint16_t acc = 0, bits = 0, top = 0, bottom = 0;
    int seen = 0;
    for (uint16_t rows = *ph; rows; rows--) {
        uint8_t *m = cols, ink = 0;
        uint16_t rem = *pw, chunk = 8;
        while (rem) {
            if (bits < 8) {
                acc = (uint16_t)(acc << 8);
                if (size) acc |= *p++, size--;
                if ((uint32_t)bits + 8 > 0xffff) of();
                bits += 8;
            }
            if (chunk > rem) chunk = rem;
            uint8_t v = (uint8_t)(acc >> (bits - 8)) & mask_of[chunk];
            ink |= v;
            *m++ |= v;
            if (chunk > bits) of();
            bits -= chunk;
            rem -= chunk;
        }
        if (ink) {
            seen = 1;
            bottom = (uint16_t)(rows - 1);
        } else if (!seen) {
            if (top == 0xffff) of();
            top++;
        }
    }
    if (top == *ph) top = 0, bottom = 0;
#ifdef IDWR_NEGATIVE_CONTROL_SHAVE
    top = 0;   /* sbit_metrics_check.py --control shave: the blank rows above the ink stay */
#endif
    /* the first inked column from the left, then from the right */
    uint16_t left = 0, right = 0;
    const uint8_t *q = cols;
    for (;;) {
        uint8_t bit = 0x80;
        for (; bit; bit >>= 1) {
            if (*q & bit) goto found;
            if (left == 0xffff) of();
            left++;
            if (left == *pw) goto out;   /* no ink: nothing is shaved */
        }
        q++;
    }
found:;
    uint16_t c = (uint16_t)(*pw - 1);
    q = cols + (c >> 3);
    uint8_t bit = (uint8_t)(0x80 >> (c & 7));
    while (!(*q & bit)) {
        if (right == 0xffff) of();
        right++;
        if (bit == 0x80) bit = 1, q--;
        else bit = (uint8_t)(bit << 1);
    }
    *pl = left;
    *pr = right;
    {
        uint32_t s = (uint32_t)left + right;
        if (s > 0xffff || s > *pw) of();
        *pw = (uint16_t)(*pw - s);
        if (left > 0x7fff) of();
        int32_t v = *hbx + (int16_t)left;
        if ((uint32_t)(v + 0x8000) > 0xffff) of();
        *hbx = (int16_t)v;
        v = *vbx + (int16_t)left;
        if ((uint32_t)(v + 0x8000) > 0xffff) of();
        *vbx = (int16_t)v;
        *pt = top;
        *pb = bottom;
        s = (uint32_t)top + bottom;
        if (s > 0xffff || s > *ph) of();
        *ph = (uint16_t)(*ph - s);
        if (top > 0x7fff) of();
        v = *hby - (int16_t)top;
        if ((uint32_t)(v + 0x8000) > 0xffff) of();
        *hby = (int16_t)v;
        v = *vby - (int16_t)top;
        if ((uint32_t)(v + 0x8000) > 0xffff) of();
        *vby = (int16_t)v;
    }
out:
    if (ctx) RELFRAG(cl, ctx);
    return 0;
}

typedef int (*getsbitmetrics_t)(uint8_t *, uint16_t, uint16_t, uint32_t, uint16_t *, uint16_t *, int16_t *,
                                int16_t *, int16_t *, int16_t *, uint16_t *, uint16_t *, uint8_t *, uint8_t *);
typedef int (*shave_t)(uint8_t *, uint16_t, uint32_t, uint32_t, uint16_t *, uint16_t *, uint16_t *, uint16_t *,
                       uint16_t *, uint16_t *, int16_t *, int16_t *, int16_t *, int16_t *);

/* GetSbitMetrics 0x1f8c: the strike glyph's metrics, once per glyph (+0x4d). Missing
 * horizontal or vertical metrics come from hmtx / vmtx (SubstituteHorMetrics,
 * SubstituteVertMetrics). The vertical bearing x is then always recomputed: the glyph centred
 * on the em box, (SEm(ascender - descender) - UEm(hmtx advance)) / 2 + SEm(descender) plus the
 * horizontal bearing x (the descender is negative: the box's left edge relative to the
 * vertical origin is its bottom edge relative to the baseline). Then ShaveSbitMetrics. */
TT_EXPORT int tt_GetSbitMetrics(uint8_t *st, uint8_t *cl) {
    uint8_t hor = 0, vert = 0;
    if (st[0x4d]) return 0;
    int e = ORIG(getsbitmetrics_t, RVA_sfac_GetSbitMetrics)(
        cl, AT(st, 0x24, uint16_t), AT(st, 0x26, uint16_t), AT(st, 4, uint32_t), (uint16_t *)(st + 0x2a),
        (uint16_t *)(st + 0x2c), (int16_t *)(st + 0x44), (int16_t *)(st + 0x46), (int16_t *)(st + 0x48),
        (int16_t *)(st + 0x4a), (uint16_t *)(st + 0x2e), (uint16_t *)(st + 0x30), &hor, &vert);
    if (e) return e;
    if (!hor && (e = ORIG(int (*)(uint8_t *, uint8_t *), RVA_SubstituteHorMetrics)(st, cl)) != 0) return e;
    if (!vert && (e = ORIG(int (*)(uint8_t *, uint8_t *), RVA_SubstituteVertMetrics)(st, cl)) != 0) return e;
    int16_t desc = AT(cl, 0xf2, int16_t);
    uint16_t span = (uint16_t)(AT(cl, 0xf0, uint16_t) - (uint16_t)desc);
    uint16_t adv = 0;
    int16_t lsb = 0;
    e = ORIG(int (*)(uint8_t *, uint16_t, uint16_t *, int16_t *), RVA_sfac_ReadGlyphHorMetrics)(
        cl, AT(cl, 0xe8, uint16_t), &adv, &lsb);
    if (e) return e;
    uint16_t uadv, sspan, sdesc;
    ORIG(uscale_t, RVA_UEmScaleX)(&uadv, st, adv);
    ORIG(sscale_t, RVA_SEmScaleX)(&sspan, st, (int16_t)span);
    ORIG(sscale_t, RVA_SEmScaleX)(&sdesc, st, desc);
    int32_t half = ((int32_t)(int16_t)sspan - (int32_t)uadv) / 2;
#ifdef IDWR_NEGATIVE_CONTROL_VERTBEARING
    half = 0;   /* sbit_metrics_check.py --control vertbearing: no centring on the em box */
#endif
    AT(st, 0x48, uint16_t) = (uint16_t)((uint16_t)half + sdesc + AT(st, 0x44, uint16_t));
    e = ORIG(shave_t, RVA_sfac_ShaveSbitMetrics)(
        cl, AT(st, 0x28, uint16_t), AT(st, 8, uint32_t), AT(st, 0xc, uint32_t), (uint16_t *)(st + 0x2a),
        (uint16_t *)(st + 0x2c), (uint16_t *)(st + 0x3c), (uint16_t *)(st + 0x3e), (uint16_t *)(st + 0x40),
        (uint16_t *)(st + 0x42), (int16_t *)(st + 0x44), (int16_t *)(st + 0x46), (int16_t *)(st + 0x48),
        (int16_t *)(st + 0x4a));
    if (e) return e;
    st[0x4d] = 1;
    return 0;
}

static int32_t sh6(int32_t v) { return (int32_t)((uint32_t)v << 6); }

/* sbit_GetMetrics 0x17c4: the strike glyph's device metrics in 26.6, turned by the rotation
 * code (+0x22): adv the advance, lsb the left side bearing point, p5 the bitmap's top-left
 * corner, vadv the vertical advance, vorg (p7, and p8 a copy of the turned origin) the
 * vertical origin's offset from the bitmap; rect {top, left, bottom, right} in pixels; the row
 * bytes and the bitmap sizes */
TT_EXPORT int tt_sbit_GetMetrics(uint8_t *st, uint8_t *cl, int32_t *adv, int32_t *lsb, int32_t *p5, int32_t *vadv,
                                 int32_t *p7, int32_t *p8, int16_t *rect, uint16_t *rowbytes, uint32_t *size,
                                 uint32_t *size2) {
    int e = ORIG(int (*)(uint8_t *, uint8_t *), RVA_GetSbitMetrics)(st, cl);
    if (e) return e;
    uint16_t t;
    ORIG(uscale_t, RVA_UScaleX)(&t, st, AT(st, 0x2c, uint16_t));
    AT(st, 0x36, uint16_t) = t;
    ORIG(uscale_t, RVA_UScaleY)(&t, st, AT(st, 0x2a, uint16_t));
    AT(st, 0x34, uint16_t) = t;
    ORIG(sscale_t, RVA_SScaleY)(&t, st, AT(st, 0x46, int16_t));
    int32_t top = (int16_t)t;
    ORIG(sscale_t, RVA_SScaleX)(&t, st, AT(st, 0x44, int16_t));
    int32_t left = (int16_t)t;
    if (AT(st, 0x34, uint16_t) > 0x7fff) of();
    int32_t bottom = top - AT(st, 0x34, int16_t);
    if ((uint32_t)(bottom + 0x8000) > 0xffff) of();
    if (AT(st, 0x36, uint16_t) > 0x7fff) of();
    int32_t right = AT(st, 0x36, int16_t) + left;
    if ((uint32_t)(right + 0x8000) > 0xffff) of();
    ORIG(uscale_t, RVA_UScaleX)(&t, st, AT(st, 0x2e, uint16_t));
    int32_t aw = sh6(t);
    ORIG(uscale_t, RVA_UScaleY)(&t, st, AT(st, 0x30, uint16_t));
    int32_t ah = sh6(t);
    ORIG(sscale_t, RVA_SScaleX)(&t, st, AT(st, 0x44, int16_t));
    int32_t l6 = sh6((int16_t)t);
    ORIG(sscale_t, RVA_SScaleY)(&t, st, AT(st, 0x46, int16_t));
    int32_t t6 = sh6((int16_t)t);
    ORIG(sscale_t, RVA_SScaleX)(&t, st, AT(st, 0x48, int16_t));
    int32_t vx6 = sh6((int16_t)t);
    ORIG(sscale_t, RVA_SScaleY)(&t, st, AT(st, 0x4a, int16_t));
    int32_t vy6 = sh6((int16_t)t);
#ifdef IDWR_NEGATIVE_CONTROL_VERTORIGIN
    vy6 += 64;   /* sbit_metrics_check.py --control vertorigin: the vertical origin one pixel lower */
#endif
    uint32_t r9 = ((uint32_t)AT(st, 0x2c, uint16_t) + 0x1f) >> 3;
    uint32_t dx = ((uint32_t)AT(st, 0x36, uint16_t) + 0x1f) >> 3;
    AT(st, 0x32, uint16_t) = (uint16_t)(r9 & 0xfffc);
    AT(st, 0x38, uint16_t) = (uint16_t)(dx & 0xfffc);
    uint16_t maxh = AT(st, 0x2a, uint16_t) >= AT(st, 0x34, uint16_t) ? AT(st, 0x2a, uint16_t) : AT(st, 0x34, uint16_t);
    uint32_t big = (AT(st, 0x32, uint16_t) >= AT(st, 0x38, uint16_t) ? r9 : dx) & 0xfffc;
    big *= maxh;
    uint32_t sz;
    uint16_t rb;
    switch (AT(st, 0x22, uint16_t)) {
    case 0:
        rect[0] = (int16_t)top, rect[1] = (int16_t)left, rect[2] = (int16_t)bottom, rect[3] = (int16_t)right;
        adv[0] = aw, adv[1] = 0;
        lsb[0] = l6, lsb[1] = t6;
        p5[0] = l6, p5[1] = sh6(top);
        vadv[0] = 0, vadv[1] = ah;
        p7[0] = vx6, p7[1] = vy6;
        p8[0] = vx6, p8[1] = vy6;
        rb = (uint16_t)((((uint32_t)AT(st, 0x36, uint16_t) + 0x1f) >> 3) & 0xfffc);
        AT(st, 0x3a, uint16_t) = rb;
        sz = (uint32_t)rb * AT(st, 0x34, uint16_t);
        AT(st, 0x10, uint32_t) = sz;
        if (AT(st, 0x18, int16_t) != 3) big = 0;
        goto tail;
    case 1: {
        int32_t wd6 = sh6(right - left);
        rect[0] = (int16_t)right, rect[1] = (int16_t)-top, rect[2] = (int16_t)left, rect[3] = (int16_t)-bottom;
        adv[0] = 0, adv[1] = aw;
        lsb[0] = -t6, lsb[1] = wd6 + l6;
        p5[0] = 0, p5[1] = sh6(right) - l6;
        vadv[0] = -ah, vadv[1] = 0;
        p7[0] = -vy6, p7[1] = vx6 + wd6;
        p8[0] = -vy6 - sh6(top), p8[1] = 0;
        rb = (uint16_t)((((uint32_t)AT(st, 0x34, uint16_t) + 0x1f) >> 3) & 0x1ffffffc);
        sz = (uint32_t)AT(st, 0x36, uint16_t) * rb;
        break;
    }
    case 2: {
        int32_t lr6 = sh6(left - right), tb6 = sh6(top - bottom);
        rect[0] = (int16_t)-bottom, rect[1] = (int16_t)-right, rect[2] = (int16_t)-top, rect[3] = (int16_t)-left;
        adv[0] = -aw, adv[1] = 0;
        lsb[0] = lr6 - l6, lsb[1] = tb6 - t6;
        p5[0] = -l6, p5[1] = -sh6(bottom);
        vadv[0] = 0, vadv[1] = -ah;
        p7[0] = lr6 - vx6, p7[1] = tb6 - vy6;
        p8[0] = -sh6(right), p8[1] = -vy6;
        rb = (uint16_t)((((uint32_t)AT(st, 0x36, uint16_t) + 0x1f) >> 3) & 0x1ffffffc);
        sz = (uint32_t)AT(st, 0x34, uint16_t) * rb;
        break;
    }
    case 3: {
        int32_t bt6 = sh6(bottom - top);
        rect[0] = (int16_t)-left, rect[1] = (int16_t)bottom, rect[2] = (int16_t)-right, rect[3] = (int16_t)top;
        adv[0] = 0, adv[1] = -aw;
        lsb[0] = t6 + bt6, lsb[1] = -l6;
        p5[0] = 0, p5[1] = l6 - sh6(left);
        vadv[0] = ah, vadv[1] = 0;
        p7[0] = bt6 + vy6, p7[1] = -vx6;
        p8[0] = sh6(bottom - 2 * top) + vy6, p8[1] = 0;
        rb = (uint16_t)((((uint32_t)AT(st, 0x34, uint16_t) + 0x1f) >> 3) & 0x1ffffffc);
        sz = (uint32_t)AT(st, 0x36, uint16_t) * rb;
        break;
    }
    default:
        return 0x1802;
    }
    AT(st, 0x3a, uint16_t) = rb;
    AT(st, 0x10, uint32_t) = sz;
tail:
    AT(st, 0x14, uint32_t) = big;
    *rowbytes = AT(st, 0x3a, uint16_t);
    *size = sz;
    *size2 = AT(st, 0x14, uint32_t);
    return 0;
}
