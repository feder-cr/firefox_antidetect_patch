/*
 * TrueType scaler font data access (sfac_*, sfntaccs.c), ported from DWrite.dll 10.0.26100.9711.
 * Reads the sfnt tables (table directory, head/hhea/OS/2/maxp/vhea, loca/gdir, glyf, hmtx/vmtx,
 * cvt, fpgm, prep, cmap header) through the client's fragment callbacks, exactly in the
 * original's order of GetSfntFragment / ReleaseSfntFragment calls.
 *
 * sfac_ClientRec (raw offsets, no header in the original):
 *   +0x00 clientID            +0x08 GetSfntFragment(id, offset, length, &ctx) -> data
 *   +0x10 ReleaseSfntFragment(ctx)
 *   +0x18 head.indexToLocFormat (int16)    +0x1c cmap subtable offset (uint32)
 *   +0x20 table directory, 8 bytes per entry {offset, length}, index from sfac_Classify:
 *         0 head 1 hhea 2 loca 3 maxp 4 cvt 5 prep 6 glyf 7 hmtx 8 cmap 9 fpgm 0xb hdmx
 *         0xc LTSH 0xe OS/2 0xf gdir 0x10 EBDT 0x11 EBLC 0x12 EBSC 0x13 vhea 0x14 vmtx
 *         0x15 fvar 0x16 {0, 0xffffffff}: whole file (gdir offsets are file offsets)
 *   +0xd8 hhea.numberOfHMetrics  +0xda vhea.numOfLongVerMetrics  +0xdc vhea present (bool)
 *   +0xde cmap subtable format   +0xe0 sfac_ComputeIndex* function   +0xe8 glyph index
 *   +0xea/+0xec/+0xee cmap format 4 searchRange/entrySelector/rangeShift
 *   +0xf0 ascender +0xf2 descender (OS/2 typo, else hhea)  +0xf4 hhea.descender
 *   +0x100 cvt variation callback(id, cvt, count)
 * sfac_GHandle (glyph data cursor): +0x00 size (uint32), +0x08 base, +0x18 current pointer.
 *
 * SafeInt checks are kept; when one fires the original throws (SafeIntOnOverflow), so the
 * port calls the original thrower, after releasing the fragments that the original's
 * AutoReleaseSfntFrag objects release during unwinding. OutputDebugStringA is called where
 * the original calls it. The original is built with asynchronous exception unwinding, so an
 * access violation releases held fragments too; the port reproduces that only for the one
 * deterministic case (sfac_CopyCVT with a NULL cvt fragment).
 */
#ifdef _WIN32
#include <windows.h> /* OutputDebugStringA; elsewhere tt_state.h */
#endif
#include <stdint.h>
#include <string.h>
#include "tt_state.h"
#include "orig.h"

#define RVA_SafeIntOnOverflow     0x1090b0
#define RVA_ComputeIndexNone      0xdcb00  /* ICF-folded "return 0" (ICacheReference::GetExpirationEvent) */
#define RVA_sfac_ComputeIndex0    0x14d530
#define RVA_sfac_ComputeIndex2    0x14d560
#define RVA_sfac_ComputeIndex4    0x14d730
#define RVA_sfac_ComputeIndex6    0x14deb0

#define ERR_SFAC_BAD_TABLE_DIR    0x1007
#define ERR_SFAC_1400             0x1400
#define ERR_SFAC_POINTS           0x1401
#define ERR_SFAC_CONTOURS         0x1403
#define ERR_SFAC_MAGIC            0x1405
#define ERR_SFAC_CMAP             0x1406
#define ERR_SFAC_NULL_FRAGMENT    0x1408
#define ERR_SFAC_MISSING_TABLE    0x1409
#define ERR_SFAC_CMAP_FORMAT      0x140a
#define ERR_SFAC_MAXP             0x140b
#define ERR_SFAC_GLYPH_DATA       0x140d
#define ERR_SFAC_UNITS_PER_EM     0x140e
#define ERR_SFAC_HMTX             0x140f
#define ERR_SFAC_LOCA             0x1411

static const char kBogusSize[] = "sfntaccs.c, sfac_GetDataPtr, optional table ignored due to bogus size";

typedef const uint8_t *(*get_fragment_fn)(void *id, uint32_t offset, uint32_t length, void **ctx);
typedef void (*release_fragment_fn)(void *ctx);
typedef void (*cvt_variation_fn)(void *id, int32_t *cvt, uint32_t count);

#define C_ID(c)          AT(c, 0x00, void *)
#define C_GET(c)         AT(c, 0x08, get_fragment_fn)
#define C_RELEASE(c)     AT(c, 0x10, release_fragment_fn)
#define C_LOCFORMAT(c)   AT(c, 0x18, int16_t)
#define C_CMAPOFF(c)     AT(c, 0x1c, uint32_t)
#define C_TOFF(c, i)     AT(c, 0x20 + 8 * (i), uint32_t)
#define C_TLEN(c, i)     AT(c, 0x24 + 8 * (i), uint32_t)
#define C_NHMETRICS(c)   AT(c, 0xd8, uint16_t)
#define C_NVMETRICS(c)   AT(c, 0xda, uint16_t)
#define C_HASVMTX(c)     AT(c, 0xdc, uint8_t)
#define C_CMAPFMT(c)     AT(c, 0xde, uint16_t)
#define C_COMPUTEIDX(c)  AT(c, 0xe0, void *)
#define C_GLYPH(c)       AT(c, 0xe8, uint16_t)
#define C_ASC(c)         AT(c, 0xf0, int16_t)
#define C_DESC(c)        AT(c, 0xf2, int16_t)
#define C_CVTVAR(c)      AT(c, 0x100, cvt_variation_fn)

enum { T_HEAD = 0, T_HHEA = 1, T_LOCA = 2, T_MAXP = 3, T_CVT = 4, T_PREP = 5, T_GLYF = 6,
       T_HMTX = 7, T_CMAP = 8, T_FPGM = 9, T_OS2 = 0xe, T_GDIR = 0xf, T_VHEA = 0x13, T_VMTX = 0x14 };

/* glyph data handle */
#define GH_SIZE(g)  AT(g, 0x00, uint32_t)
#define GH_BASE(g)  AT(g, 0x08, const uint8_t *)
#define GH_CUR(g)   AT(g, 0x18, const uint8_t *)

/* LocalMaxProfile: maxp field order, version as uint32 at +0 */
#define LMP_POINTS(m)        AT(m, 0x06, uint16_t)
#define LMP_CONTOURS(m)      AT(m, 0x08, uint16_t)
#define LMP_CPOINTS(m)       AT(m, 0x0a, uint16_t)
#define LMP_CCONTOURS(m)     AT(m, 0x0c, uint16_t)

__attribute__((noreturn)) static void overflow(void) {
    ORIG(void (*)(void), RVA_SafeIntOnOverflow)();
    __builtin_trap();
}

static const uint8_t *volatile g_null;  /* stays NULL: a read the compiler cannot elide */

static inline uint16_t be16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }
static inline uint32_t be32(const uint8_t *p) {
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static inline const uint8_t *get_frag(uint8_t *c, uint32_t off, uint32_t len, void **ctx) {
    return C_GET(c)(C_ID(c), off, len, ctx);
}
static inline void release(uint8_t *c, void *ctx) {
    if (ctx) C_RELEASE(c)(ctx);
}

/* A SafeInt check fired while AutoReleaseSfntFrag objects hold fragments: the original
 * releases them while unwinding (most recent first); the port releases them before throwing. */
__attribute__((noreturn)) static void overflow_release4(uint8_t *c, void *a, void *b, void *d, void *e) {
    release(c, a);
    release(c, b);
    release(c, d);
    release(c, e);
    overflow();
}
#define overflow_release(c, a, b, d) overflow_release4(c, a, b, d, NULL)

/* The inlined table read of the original: a missing table gives NULL (and *err 0x1409 when
 * the table is required); a NULL fragment gives 0x1408 when required, else the debug string.
 * held1..4: fragments the caller holds, released if the SafeInt check fires. */
static const uint8_t *read_table(uint8_t *c, int t, uint32_t len, void **ctx, int *err, void *held1, void *held2,
                                 void *held3, void *held4) {
    if (C_TLEN(c, t) == 0) {
        if (err) *err = ERR_SFAC_MISSING_TABLE;
        return NULL;
    }
    if (C_TOFF(c, t) > 0x7fffffff) overflow_release4(c, held1, held2, held3, held4);
    const uint8_t *p = get_frag(c, C_TOFF(c, t), len, ctx);
    if (!p) {
        if (err) *err = ERR_SFAC_NULL_FRAGMENT;
        else OutputDebugStringA(kBogusSize);
    } else if (err) {
        *err = 0;
    }
    return p;
}

/* ---- AutoReleaseSfntFrag::Release (0x18000711c): this = {client, ctx} ---- */
TT_EXPORT void tt_AutoReleaseSfntFrag_Release(void **self) {
    if (self[1]) C_RELEASE((uint8_t *)self[0])(self[1]);
}

/* ---- sfac_GetDataPtr (0x1800c6018) ---- */
static int GetDataPtr(uint8_t *c, uint32_t add, uint32_t len, int32_t t, uint8_t required,
                      const uint8_t **out, void **ctx) {
    *ctx = NULL;
    *out = NULL;
    if (len == 0 || C_TLEN(c, t) == 0) return required ? ERR_SFAC_MISSING_TABLE : 0;
    if (len > 0x7fffffff) overflow();
    uint32_t off = C_TOFF(c, t) + add;
    if (off < add || off > 0x7fffffff) overflow();
    const uint8_t *p = get_frag(c, off, len, ctx);
    *out = p;
    if (!p) {
        if (required) return ERR_SFAC_NULL_FRAGMENT;
        OutputDebugStringA(kBogusSize);
    }
    return 0;
}
TT_EXPORT int tt_sfac_GetDataPtr(uint8_t *c, uint32_t add, uint32_t len, int32_t t, uint32_t required,
                                 const uint8_t **out, void **ctx) {
    return GetDataPtr(c, add, len, t, (uint8_t)required, out, ctx);
}

/* ---- sfac_GetGlyphLocation (0x18000f078) ----
 * gdir (Type 42 / CFF-less glyph directory) wins over loca when present: 6-byte entries
 * {offset32, length16}, table index 0x16 (file offsets). */
static int GetGlyphLocation(uint8_t *c, uint16_t g, uint32_t *off, uint32_t *len, int32_t *t) {
    void *ctx1 = NULL, *ctx2 = NULL;
    uint32_t g6 = (uint32_t)g * 6;
    const uint8_t *gd = NULL;
    if (C_TLEN(c, T_GDIR) != 0) {
        if (C_TOFF(c, T_GDIR) > 0x7fffffff) overflow();
        gd = get_frag(c, C_TOFF(c, T_GDIR), g6 + 6, &ctx1);
        if (!gd) OutputDebugStringA(kBogusSize);
    }
    if (gd) {
        const uint8_t *e = gd + g6;
        uint32_t o = be32(e);
        *off = o;
        *len = o == 0 ? 0 : be16(e + 4);
        *t = 0x16;
        release(c, ctx1);
        return 0;
    }
    uint32_t n = C_LOCFORMAT(c) != 0 ? (uint32_t)g * 4 + 8 : (uint32_t)g * 2 + 4;
    const uint8_t *loca = NULL;
    int err;
    if (C_TLEN(c, T_LOCA) == 0) {
        err = ERR_SFAC_MISSING_TABLE;
    } else {
        if (C_TOFF(c, T_LOCA) > 0x7fffffff) overflow_release(c, ctx1, NULL, NULL);
        loca = get_frag(c, C_TOFF(c, T_LOCA), n, &ctx2);
        err = loca ? 0 : ERR_SFAC_NULL_FRAGMENT;
    }
    if (err) {
        release(c, ctx2);
        release(c, ctx1);
        return err;
    }
    uint32_t a, b;
    if (C_LOCFORMAT(c) != 0) {
        a = be32(loca + (size_t)g * 4);
        *off = a;
        b = be32(loca + (size_t)g * 4 + 4);
    } else {
        a = (uint32_t)be16(loca + (size_t)g * 2) * 2;
        *off = a;
        b = (uint32_t)be16(loca + (size_t)g * 2 + 2) * 2;
    }
    if (a > b) {
        release(c, ctx2);
        release(c, ctx1);
        return ERR_SFAC_LOCA;
    }
    *len = b - a;
    *t = T_GLYF;
    release(c, ctx2);
    release(c, ctx1);
    return 0;
}
TT_EXPORT int tt_sfac_GetGlyphLocation(uint8_t *c, uint32_t g, uint32_t *off, uint32_t *len, int32_t *t) {
    return GetGlyphLocation(c, (uint16_t)g, off, len, t);
}

/* ---- sfac_ReadGlyphBbox (0x18000fa40): bbox = {xMin, yMin, xMax, yMax} ---- */
static int ReadGlyphBbox(uint8_t *c, uint16_t g, int16_t *bbox) {
    uint32_t len = 0, off = 0;
    int32_t t = 0;
    int err = GetGlyphLocation(c, g, &off, &len, &t);
    if (err) return err;
    if (len == 0) {
        memset(bbox, 0, 8);
        return 0;
    }
    if (len < 10) return ERR_SFAC_GLYPH_DATA;
    void *ctx = NULL;
    const uint8_t *p = NULL;
    if (C_TLEN(c, t) == 0) {
        err = ERR_SFAC_MISSING_TABLE;
    } else {
        if (len > 0x7fffffff) overflow();
        uint32_t o = C_TOFF(c, t) + off;
        if (o < off || o > 0x7fffffff) overflow();
        p = get_frag(c, o, len, &ctx);
        err = p ? 0 : ERR_SFAC_NULL_FRAGMENT;
    }
    if (err) {
        release(c, ctx);
        return err;
    }
    bbox[0] = (int16_t)be16(p + 2);
    bbox[1] = (int16_t)be16(p + 4);
    bbox[2] = (int16_t)be16(p + 6);
    bbox[3] = (int16_t)be16(p + 8);
    release(c, ctx);
    if (bbox[0] > bbox[2] || bbox[1] > bbox[3]) return ERR_SFAC_1400;
    return 0;
}
TT_EXPORT int tt_sfac_ReadGlyphBbox(uint8_t *c, uint32_t g, int16_t *bbox) {
    return ReadGlyphBbox(c, (uint16_t)g, bbox);
}

/* ---- sfac_ReadGlyphHorMetrics (0x18000f5b0) ---- */
static int ReadGlyphHorMetrics(uint8_t *c, uint16_t g, uint16_t *aw, int16_t *lsb) {
    uint32_t nhm = C_NHMETRICS(c);
    uint32_t len = C_TLEN(c, T_HMTX);
    void *ctx = NULL;
    const uint8_t *h = NULL;
    int err;
    if (len == 0) {
        err = ERR_SFAC_MISSING_TABLE;
    } else {
        if (len > 0x7fffffff || C_TOFF(c, T_HMTX) > 0x7fffffff) overflow();
        h = get_frag(c, C_TOFF(c, T_HMTX), len, &ctx);
        err = h ? 0 : ERR_SFAC_NULL_FRAGMENT;
    }
    if (err) {
        release(c, ctx);
        return err;
    }
    if (g < nhm) {
        const uint8_t *e = h + ((uint32_t)g << 2);
        uint64_t last = (uint64_t)len - 4;        /* SafeInt: the table must hold 4 bytes */
        if (last > 0xffffffff) overflow_release(c, ctx, NULL, NULL);
        const uint8_t *lim = h + (uint32_t)last;
        if ((uintptr_t)lim < (uintptr_t)h) overflow_release(c, ctx, NULL, NULL);
        if (e > lim) {
            release(c, ctx);
            return ERR_SFAC_HMTX;
        }
        *aw = be16(e);
        *lsb = (int16_t)be16(e + 2);
        release(c, ctx);
        return 0;
    }
    /* glyphs past numberOfHMetrics: last advance, lsb from the trailing int16 array */
    const uint8_t *lsbs = h + (uint32_t)(nhm * 4);
    if (len < 2) overflow_release(c, ctx, NULL, NULL);
    const uint8_t *lim = h + (len - 2);
    if ((uintptr_t)lim < (uintptr_t)h) overflow_release(c, ctx, NULL, NULL);
    uint16_t k = (uint16_t)(g - nhm);
    const uint8_t *e = lsbs + (size_t)k * 2;
    if ((uintptr_t)e < (uintptr_t)lsbs) overflow_release(c, ctx, NULL, NULL);
    if (e > lim) {
        release(c, ctx);
        return ERR_SFAC_HMTX;
    }
    if (nhm == 0) overflow_release(c, ctx, NULL, NULL);
    *aw = be16(h + (uint32_t)((nhm - 1) * 4));
    *lsb = (int16_t)be16(e);
    release(c, ctx);
    return 0;
}
TT_EXPORT int tt_sfac_ReadGlyphHorMetrics(uint8_t *c, uint32_t g, uint16_t *aw, int16_t *lsb) {
    return ReadGlyphHorMetrics(c, (uint16_t)g, aw, lsb);
}

/* Vertical advance and top side bearing from vmtx; without vmtx (or a NULL fragment) from
 * ascender - descender and ascender - yMax of the glyph at client + 0xe8 (not g). The vmtx
 * fragment is only as long as glyph g's entry; there is no bounds check on it. */
static int vert_metrics(uint8_t *c, uint16_t g, uint16_t *ah, int16_t *tsb, void **ctx_out,
                        const uint8_t **vm_out) {
    uint32_t nlv = C_NVMETRICS(c);
    uint32_t n = g < nlv ? (uint32_t)g * 4 + 4 : ((uint32_t)g + nlv) * 2 + 2;
    const uint8_t *vm = NULL;
    void *ctx = NULL;
    if (C_HASVMTX(c) && C_TLEN(c, T_VMTX) != 0) {
        if (C_TOFF(c, T_VMTX) > 0x7fffffff) overflow();
        vm = get_frag(c, C_TOFF(c, T_VMTX), n, &ctx);
        if (!vm) OutputDebugStringA(kBogusSize);
    }
    *ctx_out = ctx;
    *vm_out = vm;
    if (C_HASVMTX(c) && vm) {
        if (g < nlv) {
            *ah = be16(vm + (uint32_t)g * 4);
            *tsb = (int16_t)be16(vm + (uint32_t)g * 4 + 2);
        } else {
            const uint8_t *e = vm + (uint32_t)(nlv * 4);
            *ah = be16(e - 4);
            *tsb = (int16_t)be16(e + (size_t)(g - nlv) * 2);
        }
        return 0;
    }
    int16_t bbox[4] = {0, 0, 0, 0};
    int err = ReadGlyphBbox(c, C_GLYPH(c), bbox);
    if (err) return err;
    *ah = (uint16_t)(C_ASC(c) - C_DESC(c));
    *tsb = (int16_t)(C_ASC(c) - bbox[3]);
    return 0;
}

/* ---- sfac_ReadGlyphVertMetrics (0x18000f840) ---- */
static int ReadGlyphVertMetrics(uint8_t *c, uint16_t g, uint16_t *ah, int16_t *tsb) {
    void *ctx;
    const uint8_t *vm;
    int err = vert_metrics(c, g, ah, tsb, &ctx, &vm);
    release(c, ctx);
    return err;
}
TT_EXPORT int tt_sfac_ReadGlyphVertMetrics(uint8_t *c, uint32_t g, uint16_t *ah, int16_t *tsb) {
    return ReadGlyphVertMetrics(c, (uint16_t)g, ah, tsb);
}

/* ---- sfac_ReadGlyphMetrics (0x18000f330) ----
 * mode 1: horizontal origin offset for vertical layout, -((asc - aw - desc) / 2) - desc
 * (IDIV truncation, 16-bit result); mode 2: 0; otherwise aw >> 1. Written even when the
 * vertical metrics fail. */
static int ReadGlyphMetrics(uint8_t *c, uint16_t g, uint16_t *aw, uint16_t *ah, int16_t *lsb,
                            int16_t *tsb, int16_t *org, uint8_t mode) {
    int err = ReadGlyphHorMetrics(c, g, aw, lsb);
    if (err) return err;
    void *ctx;
    const uint8_t *vm;
    err = vert_metrics(c, g, ah, tsb, &ctx, &vm);
    release(c, ctx);
    uint16_t v = 0;
    if (mode == 1) {
        int32_t desc = C_DESC(c);
        int32_t q = ((int32_t)C_ASC(c) - (int32_t)*aw - desc) / 2;
        v = (uint16_t)((uint16_t)-desc - (uint16_t)q);
    } else if (mode != 2) {
        v = (uint16_t)(*aw >> 1);
    }
    *(uint16_t *)org = v;
    return err;
}
TT_EXPORT int tt_sfac_ReadGlyphMetrics(uint8_t *c, uint32_t g, uint16_t *aw, uint16_t *ah, int16_t *lsb,
                                       int16_t *tsb, int16_t *org, uint32_t mode) {
    return ReadGlyphMetrics(c, (uint16_t)g, aw, ah, lsb, tsb, org, (uint8_t)mode);
}

/* ---- sfac_ReadOutlineData (0x18000eac0) ----
 * Simple glyph: endPtsOfContours -> sp/ep, instructions, flags (with repeats), x then y
 * deltas. Points and contours are accumulated over components (totals checked against
 * max(maxPoints, maxCompositePoints) and max(maxContours, maxCompositeContours)). Flags are
 * left with only the on-curve bit. Without outline data (has_outline 0, p8 set) only the
 * counts are checked, against a NULL cursor like the original. */
static int ReadOutlineData(int32_t *npts, uint8_t *flags, int32_t *ys, int32_t *xs, uint8_t *gh,
                           const uint8_t *maxp, uint8_t has_outline, uint8_t p8, int16_t nc, int16_t *sp,
                           int16_t *ep, uint16_t *ilen, const uint8_t **iptr, uint32_t *tot_pts,
                           uint32_t *tot_ctrs) {
    const uint8_t *end = NULL, *cur = NULL, *p = NULL;
    *sp = 0;
    *ep = 0;
    *flags = 1;
    *xs = 0;
    *ys = 0;
    *iptr = NULL;
    *ilen = 0;
    *npts = 0;
    if (!has_outline && !p8) return 0;
    uint16_t lim = LMP_CONTOURS(maxp);
    if (nc <= 0 || (uint16_t)nc > lim) return ERR_SFAC_CONTOURS;
    uint64_t tc = (uint64_t)*tot_ctrs + (uint64_t)(int64_t)nc;
    if (tc > 0xffffffff) overflow();
    *tot_ctrs = (uint32_t)tc;
    if (lim < LMP_CCONTOURS(maxp)) lim = LMP_CCONTOURS(maxp);
    if ((uint32_t)tc > lim) return ERR_SFAC_CONTOURS;
    if (has_outline) {
        end = GH_BASE(gh) + GH_SIZE(gh);
        cur = GH_CUR(gh);
        const uint8_t *a = cur + (uint32_t)nc * 2;
        if ((uintptr_t)a < (uintptr_t)cur) overflow();
        const uint8_t *b = a + 2;
        if (b > end) return ERR_SFAC_GLYPH_DATA;
        *ilen = be16(a);
        *iptr = b;
        p = b + *ilen;
        if ((uintptr_t)p < (uintptr_t)b) overflow();
        *sp = 0;
        *ep = (int16_t)be16(cur);
    }
    int32_t n = (int32_t)ep[0] + 1;
    for (int32_t i = 1;; i++) {
        *npts = n;
        if (i >= nc) break;
        int64_t s = (int64_t)ep[i - 1] + 1;
        if ((uint64_t)(s + 0x8000) > 0xffff) overflow();
        sp[i] = (int16_t)s;
        int16_t e = (int16_t)be16((const uint8_t *)((uintptr_t)cur + 2 * (uint32_t)i));
        ep[i] = e;
        int32_t k = *npts;
        if (k > e || k > (int32_t)LMP_POINTS(maxp) || k <= 0) return ERR_SFAC_POINTS;
        n = e + 1;
    }
    if (n <= 0) return ERR_SFAC_POINTS;
    uint16_t mp = LMP_POINTS(maxp);
    if (n > (int32_t)mp) return ERR_SFAC_POINTS;
    uint64_t tp = (uint64_t)(int64_t)n + *tot_pts;
    if (tp > 0xffffffff) overflow();
    if (mp < LMP_CPOINTS(maxp)) mp = LMP_CPOINTS(maxp);
    *tot_pts = (uint32_t)tp;
    if ((uint32_t)tp > mp) return ERR_SFAC_POINTS;
    if (!has_outline) return 0;

    /* flags */
    uint16_t rep = 0;
    uint8_t *f = flags;
    int32_t left = n;
    while (left > 0) {
        if (rep) {
            left -= rep;
            if (left < 0) return ERR_SFAC_GLYPH_DATA;
            memset(f, f[-1], rep);
            f += rep;
            rep = 0;
            continue;
        }
        if (p + 1 > end) return ERR_SFAC_GLYPH_DATA;
        uint8_t fl = *p;
        *f = fl;
        if (fl & 8) {
            p++;
            if (p + 1 > end) return ERR_SFAC_GLYPH_DATA;
            rep = *p;
        }
        p++;
        f++;
        left--;
    }
    if (rep) return ERR_SFAC_POINTS;

    /* x: bit 1 short (bit 4 sign), else bit 4 same, else int16 delta */
    int16_t v = 0;
    const uint8_t *fp = flags;
    for (int32_t i = 0; i < *npts; i++) {
        uint8_t fl = *fp++;
        if (fl & 2) {
            if (p + 1 > end) return ERR_SFAC_GLYPH_DATA;
            int32_t d = (fl & 0x10) ? (int32_t)v + p[0] : (int32_t)v - p[0];
            if ((uint32_t)(d + 0x8000) > 0xffff) overflow();
            v = (int16_t)d;
            p++;
        } else if (!(fl & 0x10)) {
            if (p + 2 > end) return ERR_SFAC_GLYPH_DATA;
            int32_t d = (int32_t)(int16_t)be16(p) + v;
            if ((uint32_t)(d + 0x8000) > 0xffff) overflow();
            v = (int16_t)d;
            p += 2;
        }
        *xs++ = v;
    }
    /* y: bit 2 short (bit 5 sign), else bit 5 same */
    v = 0;
    for (int32_t i = 0; i < *npts; i++) {
        uint8_t fl = *flags;
        if (fl & 4) {
            if (p + 1 > end) return ERR_SFAC_GLYPH_DATA;
            int32_t d = (fl & 0x20) ? (int32_t)v + p[0] : (int32_t)v - p[0];
            if ((uint32_t)(d + 0x8000) > 0xffff) overflow();
            v = (int16_t)d;
            p++;
        } else if (!(fl & 0x20)) {
            if (p + 2 > end) return ERR_SFAC_GLYPH_DATA;
            int32_t d = (int32_t)(int16_t)be16(p) + v;
            if ((uint32_t)(d + 0x8000) > 0xffff) overflow();
            v = (int16_t)d;
            p += 2;
        }
        *ys++ = v;
        *flags++ &= 1;
    }
    if (p > end) return ERR_SFAC_GLYPH_DATA;
    GH_CUR(gh) = p;
    return 0;
}
TT_EXPORT int tt_sfac_ReadOutlineData(int32_t *npts, uint8_t *flags, int32_t *ys, int32_t *xs, uint8_t *gh,
                                      const uint8_t *maxp, uint32_t has_outline, uint32_t p8, uint32_t nc,
                                      int16_t *sp, int16_t *ep, uint16_t *ilen, const uint8_t **iptr,
                                      uint32_t *tot_pts, uint32_t *tot_ctrs) {
    return ReadOutlineData(npts, flags, ys, xs, gh, maxp, (uint8_t)has_outline, (uint8_t)p8, (int16_t)nc,
                           sp, ep, ilen, iptr, tot_pts, tot_ctrs);
}

/* ---- sfac_ReadComponentData (0x1800da400) ----
 * One composite component record. m is the 3x3 TransMatrix (16.16, F2Dot14 scales << 2).
 * *scaled is only written when SCALED (0x800) or UNSCALED (0x1000) component offset is set. */
static int ReadComponentData(uint8_t *gh, int32_t *xy_args, uint8_t *round_xy, uint8_t *use_metrics,
                             uint8_t *scaled, uint8_t *has_instr, uint16_t *glyph, int16_t *arg1,
                             int16_t *arg2, uint16_t *pt1, uint16_t *pt2, int32_t *m, uint8_t *has_xform,
                             uint8_t *last) {
    const uint8_t *c = GH_CUR(gh);
    const uint8_t *end = GH_BASE(gh) + GH_SIZE(gh);
    static const int32_t identity[9] = {0x10000, 0, 0, 0, 0x10000, 0, 0, 0, 0x10000};
    memcpy(m, identity, sizeof identity);
    *arg1 = 0;
    *arg2 = 0;
    *pt1 = 0;
    *pt2 = 0;
    *has_xform = 0;
    const uint8_t *q = c + 2;
    if (q > end) return ERR_SFAC_GLYPH_DATA;
    uint16_t fl = be16(c);
    *has_instr = (fl >> 8) & 1;
    *use_metrics = (fl >> 9) & 1;
    *round_xy = (fl >> 2) & 1;
    if (fl & 0x800) *scaled = 1;
    if (fl & 0x1000) *scaled = 0;
    const uint8_t *a = q + 2;
    if (a > end) return ERR_SFAC_GLYPH_DATA;
    *glyph = be16(q);
    *xy_args = (fl & 2) != 0;
    const uint8_t *d;
    if (fl & 1) {
        if (a + 4 > end) return ERR_SFAC_GLYPH_DATA;
        if (fl & 2) {
            *arg1 = (int16_t)be16(a);
            *arg2 = (int16_t)be16(a + 2);
        } else {
            *pt1 = be16(a);
            *pt2 = be16(a + 2);
        }
        d = a + 4;
    } else {
        d = a + 2;
        if (d > end) return ERR_SFAC_GLYPH_DATA;
        if (fl & 2) {
            *arg1 = (int8_t)a[0];
            *arg2 = (int8_t)a[1];
        } else {
            *pt1 = a[0];
            *pt2 = a[1];
        }
    }
    if (fl & 0xc8) {
        *has_xform = 1;
        if (fl & 0x80) {                      /* WE_HAVE_A_TWO_BY_TWO */
            if (d + 8 > end) return ERR_SFAC_GLYPH_DATA;
            m[0] = (int32_t)(int16_t)be16(d) * 4;
            m[1] = (int32_t)(int16_t)be16(d + 2) * 4;
            m[3] = (int32_t)(int16_t)be16(d + 4) * 4;
            m[4] = (int32_t)(int16_t)be16(d + 6) * 4;
            d += 8;
        } else {
            m[1] = 0;
            m[3] = 0;
            if (d + 2 > end) return ERR_SFAC_GLYPH_DATA;
            int32_t s = (int32_t)(int16_t)be16(d) * 4;
            m[0] = s;
            d += 2;
            if (fl & 0x40) {                  /* WE_HAVE_AN_X_AND_Y_SCALE */
                if (d + 2 > end) return ERR_SFAC_GLYPH_DATA;
                m[4] = (int32_t)(int16_t)be16(d) * 4;
                d += 2;
            } else {
                m[4] = s;
            }
        }
    }
    *last = (uint8_t)(~(fl >> 5) & 1);        /* !MORE_COMPONENTS */
    GH_CUR(gh) = d;
    return end < d ? ERR_SFAC_GLYPH_DATA : 0;
}
TT_EXPORT int tt_sfac_ReadComponentData(uint8_t *gh, int32_t *xy_args, uint8_t *round_xy, uint8_t *use_metrics,
                                        uint8_t *scaled, uint8_t *has_instr, uint16_t *glyph, int16_t *arg1,
                                        int16_t *arg2, uint16_t *pt1, uint16_t *pt2, int32_t *m,
                                        uint8_t *has_xform, uint8_t *last) {
    return ReadComponentData(gh, xy_args, round_xy, use_metrics, scaled, has_instr, glyph, arg1, arg2, pt1,
                             pt2, m, has_xform, last);
}

/* ---- sfac_ReadCompositeInstructions (0x180002fd8) ---- */
static int ReadCompositeInstructions(uint8_t *gh, const uint8_t **iptr, uint16_t *ilen) {
    const uint8_t *end = GH_BASE(gh) + GH_SIZE(gh);
    const uint8_t *c = GH_CUR(gh);
    const uint8_t *b = c + 2;
    if (b > end) return ERR_SFAC_GLYPH_DATA;
    *ilen = be16(c);
    *iptr = b;
    const uint8_t *n = b + *ilen;
    if ((uintptr_t)n < (uintptr_t)b) overflow();
    GH_CUR(gh) = n;
    return end < n ? ERR_SFAC_GLYPH_DATA : 0;
}
TT_EXPORT int tt_sfac_ReadCompositeInstructions(uint8_t *gh, const uint8_t **iptr, uint16_t *ilen) {
    return ReadCompositeInstructions(gh, iptr, ilen);
}

/* ---- sfac_CopyCVT (0x18000c0f0) ----
 * int16 cvt -> int32, then the variation callback, then zero fill up to 256 entries. With a
 * NULL fragment for a non-empty cvt the original reads through NULL; so does the port. */
static int CopyCVT(uint8_t *c, int32_t *cvt) {
    uint32_t len = C_TLEN(c, T_CVT);
    void *ctx = NULL;
    const uint8_t *p = NULL;
    int32_t i = 0;
    if (len != 0) {
        if (len > 0x7fffffff || C_TOFF(c, T_CVT) > 0x7fffffff) overflow();
        p = get_frag(c, C_TOFF(c, T_CVT), len, &ctx);
        if (!p) OutputDebugStringA(kBogusSize);
        int32_t n = (int32_t)(len >> 1);
        if (!p && n > 0) {
            /* the original reads through NULL; its unwinding (/EHa) releases the fragment */
            release(c, ctx);
            (void)*(volatile const uint8_t *)g_null;
        }
        for (; i < n; i++) {
            cvt[i] = (int16_t)be16(p);
            if (i == 0x7fffffff) overflow_release(c, ctx, NULL, NULL);
            p += 2;
        }
        C_CVTVAR(c)(C_ID(c), cvt, len >> 1);
        if (i >= 0x100) {
            release(c, ctx);
            return 0;
        }
    }
    int64_t rest = 0x100 - (int64_t)i;      /* SafeInt<int> (256 - i) * 4, non-negative here */
    memset(cvt + i, 0, (size_t)(rest * 4));
    release(c, ctx);
    return 0;
}
TT_EXPORT int tt_sfac_CopyCVT(uint8_t *c, int32_t *cvt) { return CopyCVT(c, cvt); }

/* ---- sfac_CopyFontAndPrePrograms (0x18000bc38) ---- */
static int CopyFontAndPrePrograms(uint8_t *c, void *fpgm, uint32_t fpgm_cap, void *prep, uint32_t prep_cap) {
    void *ctx_f = NULL, *ctx_p = NULL;
    uint32_t lf = C_TLEN(c, T_FPGM);
    const uint8_t *f = NULL;
    if (lf != 0) {
        if (lf > 0x7fffffff || C_TOFF(c, T_FPGM) > 0x7fffffff) overflow();
        f = get_frag(c, C_TOFF(c, T_FPGM), lf, &ctx_f);
        if (!f) OutputDebugStringA(kBogusSize);
        if (!f || lf > fpgm_cap) {
            release(c, ctx_f);
            return ERR_SFAC_1400;
        }
        memcpy(fpgm, f, lf);
    }
    uint32_t lp = C_TLEN(c, T_PREP);
    const uint8_t *p = NULL;
    if (lp != 0) {
        if (lp > 0x7fffffff || C_TOFF(c, T_PREP) > 0x7fffffff) overflow_release(c, ctx_f, NULL, NULL);
        p = get_frag(c, C_TOFF(c, T_PREP), lp, &ctx_p);
        if (!p) OutputDebugStringA(kBogusSize);
        if (!p || lp > prep_cap) {
            release(c, ctx_p);
            release(c, ctx_f);
            return ERR_SFAC_1400;
        }
        memcpy(prep, p, lp);
    }
    release(c, ctx_p);
    release(c, ctx_f);
    return 0;
}
TT_EXPORT int tt_sfac_CopyFontAndPrePrograms(uint8_t *c, void *fpgm, uint32_t fpgm_cap, void *prep,
                                             uint32_t prep_cap) {
    return CopyFontAndPrePrograms(c, fpgm, fpgm_cap, prep, prep_cap);
}

/* ---- sfac_ReadNumLongVertMetrics (0x18009101c): *n is left alone without vhea ----
 * held: the fragments of the caller (sfac_LoadCriticalSfntMetrics) released on overflow. */
static int ReadNumLongVertMetrics(uint8_t *c, uint16_t *n, uint8_t *has, void *const held[4]) {
    *has = 0;
    void *ctx = NULL;
    const uint8_t *v = read_table(c, T_VHEA, 0x24, &ctx, NULL, held[0], held[1], held[2], held[3]);
    if (v) {
        *n = be16(v + 0x22);
        *has = 1;
    }
    release(c, ctx);
    return 0;
}
TT_EXPORT int tt_sfac_ReadNumLongVertMetrics(uint8_t *c, uint16_t *n, uint8_t *has) {
    void *const none[4] = {NULL, NULL, NULL, NULL};
    return ReadNumLongVertMetrics(c, n, has, none);
}

/* ---- sfac_LoadCriticalSfntMetrics (0x180091854) ----
 * head, hhea, OS/2 (optional), maxp -> client record and LocalMaxProfile. Fragments are
 * released in reverse order of acquisition. */
static int LoadCriticalSfntMetrics(uint8_t *c, uint16_t *upem, uint8_t *int_ppem, uint8_t *lmp) {
    void *ctx_head = NULL, *ctx_hhea = NULL, *ctx_os2 = NULL, *ctx_maxp = NULL;
    int err;
    const uint8_t *head = read_table(c, T_HEAD, 0x36, &ctx_head, &err, NULL, NULL, NULL, NULL);
    if (err) {
        release(c, ctx_head);
        return err;
    }
    const uint8_t *hhea = read_table(c, T_HHEA, 0x24, &ctx_hhea, &err, ctx_head, NULL, NULL, NULL);
    if (!err) {
        if (be32(head + 0xc) != 0x5f0f3cf5) {
            err = ERR_SFAC_MAGIC;
        } else {
            uint16_t u = be16(head + 0x12);
            *upem = u;
            if ((uint16_t)(u - 0x10) > 0x3ff0) {
                err = ERR_SFAC_UNITS_PER_EM;
            } else {
                *int_ppem = (head[0x11] >> 3) & 1;
                uint16_t nhm = be16(hhea + 0x22);
                C_NHMETRICS(c) = nhm;
                if (nhm == 0) err = ERR_SFAC_HMTX;
            }
        }
    }
    if (err) {
        release(c, ctx_hhea);
        release(c, ctx_head);
        return err;
    }
    C_LOCFORMAT(c) = (int16_t)be16(head + 0x32);
    const uint8_t *os2 = read_table(c, T_OS2, 0x4e, &ctx_os2, NULL, ctx_hhea, ctx_head, NULL, NULL);
    if (os2) {
        C_ASC(c) = (int16_t)be16(os2 + 0x44);
        C_DESC(c) = (int16_t)be16(os2 + 0x46);
    } else {
        C_ASC(c) = (int16_t)be16(hhea + 4);
        C_DESC(c) = (int16_t)be16(hhea + 6);
    }
    AT(c, 0xf4, int16_t) = (int16_t)be16(hhea + 6);
    const uint8_t *mx = read_table(c, T_MAXP, 0x20, &ctx_maxp, &err, ctx_os2, ctx_hhea, ctx_head, NULL);
    if (!err) {
        AT(lmp, 0x00, uint32_t) = be32(mx);
        for (int k = 4; k < 0x20; k += 2) AT(lmp, k, uint16_t) = be16(mx + k);
        if (AT(lmp, 0x18, uint16_t) == 0) AT(lmp, 0x18, uint16_t) = 1;  /* maxStackElements */
        uint16_t pts = AT(lmp, 0x06, uint16_t), cpts = AT(lmp, 0x0a, uint16_t);
        uint16_t twilight = AT(lmp, 0x10, uint16_t);
        if ((pts < cpts ? cpts : pts) > 0xfff6) {
            err = ERR_SFAC_MAXP;
        } else {
            if (pts > 0x7ff6) AT(lmp, 0x06, uint16_t) = 0x7ff6;
            if (cpts > 0x7ff6) AT(lmp, 0x0a, uint16_t) = 0x7ff6;
            if (twilight > 0x7fff) AT(lmp, 0x10, uint16_t) = 0x7fff;
            void *const held[4] = {ctx_maxp, ctx_os2, ctx_hhea, ctx_head};
            err = ReadNumLongVertMetrics(c, &C_NVMETRICS(c), &C_HASVMTX(c), held);
        }
    }
    release(c, ctx_maxp);
    release(c, ctx_os2);
    release(c, ctx_hhea);
    release(c, ctx_head);
    return err;
}
TT_EXPORT int tt_sfac_LoadCriticalSfntMetrics(uint8_t *c, uint16_t *upem, uint8_t *int_ppem, uint8_t *lmp) {
    return LoadCriticalSfntMetrics(c, upem, int_ppem, lmp);
}

/* ---- sfac_Classify (0x180091fc0) ---- */
#ifdef IDWR_STANDALONE
/* every caller is a C port: the export is the C function itself, no register-preserving entry */
#define tt_sfac_Classify_impl tt_sfac_Classify
TT_EXPORT
#endif
__attribute__((used)) void tt_sfac_Classify_impl(uint8_t *dir, const uint8_t *rec) {
    int t;
    switch (be32(rec)) {
    case 0x68656164: t = T_HEAD; break;  /* head */
    case 0x68686561: t = T_HHEA; break;  /* hhea */
    case 0x6c6f6361: t = T_LOCA; break;  /* loca */
    case 0x6d617870: t = T_MAXP; break;  /* maxp */
    case 0x63767420: t = T_CVT; break;   /* cvt  */
    case 0x70726570: t = T_PREP; break;  /* prep */
    case 0x676c7966: t = T_GLYF; break;  /* glyf */
    case 0x686d7478: t = T_HMTX; break;  /* hmtx */
    case 0x636d6170: t = T_CMAP; break;  /* cmap */
    case 0x6670676d: t = T_FPGM; break;  /* fpgm */
    case 0x68646d78: t = 0xb; break;     /* hdmx */
    case 0x4c545348: t = 0xc; break;     /* LTSH */
    case 0x4f532f32: t = T_OS2; break;   /* OS/2 */
    case 0x67646972: t = T_GDIR; break;  /* gdir */
    case 0x45424454: t = 0x10; break;    /* EBDT */
    case 0x45424c43: t = 0x11; break;    /* EBLC */
    case 0x45425343: t = 0x12; break;    /* EBSC */
    case 0x76686561: t = T_VHEA; break;  /* vhea */
    case 0x766d7478: t = T_VMTX; break;  /* vmtx */
    case 0x66766172: t = 0x15; break;    /* fvar */
    default: return;
    }
    AT(dir, 8 * t, uint32_t) = be32(rec + 8);
    AT(dir, 8 * t + 4, uint32_t) = be32(rec + 12);
}
/* The original sfac_DoOffsetTableMap keeps its loop state in r10/r11 across the call (the
 * original Classify does not touch them), so the swappable entry preserves both. */
#ifndef IDWR_STANDALONE
__attribute__((naked)) TT_EXPORT void tt_sfac_Classify(uint8_t *dir, const uint8_t *rec) {
    __asm__ volatile(
        "push %r10\n\t"
        "push %r11\n\t"
        "sub $0x28, %rsp\n\t"
        "call tt_sfac_Classify_impl\n\t"
        "add $0x28, %rsp\n\t"
        "pop %r11\n\t"
        "pop %r10\n\t"
        "ret\n\t");
}
#endif

/* ---- sfac_DoOffsetTableMap (0x180091e38) ----
 * numTables is read as a signed int16 (a count >= 0x8000 maps no table). */
static int DoOffsetTableMap(uint8_t *c) {
    void *ctx1 = NULL, *ctx2 = NULL;
    const uint8_t *h = get_frag(c, 0, 0xc, &ctx1);
    if (!h) {
        release(c, ctx1);
        return ERR_SFAC_BAD_TABLE_DIR;
    }
    int32_t n = (int16_t)be16(h + 4);
    int64_t sz = (int64_t)n * 16 + 0xc;      /* SafeInt checks cannot fire for an int16 count */
    const uint8_t *d = get_frag(c, 0, (uint32_t)sz, &ctx2);
    release(c, ctx1);
    if (!d) {
        release(c, ctx2);
        return ERR_SFAC_NULL_FRAGMENT;
    }
    memset(c + 0x20, 0, 0xb8);
    for (int32_t i = 0; i < n; i++) tt_sfac_Classify_impl(c + 0x20, d + 0xc + 16 * i);
    AT(c, 0xd0, uint32_t) = 0;
    AT(c, 0xd4, uint32_t) = 0xffffffff;
    release(c, ctx2);
    return 0;
}
TT_EXPORT int tt_sfac_DoOffsetTableMap(uint8_t *c) { return DoOffsetTableMap(c); }

/* ---- sfac_ComputeBinarySearchParams (0x18014d4c8) ---- */
static void ComputeBinarySearchParams(uint16_t n, uint16_t *range, uint16_t *sel, uint16_t *shift) {
    uint16_t r = 1, e = 0;
    if (n >= 2) {
        do {
            r = (uint16_t)(r * 2);
            e++;
        } while ((uint32_t)r * 2 <= n);
    }
    *range = (uint16_t)(r * 2);
    *sel = e;
    *shift = (uint16_t)((n - r) * 2);
}
TT_EXPORT void tt_sfac_ComputeBinarySearchParams(uint32_t n, uint16_t *range, uint16_t *sel, uint16_t *shift) {
    ComputeBinarySearchParams((uint16_t)n, range, sel, shift);
}

/* ---- sfac_ComputeMapping (0x1800927b4) ----
 * Finds the (platform, encoding) cmap subtable; platform 0xffff means glyph indices are used
 * directly. The client record keeps the original's sfac_ComputeIndex* function pointers. */
static int ComputeMapping(uint8_t *c, uint16_t platform, uint16_t encoding) {
    if (platform == 0xffff) {
        C_COMPUTEIDX(c) = idwr_orig(RVA_ComputeIndexNone);
        return 0;
    }
    uint32_t len = C_TLEN(c, T_CMAP);
    const uint8_t *p = NULL;
    void *ctx = NULL;
    int err = GetDataPtr(c, 0, len, T_CMAP, 0, &p, &ctx);
    if (err) {
        release(c, ctx);
        return err;
    }
    uint32_t fn = RVA_ComputeIndexNone;
    if (p) {
        if (len < 4) {
            release(c, ctx);
            return ERR_SFAC_CMAP;
        }
        const uint8_t *r = p + 4;
        uint32_t span = (uint32_t)be16(p + 2) * 8;           /* SafeInt<uint16> */
        if (span > 0xffff) overflow_release(c, ctx, NULL, NULL);
        const uint8_t *rend = r + span;
        if ((uintptr_t)rend < (uintptr_t)r) overflow_release(c, ctx, NULL, NULL);
        int found = 0;
        while (r < rend && !found) {
            if (len < 8) overflow_release(c, ctx, NULL, NULL); /* SafeInt<uint32> len - 8 */
            const uint8_t *lim = p + (len - 8);
            if ((uintptr_t)lim < (uintptr_t)p) overflow_release(c, ctx, NULL, NULL);
            if (r > lim) {
                release(c, ctx);
                return ERR_SFAC_CMAP;
            }
            if (be16(r) == platform && be16(r + 2) == encoding) {
                found = 1;
                C_CMAPOFF(c) = be32(r + 4);
            }
            r += 8;
        }
        if (!found) {
            C_CMAPOFF(c) = 0;
            err = ERR_SFAC_CMAP;
        } else {
            uint32_t off = C_CMAPOFF(c);
            const uint8_t *s = p + off;
            if ((uintptr_t)s < (uintptr_t)p) overflow_release(c, ctx, NULL, NULL);
            if ((uint64_t)off + 6 > 0xffffffff) overflow_release(c, ctx, NULL, NULL);
            C_CMAPOFF(c) = off + 6;                          /* past format/length/language */
            if (len < 2) overflow_release(c, ctx, NULL, NULL);
            const uint8_t *lim = p + (len - 2);
            if ((uintptr_t)lim < (uintptr_t)p) overflow_release(c, ctx, NULL, NULL);
            if (s > lim) {
                C_CMAPOFF(c) = 0;
                err = ERR_SFAC_CMAP;
            } else {
                uint16_t format = be16(s);
                C_CMAPFMT(c) = format;
                if (format == 0) {
                    fn = RVA_sfac_ComputeIndex0;
                } else if (format == 2) {
                    fn = RVA_sfac_ComputeIndex2;
                } else if (format == 4) {
                    C_COMPUTEIDX(c) = idwr_orig(RVA_sfac_ComputeIndex4);
                    const uint8_t *q = p + C_CMAPOFF(c);     /* segCountX2 */
                    if ((uintptr_t)q < (uintptr_t)p) overflow_release(c, ctx, NULL, NULL);
                    if (q <= lim) {
                        ComputeBinarySearchParams((uint16_t)(be16(q) >> 1), &AT(c, 0xea, uint16_t),
                                                  &AT(c, 0xec, uint16_t), &AT(c, 0xee, uint16_t));
                        release(c, ctx);
                        return err;
                    }
                    C_CMAPOFF(c) = 0;
                    err = ERR_SFAC_CMAP;
                } else if (format == 6) {
                    fn = RVA_sfac_ComputeIndex6;
                } else {
                    err = ERR_SFAC_CMAP_FORMAT;
                }
            }
        }
    }
    C_COMPUTEIDX(c) = idwr_orig(fn);
    release(c, ctx);
    return err;
}
TT_EXPORT int tt_sfac_ComputeMapping(uint8_t *c, uint32_t platform, uint32_t encoding) {
    return ComputeMapping(c, (uint16_t)platform, (uint16_t)encoding);
}
