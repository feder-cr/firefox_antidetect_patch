/*
 * Scan converter, last stage: filling the 1-bit bitmap from the scan-line crossing lists and
 * dropout control. Ported from DWrite.dll 10.0.26100.9711: fsc_FillBitMap, LookForDropouts,
 * DoVertDropout, DoHorizDropout, GetBitAbs, SetBitAbs, HorizCrossings, VertCrossings.
 *
 * StateVars (sv) fields used, by offset:
 *   0x18 xMin, 0x1c xMax (columns [xMin, xMax)), 0x20 yMax, 0x24 yMin (rows [yMin, yMax)),
 *   0x28 bitmap row bytes (stored by fsc_FillBitMap),
 *   0x30/0x38/0x40 per row (index y - yMin): ON list start, OFF list start, ON list end,
 *   0x70/0x78/0x80/0x88 per column (index x - xMin): ON start, OFF start, ON end, OFF end,
 *   0xc0[3]/0xd8[3] smart-dropout callbacks (horizontal/vertical) by edge type (low 2 bits of
 *   the crossing's second word; type 3 = none), called as cb(scan, &0xf8[i], &0x100[i]) with
 *   i = second word >> 2,
 *   0xf0 bit order (0: most significant bit first in each byte, else least significant first),
 *   0x112 stride between crossings of one list, in int16 units.
 * A crossing is int16 coordinate (pixel index) followed by an int16 edge reference.
 * The bitmap is stored top row first: row y is at (yMax - 1 - y) * rowBytes.
 *
 * Dropout flags (fsc_FillGlyph's scan control word): 1 = exclude stubs (the crossing counts
 * around the dropout must show a continuing contour), 2 = no dropout control, 4 = smart
 * dropout (the pixel nearest the midpoint of the two edges, from the callbacks, instead of the
 * pixel left/below).
 *
 * Calls between these functions go through the original entry points (ORIG): the oracle's
 * function swap patches those entries, so one function can be swapped alone and every call
 * reaches whichever version is swapped in. Where the original inlines a helper (DoVertDropout
 * inlines VertCrossings, HorizCrossings, GetBitAbs and SetBitAbs), the port uses the static
 * helper below, with the same checks.
 */
#include <string.h>
#include "tt_state.h"
#include "orig.h"

#define SV_XMIN(sv)     AT(sv, 0x18, int32_t)
#define SV_XMAX(sv)     AT(sv, 0x1c, int32_t)
#define SV_YMAX(sv)     AT(sv, 0x20, int32_t)
#define SV_YMIN(sv)     AT(sv, 0x24, int32_t)
#define SV_ROWBYTES(sv) AT(sv, 0x28, int32_t)
#define SV_ROW_ON(sv)   AT(sv, 0x30, int16_t **)
#define SV_ROW_OFF(sv)  AT(sv, 0x38, int16_t **)
#define SV_ROW_END(sv)  AT(sv, 0x40, int16_t **)
#define SV_COL_ON(sv)   AT(sv, 0x70, int16_t **)
#define SV_COL_OFF(sv)  AT(sv, 0x78, int16_t **)
#define SV_COL_END(sv)  AT(sv, 0x80, int16_t **)
#define SV_COL_OEND(sv) AT(sv, 0x88, int16_t **)
#define SV_HCB(sv, t)   AT(sv, 0xc0 + 8 * (t), dropout_cb)
#define SV_VCB(sv, t)   AT(sv, 0xd8 + 8 * (t), dropout_cb)
#define SV_LSBFIRST(sv) AT(sv, 0xf0, uint8_t)
#define SV_CBA(sv)      AT(sv, 0xf8, uint8_t *)
#define SV_CBB(sv)      AT(sv, 0x100, uint8_t *)
#define SV_STRIDE(sv)   AT(sv, 0x112, int16_t)

#define ERR_SCAN 0x1302 /* dropout outside the bitmap, or an edge without a callback */

typedef int (*dropout_cb)(int scan, void *a, void *b);
typedef void (*blt_fn)(int x1, int x2, uint32_t *row);

/* original entry points (RVA) */
#define RVA_SAFEINT_OVERFLOW 0x1090b0 /* SafeIntExceptionHandler<...>::SafeIntOnOverflow, throws */
#define RVA_BLT_HORIZ        0x151200 /* fsc_BLTHoriz */
#define RVA_BLT_HORIZ_REV    0x09a7f0 /* fsc_BLTHorizReverseBitOrder */
#define RVA_LOOK_FOR_DROPOUTS 0x70bd0
#define RVA_DO_VERT_DROPOUT  0x70e50
#define RVA_DO_HORIZ_DROPOUT 0x717f0
#define RVA_GET_BIT_ABS      0x71a08
#define RVA_SET_BIT_ABS      0x71b5c
#define RVA_HORIZ_CROSSINGS  0x71ca8
#define RVA_VERT_CROSSINGS   0x71d60

/* SafeInt checks of the original: on overflow it throws (SafeIntOnOverflow); the port calls
 * the same function so the behaviour stays the same. None fires for a valid scan state. */
static int overflow(void) {
    ORIG(void (*)(void), RVA_SAFEINT_OVERFLOW)();
    return 0;
}

static inline int fits32(int64_t v) { return (uint64_t)(v + 0x80000000LL) <= 0xffffffffULL; }
static inline int fits16(int32_t v) { return (uint32_t)v + 0x8000u <= 0xffffu; }

/* SafeInt pointer + signed offset: overflow if the address wraps. */
static uint8_t *ptr_add(uint8_t *base, int64_t off) {
    uintptr_t b = (uintptr_t)base;
    if (off >= 0) {
        uintptr_t r = b + (uint64_t)off;
        if (r < b) overflow();
        return (uint8_t *)r;
    }
    uint64_t n = 0 - (uint64_t)off;
    if (n > b) overflow();
    return (uint8_t *)(b - n);
}

/* Address of bitmap row y (rows stored top first), with the original's SafeInt checks. */
static uint32_t *row_ptr(uint8_t *sv, void *bitmap, int32_t y) {
    int64_t t = (int64_t)SV_YMAX(sv) - 1;
    if (!fits32(t)) overflow();
    int64_t d = (int64_t)(int32_t)t - (int64_t)y;
    if (!fits32(d)) overflow();
    int64_t m = (int64_t)SV_ROWBYTES(sv) * (int64_t)(int32_t)d;
    if (!fits32(m)) overflow();
    return (uint32_t *)ptr_add((uint8_t *)bitmap, (int32_t)m);
}

static inline uint32_t bit_mask(uint8_t *sv, int32_t c) {
    if (SV_LSBFIRST(sv)) return 1u << (c & 31);
    return __builtin_bswap32(0x80000000u >> (c & 31));
}

/* GetBitAbs: the bitmap's bit at (x, y) as the masked word (nonzero if set), 0 outside. */
static uint32_t get_bit(uint8_t *sv, void *bitmap, int32_t x, int32_t y) {
    if (x < SV_XMIN(sv) || x >= SV_XMAX(sv) || y < SV_YMIN(sv) || y >= SV_YMAX(sv)) return 0;
    uint32_t *row = row_ptr(sv, bitmap, y);
    int32_t c = (int32_t)((uint32_t)x - (uint32_t)SV_XMIN(sv));
    return bit_mask(sv, c) & row[(int64_t)c >> 5];
}

/* SetBitAbs: sets the bit at (x, y); ERR_SCAN outside the bitmap. */
static int set_bit(uint8_t *sv, void *bitmap, int32_t x, int32_t y) {
    if (x < SV_XMIN(sv) || x >= SV_XMAX(sv) || y < SV_YMIN(sv) || y >= SV_YMAX(sv)) return ERR_SCAN;
    uint32_t *row = row_ptr(sv, bitmap, y);
    int32_t c = (int32_t)((uint32_t)x - (uint32_t)SV_XMIN(sv));
    row[(int64_t)c >> 5] |= bit_mask(sv, c);
    return 0;
}

/* Count of the crossings at coordinate v in two interleaved lists (ON from a until end, OFF
 * from b, stepped together). */
static int count_crossings(uint8_t *sv, const int16_t *a, const int16_t *b, const int16_t *end, int16_t v) {
    int n = 0;
    ptrdiff_t s = SV_STRIDE(sv);
    while ((uintptr_t)a < (uintptr_t)end) {
        if (*a == v) n++;
        a += s;
        if (*b == v) n++;
        b += s;
    }
    return n;
}

/* HorizCrossings: crossings of scan line y at column x. */
static int horiz_crossings(uint8_t *sv, int32_t x, int32_t y) {
    if (y < SV_YMIN(sv) || y >= SV_YMAX(sv)) return 0;
    int64_t i = (int64_t)y - SV_YMIN(sv);
    if (!fits32(i)) overflow();
    const int16_t *a = SV_ROW_ON(sv)[(int32_t)i], *b = SV_ROW_OFF(sv)[(int32_t)i], *end = SV_ROW_END(sv)[(int32_t)i];
    if (!fits16(x)) overflow();
    return count_crossings(sv, a, b, end, (int16_t)x);
}

/* VertCrossings: crossings of scan column x at row y. */
static int vert_crossings(uint8_t *sv, int32_t x, int32_t y) {
    if (x < SV_XMIN(sv) || x >= SV_XMAX(sv)) return 0;
    int64_t i = (int64_t)x - SV_XMIN(sv);
    if (!fits32(i)) overflow();
    const int16_t *a = SV_COL_ON(sv)[(int32_t)i], *b = SV_COL_OFF(sv)[(int32_t)i], *end = SV_COL_END(sv)[(int32_t)i];
    if (!fits16(y)) overflow();
    return count_crossings(sv, a, b, end, (int16_t)y);
}

TT_EXPORT uint32_t tt_GetBitAbs(uint8_t *sv, void *bitmap, int x, int y) { return get_bit(sv, bitmap, x, y); }
TT_EXPORT int tt_SetBitAbs(uint8_t *sv, void *bitmap, int x, int y) { return set_bit(sv, bitmap, x, y); }
TT_EXPORT int tt_HorizCrossings(uint8_t *sv, int x, int y) { return horiz_crossings(sv, x, y); }
TT_EXPORT int tt_VertCrossings(uint8_t *sv, int x, int y) { return vert_crossings(sv, x, y); }

#define GetBitAbs      ORIG(uint32_t (*)(uint8_t *, void *, int, int), RVA_GET_BIT_ABS)
#define SetBitAbs      ORIG(int (*)(uint8_t *, void *, int, int), RVA_SET_BIT_ABS)
#define HorizCrossings ORIG(int (*)(uint8_t *, int, int), RVA_HORIZ_CROSSINGS)
#define VertCrossings  ORIG(int (*)(uint8_t *, int, int), RVA_VERT_CROSSINGS)
#define DoHorizDropout ORIG(int (*)(uint8_t *, int16_t *, int16_t *, int, void *, uint16_t), RVA_DO_HORIZ_DROPOUT)
#define DoVertDropout  ORIG(int (*)(uint8_t *, int16_t *, int16_t *, int, void *, uint16_t), RVA_DO_VERT_DROPOUT)
#define LookForDropouts ORIG(int (*)(uint8_t *, void *, uint16_t), RVA_LOOK_FOR_DROPOUTS)

/* Smart dropout: an edge's exact crossing of the scan line/column, from the callback for its
 * type (26.6 or finer units; the caller averages two and shifts by 7). */
#define EDGE_CB(table, sv, e, scan, out)                                                   \
    do {                                                                                   \
        uint16_t w_ = (uint16_t)(e)[1];                                                    \
        if ((w_ & 3) == 3) return ERR_SCAN;                                                \
        (out) = table(sv, w_ & 3)(scan, SV_CBA(sv) + (size_t)(w_ >> 2) * 4,                \
                                  SV_CBB(sv) + (size_t)(w_ >> 2) * 4);                     \
    } while (0)

/* DoHorizDropout: on scan line y the ON and OFF crossings fall on the same pixel column x
 * (no pixel centre between them). Unless a neighbouring pixel is already set, set one. */
TT_EXPORT int tt_DoHorizDropout(uint8_t *sv, int16_t *on, int16_t *off, int y, void *bitmap, uint16_t flags) {
    int32_t x = on[0];
    int32_t x0 = (int32_t)((uint32_t)x - 1);
    if (flags & 1) { /* stubs: skip unless the contour continues both above and below */
        if (HorizCrossings(sv, x, y + 1) + VertCrossings(sv, x0, y + 1) + VertCrossings(sv, x, y + 1) < 2)
            return 0;
        if (HorizCrossings(sv, x, y - 1) + VertCrossings(sv, x0, y) + VertCrossings(sv, x, y) < 2)
            return 0;
    }
    if ((x > SV_XMIN(sv) && GetBitAbs(sv, bitmap, x0, y) != 0) ||
        (x < SV_XMAX(sv) && GetBitAbs(sv, bitmap, x, y) != 0))
        return 0;
    if (flags & 4) {
        int r1, r2;
        EDGE_CB(SV_HCB, sv, on, y, r1);
        EDGE_CB(SV_HCB, sv, off, y, r2);
        x0 = (int32_t)((uint32_t)r1 + ((uint32_t)r2 - 1)) >> 7;
    }
    int32_t c = x0 >= SV_XMIN(sv) ? x0 : SV_XMIN(sv);
    if (c >= SV_XMAX(sv)) {
        int64_t t = (int64_t)SV_XMAX(sv) - 1;
        if (!fits32(t)) overflow();
        c = (int32_t)t;
    }
    return SetBitAbs(sv, bitmap, c, y);
}

/* DoVertDropout: in scan column x the ON and OFF crossings fall on the same pixel row y.
 * The original inlines the crossing counts and the bit get/set. */
TT_EXPORT int tt_DoVertDropout(uint8_t *sv, int16_t *on, int16_t *off, int x, void *bitmap, uint16_t flags) {
    int32_t y = on[0];
    if (y < SV_YMIN(sv) || y > SV_YMAX(sv)) return 0;
    if (flags & 1) { /* stubs: skip unless the contour continues left and right */
        int32_t xm = (int32_t)((uint32_t)x - 1), xp = (int32_t)((uint32_t)x + 1);
        int a = vert_crossings(sv, xm, y);
        a += horiz_crossings(sv, x, y);
        if (horiz_crossings(sv, x, y - 1) + a < 2) return 0;
        a = vert_crossings(sv, xp, y);
        a += horiz_crossings(sv, xp, y);
        if (horiz_crossings(sv, xp, y - 1) + a < 2) return 0;
    }
    if (y > SV_YMIN(sv) && get_bit(sv, bitmap, x, y - 1) != 0) return 0;
    if (y < SV_YMAX(sv) && get_bit(sv, bitmap, x, y) != 0) return 0;
    int32_t y0;
    if (flags & 4) {
        int r1, r2;
        EDGE_CB(SV_VCB, sv, on, x, r1);
        EDGE_CB(SV_VCB, sv, off, x, r2);
        y0 = (int32_t)((uint32_t)r1 + ((uint32_t)r2 - 1)) >> 7;
    } else {
        y0 = y - 1;
    }
    int32_t c = y0 >= SV_YMIN(sv) ? y0 : SV_YMIN(sv);
    if (c >= SV_YMAX(sv)) {
        int64_t t = (int64_t)SV_YMAX(sv) - 1;
        if (!fits32(t)) overflow();
        c = (int32_t)t;
    }
    if (c < SV_YMIN(sv) || c >= SV_YMAX(sv)) return 0; /* empty row range: nothing to set */
    return set_bit(sv, bitmap, x, c);                     /* ERR_SCAN if x is outside */
}

/* LookForDropouts: every ON/OFF crossing pair on the same pixel, rows (top first) then
 * columns (left first, each from its list end). */
TT_EXPORT int tt_LookForDropouts(uint8_t *sv, void *bitmap, uint16_t flags) {
    int64_t n = (int64_t)SV_YMAX(sv) - SV_YMIN(sv);
    if (!fits32(n)) return overflow();
    int64_t last = (int64_t)(int32_t)n - 1;
    if (!fits32(last)) return overflow();
    int16_t **pon = SV_ROW_ON(sv) + (int32_t)last, **poff = SV_ROW_OFF(sv) + (int32_t)last,
            **pend = SV_ROW_END(sv) + (int32_t)last;
    for (int32_t i = 0; i < (int32_t)n;) {
        int16_t *a = *pon--, *b = *poff--, *end = *pend--;
        for (; (uintptr_t)a < (uintptr_t)end; a += SV_STRIDE(sv), b += SV_STRIDE(sv)) {
            if (*a != *b) continue;
            int64_t t = (int64_t)SV_YMAX(sv) - i;
            if (!fits32(t)) return overflow();
            int64_t y = (int64_t)(int32_t)t - 1;
            if (!fits32(y)) return overflow();
            int r = DoHorizDropout(sv, a, b, (int32_t)y, bitmap, flags);
            if (r) return r;
        }
        if (i == 0x7fffffff) return overflow();
        i++;
    }
    int64_t w = (int64_t)SV_XMAX(sv) - SV_XMIN(sv);
    if (!fits32(w)) return overflow();
    int16_t **con = SV_COL_ON(sv), **cend = SV_COL_END(sv), **coend = SV_COL_OEND(sv);
    for (int32_t j = 0; j < (int32_t)w;) {
        ptrdiff_t s = SV_STRIDE(sv);
        int16_t *a = *cend++ - s, *b = *coend++ - s, *begin = *con++;
        for (; (uintptr_t)a >= (uintptr_t)begin; a -= SV_STRIDE(sv), b -= SV_STRIDE(sv)) {
            if (*a != *b) continue;
            int64_t x = (int64_t)SV_XMIN(sv) + j;
            if (!fits32(x)) return overflow();
            int r = DoVertDropout(sv, a, b, (int32_t)x, bitmap, flags);
            if (r) return r;
        }
        if (j == 0x7fffffff) return overflow();
        j++;
    }
    return 0;
}

/* fsc_FillBitMap: clears the bitmap, fills each row between its ON/OFF crossing pairs
 * (fsc_BLTHoriz), then runs dropout control unless flags bit 2 turns it off. */
TT_EXPORT int tt_fsc_FillBitMap(uint8_t *sv, char *bitmap, int rowBytes, uint16_t flags) {
    int64_t n = (int64_t)SV_YMAX(sv) - SV_YMIN(sv);
    if (!fits32(n)) return overflow();
    SV_ROWBYTES(sv) = rowBytes;
    int64_t words = (int64_t)rowBytes >> 2; /* row length in 32-bit words */
    int64_t total = (int64_t)(int32_t)n * words;
    if (!fits32(total) || (int32_t)total < 0) return overflow();
    memset(bitmap, 0, (uint64_t)(uint32_t)total << 2);
    int32_t xmin = SV_XMIN(sv);
    if (!fits16(xmin)) return overflow();
    n = (int64_t)SV_YMAX(sv) - SV_YMIN(sv);
    if (!fits32(n)) return overflow();
    int64_t last = (int64_t)(int32_t)n - 1;
    if (!fits32(last)) return overflow();
    int16_t **pon = SV_ROW_ON(sv) + (int32_t)last, **poff = SV_ROW_OFF(sv) + (int32_t)last,
            **pend = SV_ROW_END(sv) + (int32_t)last;
    blt_fn blt = SV_LSBFIRST(sv) ? ORIG(blt_fn, RVA_BLT_HORIZ_REV) : ORIG(blt_fn, RVA_BLT_HORIZ);
    uint8_t *row = (uint8_t *)bitmap;
    int32_t xm = (int16_t)xmin;
    for (int32_t i = 0; i < (int32_t)n;) {
        int16_t *a = *pon--, *end = *pend--, *b = *poff--;
        while ((uintptr_t)a < (uintptr_t)end) {
            int32_t x1 = *a - xm;
            if (!fits16(x1)) return overflow();
            int32_t x2 = *b - xm;
            a += SV_STRIDE(sv);
            if (!fits16(x2)) return overflow();
            b += SV_STRIDE(sv);
            int16_t s1 = (int16_t)x1, s2 = (int16_t)x2;
            /* pixels [min, max - 1], in either order of the pair; nothing if equal */
            if (s1 < s2) blt(s1, s2 - 1, (uint32_t *)row);
            else if (s1 > s2) blt(s2, s1 - 1, (uint32_t *)row);
        }
        row = ptr_add(row, words * 4);
        if (i == 0x7fffffff) return overflow();
        i++;
    }
    if (flags & 2) return 0;
    return LookForDropouts(sv, bitmap, flags);
}
