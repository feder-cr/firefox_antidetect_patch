/*
 * TrueTypeRasterizer::Implementation (DWrite.dll 10.0.26100.9711) around the ported scaler:
 * the driver of the standalone texture path (reimpl/idwr/texture.c). Written from the
 * original's behaviour (notes/truetype.md, "Standalone scaler" and "Glyph run port: the
 * standalone alpha texture"); reimpl/oracle/fs_oracle.c models the same sequence for the
 * scaler checks, without bold, oblique, variations and the strike cases below.
 *
 *   Initialize 0x1800911bc       memory bases, the sfnt callbacks, the variation hooks
 *                                (ApplyOutlineVariation 0x8230, ApplyCVTVariation 0x76a0,
 *                                GetVariationAxisCoordinates 0xd6ab0), fs_NewSfnt
 *   NewTransform 0x1800091bc     strike lookup (fs_FindBlocForPpem), the oblique shear
 *                                (x += 0x5700 / 65536 * y, or of y for sideways runs), the
 *                                fs_GlyphInputType fields, the scan-control word
 *   RasterizeInternal 0x18006aa64 fs_NewGlyph, the bold ClearType compatibility word at
 *                                input + 0x8c, the GlyphRasterizationFlags, fs__Contour
 *   GetBitmap 0x18006a680        fs_FindBitMapSize, fs_ContourScan, the GlyphBitmap, and the
 *                                retry without strikes on a 0x18xx error
 *
 * Embedded bitmap strikes stay outside the scaler, as in fs_oracle.c: whether a strike is
 * active and whether it holds the glyph come from reimpl/idwr/sbit.c, and so does the bitmap,
 * then treated as sbit_GetBitmap does (strike_glyph, strike_bitmap below): searched only
 * under a multiple of 90 degrees, emboldened by the scaler's bold shifts (one pixel right,
 * one pixel wider), turned, cut to its ink box for image format 5; under the ClearType bold
 * word (input + 0x8c = 3) expanded to the overscale and emboldened by fsc_OverscaleToBold.
 *
 * Errors: what the original throws (FileFormatException for an fs error, SafeInt traps)
 * raises through native.h's catch frame, which the caller arms. Initialize, NewTransform,
 * GetMetrics and GetBitmap (like GetOutline, outline_driver.c) run under a __try/__except whose
 * filter, FilterSafeIntExceptions 0x1800ef6d4, takes the SafeInt traps (STATUS_INTEGER_OVERFLOW
 * from SafeIntOnOverflow 0x1801090b0, STATUS_INTEGER_DIVIDE_BY_ZERO from 0x18014cd24); the handler,
 * TranslateRasterizerException 0x18014ccb4, rethrows them as FileFormatException
 * (DWRITE_E_FILEFORMAT). RASTERIZER_TRY below does the same with IDWR_ERR_SAFEINT_*: a glyph
 * program that traps gives an empty glyph (RasterizeGlyph's catch), not an unhinted one.
 */
#ifdef IDWR_STANDALONE
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "idwr_internal.h"
#include "ttraster.h"
#include "../truetype/orig.h"
#include "../truetype/native.h"

#define RVA_fs_NewSfnt            0x9168c
#define RVA_fs__NewTransformation 0x09804
#define RVA_fs_NewGlyph           0x6c388
#define RVA_fs__Contour           0x0c8f0
#define RVA_fs_FindBitMapSize     0x6ad78
#define RVA_fs_ContourScan        0x6dd58
#define RVA_FixMul                0x67b70

#define HR_FILEFORMAT ((int)0x88985000)   /* DWRITE_E_FILEFORMAT */

typedef int (*fs2_fn)(void *input, void *info);
typedef int (*fs3_fn)(void *input, void *info, int flag);

#define INPUT_SIZE 0xe0
#define INFO_SIZE  0x100
#define BASE(in, i)  (*(void **)((uint8_t *)(in) + 8 + 8 * (i)))
#define SIZE(inf, i) (*(int32_t *)((uint8_t *)(inf) + 4 * (i)))
#define FIELD(p, o, T) (*(T *)((uint8_t *)(p) + (o)))

struct idwr_tt {
    const idwr_font *font;
    const uint8_t *data;
    size_t len;
    uint32_t face_offset;         /* this + 0x248 */
    uint8_t *input, *info;        /* this + 0x1e0, + 0x1d8 */
    const float *coords;          /* this + 0x2a0: normalized coordinates */
    uint16_t axes;
    uint16_t *f2dot14;            /* this + 0x2b8: the same as Fixed2_14 */
    int32_t matrix[4];            /* this + 0x1e8: the transform as given */
    uint32_t flags;               /* this + 0x214 */
    uint16_t overscale_x;         /* this + 0x20e */
    uint16_t overscale_y;         /* this + 0x210 */
    uint16_t scan_control;        /* this + 0x20c */
    uint8_t skip;                 /* this + 0x240 */
    uint8_t ppem;                 /* this + 0x20a */
    uint16_t glyph;               /* this + 0x220 */
    uint16_t strike_rot;          /* the strike glyph's rotation code and ppem */
    uint8_t strike_ppem;
    uint32_t *strike_bits;        /* the last strike glyph's bitmap */
    size_t strike_cap;
    /* the GDI-compatible metrics (GetMetrics) */
    float inv[4];                 /* this + 0x1f8: TransformF, the inverse of the 2x2 matrix */
    uint16_t upem;                /* this + 0x208 */
    uint8_t full_width;           /* this + 0x218: NewGlyph's compatible full-width flag */
    uint32_t avg_width;           /* this + 0x21c: the average char width, whole pixels, 16.16 */
    const uint8_t *hdmx_rec;      /* this + 0x250: the hdmx device record at the ppem */
    uint32_t hdmx_len;
    const uint8_t *ltsh;          /* this + 0x278 */
    uint32_t ltsh_len;
    uint8_t from_strike;          /* the last get_bitmap drew the glyph from the strike (sbit.c) */
};

/* GetSfntFragment 0x180058130: offset 0 is the face's table directory */
static const void *get_fragment(void *client, int offset, int length, void **ctx) {
    idwr_tt *t = (idwr_tt *)client;
    size_t off = offset == 0 ? t->face_offset : (size_t)(uint32_t)offset;
    *ctx = NULL;
    if (off > t->len || (size_t)(uint32_t)length > t->len - off) return NULL;
    return t->data + off;
}
static void release_fragment(void *ctx) { (void)ctx; }

/* ApplyOutlineVariation 0x180008230: gvar deltas when the instance has coordinates */
static void apply_outline_variation(void *client, uint16_t glyph, int16_t *ends, uint16_t contours, int32_t *x,
                                    int32_t *y, uint32_t points) {
    idwr_tt *t = (idwr_tt *)client;
    if (t == NULL || t->axes == 0) return;
    idwr_apply_gvar(t->font, t->coords, t->axes, glyph, ends, contours, x, y, points);
}
/* ApplyCVTVariation 0x1800076a0 */
static void apply_cvt_variation(void *client, int32_t *cvt, uint32_t count) {
    idwr_tt *t = (idwr_tt *)client;
    if (t == NULL || t->axes == 0) return;
    idwr_apply_cvar(t->font, t->coords, t->axes, cvt, count);
}
/* GetVariationAxisCoordinates 0x1800d6ab0: the Fixed2_14 coordinates, zeros on a count mismatch */
static void get_axis_coordinates(void *client, uint16_t *out, uint16_t n) {
    idwr_tt *t = (idwr_tt *)client;
    if (t == NULL) return;
    for (uint16_t i = 0; i < n; i++) out[i] = n == t->axes ? t->f2dot14[i] : 0;
}

static void fs_check(int err) {
    if (err) idwr_raise(HR_FILEFORMAT);
}

/* the __try/__except of Implementation's entry points (scopes 0x1800911eb, 0x1800092e1,
 * 0x18006a3ce, 0x18006a6a9): SafeInt traps become DWRITE_E_FILEFORMAT, other codes pass */
static int translate_rasterizer_exception(int code) {
    return code == IDWR_ERR_SAFEINT_OVERFLOW || code == IDWR_ERR_SAFEINT_DIVZERO ? HR_FILEFORMAT : code;
}
#ifdef IDWR_NEGATIVE_CONTROL_SAFEINT
#define RASTERIZER_TRY(c) idwr_catch c; idwr_catch_push(&c); if (IDWR_SETJMP(c.buf)) { idwr_catch_pop(&c); idwr_raise(c.code); }
#else
#define RASTERIZER_TRY(c)                                                  idwr_catch c;                                                         idwr_catch_push(&c);                                                  if (IDWR_SETJMP(c.buf)) {                                                 idwr_catch_pop(&c);                                                   idwr_raise(translate_rasterizer_exception(c.code));               }
#endif
#define RASTERIZER_END(c) idwr_catch_pop(&c)

idwr_tt *idwr_tt_open(const idwr_font *font, const float *coords, uint16_t axes) {
    idwr_tt *t = calloc(1, sizeof *t);
    if (!t) return NULL;
    t->font = font;
    t->data = font->data;
    t->len = font->size;
    t->face_offset = (uint32_t)(font->dir - 12 - font->data);
    t->coords = coords;
    t->axes = axes;
    t->upem = font->head && font->head_len >= 20 ? idwr_u16(font->head + 18) : 0;
    t->f2dot14 = calloc(axes ? axes : 1, sizeof *t->f2dot14);
    t->input = calloc(1, INPUT_SIZE);
    t->info = calloc(1, INFO_SIZE);
    if (!t->f2dot14 || !t->input || !t->info) {
        idwr_tt_close(t);
        return NULL;
    }
    /* ComputeNormalizedAxisCoordinates: Fixed2_14 = (Fixed16_16 + 2) >> 2 */
    for (uint16_t i = 0; i < axes; i++) {
        int32_t fx = idwr_cvttss2si(coords[i] * 65536.0f);
        t->f2dot14[i] = (uint16_t)(int16_t)((fx + 2) >> 2);
    }
    uint8_t *in = t->input, *info = t->info;
    SIZE(info, 0) = 0x4ac;
    for (int i = 0; i < 3; i++) BASE(in, i) = SIZE(info, i) ? calloc(1, (size_t)SIZE(info, i)) : NULL;
    uint8_t *b0 = BASE(in, 0);
    FIELD(b0, SIZE(info, 0) - 4, uint32_t) = 0x73666e74;
    memset(b0, 0, 0x4a8);
    FIELD(b0, 0x178, uint64_t) = 0x10000;
    FIELD(b0, 0x188, uint64_t) = 0x10000;
    FIELD(b0, 0x198, uint32_t) = 0x10000;
    FIELD(b0, 0x420, uint64_t) = 0x10000;
    FIELD(b0, 0x430, uint64_t) = 0x10000;
    FIELD(b0, 0x440, uint32_t) = 0x10000;
    FIELD(b0, 0x118, void *) = in + 8;
    FIELD(b0, 0x120, void *) = info;
    FIELD(in, 0x50, void *) = (void *)get_fragment;
    FIELD(in, 0x58, void *) = (void *)release_fragment;
    FIELD(in, 0x80, void *) = NULL;
    FIELD(in, 0x78, void *) = t;
    in[0x8a] = 1;
    in[0x89] = 1;
    FIELD(in, 0x90, uint16_t) = 0xffff;
    FIELD(in, 0x92, uint16_t) = 0xffff;
    FIELD(in, 0x94, uint16_t) = axes;
    FIELD(in, 0x60, void *) = (void *)apply_outline_variation;
    FIELD(in, 0x68, void *) = (void *)apply_cvt_variation;
    FIELD(in, 0x70, void *) = (void *)get_axis_coordinates;
    int err;
    {
        idwr_catch c;
        idwr_catch_push(&c);
        if (IDWR_SETJMP(c.buf)) {
            idwr_catch_pop(&c);
            idwr_tt_close(t);
            int code = translate_rasterizer_exception(c.code);
            if (code == HR_FILEFORMAT) return NULL;   /* as an fs error: the caller's FileFormatException */
            idwr_raise(code);
        }
        err = ORIG(fs2_fn, RVA_fs_NewSfnt)(in, info);
        idwr_catch_pop(&c);
    }
    if (err) {
        idwr_tt_close(t);
        return NULL;
    }
    for (int i = 3; i <= 4; i++) BASE(in, i) = SIZE(info, i) ? calloc(1, (size_t)SIZE(info, i)) : NULL;
    return t;
}

void idwr_tt_close(idwr_tt *t) {
    if (!t) return;
    if (t->input)
        for (int i = 0; i < 9; i++) free(BASE(t->input, i));
    free(t->input);
    free(t->info);
    free(t->f2dot14);
    free(t->strike_bits);
    free(t);
}

static int32_t fixmul(int32_t a, int32_t b) { return ORIG(int32_t (*)(int32_t, int32_t), RVA_FixMul)(a, b); }
#define RVA_FixDiv 0x69800
static int32_t fixdiv(int32_t a, int32_t b) { return ORIG(int32_t (*)(int32_t, int32_t), RVA_FixDiv)(a, b); }

/* Fixed16_16::FixedInt32ToFloat 0x180009788 */
static float fixed_float(int32_t v) {
    if (v == INT32_MAX) return INFINITY;
    if (v == INT32_MIN) return -INFINITY;
    return (float)v * 1.5258789e-05f;
}

/* Fixed16_16::FloatToFixedInt32 0x18004c304 */
static int32_t float_fixed(float v) {
    v = v * 65536.0f;
    v = v * 65536.0f;
    int64_t x = (v >= -9223372036854775808.0f && v < 9223372036854775808.0f) ? (int64_t)v : INT64_MIN;
    x = (x + 0x8000) >> 16;
    return x < INT32_MIN ? INT32_MIN : x > INT32_MAX ? INT32_MAX : (int32_t)x;
}

static int32_t iabs(int32_t v) { return v < 0 ? -v : v; }

/* TrueTypeRasterizer::Implementation::ShouldCheckHdmx 0x1800097c4: not a variable font, a strike
 * ppem byte, GDI measuring (not GDI natural without strikes), no rotation or skew */
static int should_check_hdmx(const idwr_tt *t) {
    uint32_t fl;
    const uint8_t *fvar = idwr_font_table(t->font, IDWR_TAG('f', 'v', 'a', 'r'), &fl);
    int variable = fvar && fl >= 10 && idwr_u16(fvar + 8) != 0;
    return !variable && t->ppem && (t->flags & 0x30) && (t->flags & 0x60) != 0x20 && t->matrix[2] == 0 &&
           t->matrix[1] == 0;
}

/* GetCompatibleWidthInfo 0x180008ea8: the hdmx record for the ppem (records sorted by size),
 * LTSH, and the average char width (OS/2 xAvgCharWidth; without OS/2 two thirds of the head
 * box width, with a zero xAvgCharWidth half of it) in whole pixels */
static void compatible_width_info(idwr_tt *t) {
    uint32_t len;
    const uint8_t *hdmx = idwr_font_table(t->font, IDWR_TAG('h', 'd', 'm', 'x'), &len);
    if (hdmx && len) {
        if (len >= 8 && idwr_u16(hdmx) == 0) {
            uint32_t size = (uint32_t)hdmx[4] << 24 | (uint32_t)hdmx[5] << 16 | (uint32_t)hdmx[6] << 8 | hdmx[7];
            uint32_t at = 8;
            for (uint16_t i = 0; i != idwr_u16(hdmx + 2); i++) {
                if (len < at || len - at < 2) break;   /* the original's catch swallows it */
                uint8_t px = hdmx[at];
                if (px == t->ppem) {
                    t->hdmx_rec = hdmx + at;
                    t->hdmx_len = len - at < size ? len - at : size;
                }
                if (t->ppem <= px) break;
                at += size;
            }
        }
    }
    const uint8_t *ltsh = idwr_font_table(t->font, IDWR_TAG('L', 'T', 'S', 'H'), &len);
    if (ltsh && len) t->ltsh = ltsh, t->ltsh_len = len;
    const uint8_t *h = t->font->head;
    uint32_t ol;
    const uint8_t *os2 = idwr_font_table(t->font, IDWR_TAG('O', 'S', '/', '2'), &ol);
    int32_t box = h && t->font->head_len >= 44 ? (int32_t)idwr_i16(h + 40) - idwr_i16(h + 36) : 0;
    uint32_t avg;
    if (!os2) avg = (uint32_t)(box * 2 / 3);
    else {
        avg = ol >= 4 ? idwr_u16(os2 + 2) : 0;
        if (avg == 0) avg = (uint32_t)(box / 2);
    }
    t->avg_width = ((uint32_t)(iabs(t->matrix[0]) / (int32_t)t->upem) * (avg & 0xffff) + 0x8000) & 0xffff0000u;
}

/* a strike at this ppem: fs_FindBlocForPpem's EBLC BitmapSize with ppemX = ppemY = ppem and
 * bitDepth 1, which is sbit.c's strike in aliased mode */
static int strike_at(const idwr_tt *t, uint8_t ppem) {
    return idwr_strike_state(t->font, IDWR_MODE_ALIASED, (float)ppem, 0) != IDWR_STRIKE_NONE;
}

static void tt_transform(idwr_tt *t, uint32_t point_size, const int32_t matrix[4], uint16_t ox, uint16_t oy,
                         uint32_t flags, uint8_t ppem);
void idwr_tt_transform(idwr_tt *t, uint32_t point_size, const int32_t matrix[4], uint16_t ox, uint16_t oy,
                      uint32_t flags, uint8_t ppem) {
    RASTERIZER_TRY(c)
    tt_transform(t, point_size, matrix, ox, oy, flags, ppem);
    RASTERIZER_END(c);
}
static void tt_transform(idwr_tt *t, uint32_t point_size, const int32_t matrix[4], uint16_t ox, uint16_t oy,
                         uint32_t flags, uint8_t ppem) {
    uint8_t *in = t->input;
    t->ppem = ppem;
    t->skip = 0;
    memcpy(t->matrix, matrix, sizeof t->matrix);
    t->flags = flags;
    t->overscale_x = ox;
    t->overscale_y = oy;
    if (flags & 0x40) {
        if (!(ppem && strike_at(t, ppem))) t->flags = flags &= ~0x40u;
    }
    /* TransformF: the inverse of the 2x2 matrix, for DisplayToEmX/Y and the design bounds */
    {
        float xx = fixed_float(matrix[0]), yx = fixed_float(matrix[2]), xy = fixed_float(matrix[1]),
              yy = fixed_float(matrix[3]);
        float det = yy * xx - xy * yx;
        t->inv[0] = t->inv[1] = t->inv[2] = t->inv[3] = 0.0f;
        if (det != 0.0f) {
            float r = 1.0f / det;
            t->inv[0] = r * yy;
            t->inv[2] = -yx * r;
            t->inv[1] = -xy * r;
            t->inv[3] = r * xx;
        }
    }
    t->hdmx_rec = t->ltsh = NULL;
    t->hdmx_len = t->ltsh_len = 0;
    if (should_check_hdmx(t)) compatible_width_info(t);
    int32_t a = matrix[0], b = matrix[1], c = matrix[2], d = matrix[3];
#ifdef IDWR_NEGATIVE_CONTROL_OBLIQUE
    if (0) {           /* texture_check.py --control oblique: no shear */
#else
    if (flags & 4) {   /* oblique simulation: shear by 0x5700 / 65536 */
#endif
        if ((flags & 8) == 0) {
            b += fixmul(a, 0x5700);
            d += fixmul(c, 0x5700);
        } else {
            a -= fixmul(b, 0x5700);
            c -= fixmul(d, 0x5700);
        }
    }
#ifdef IDWR_NEGATIVE_CONTROL_SKEWSWAP
    { int32_t s = b; b = c; c = s; }   /* texture_check.py --control skewswap: off-diagonal terms swapped */
#endif
    FIELD(in, 0x9c, int32_t) = a;
    FIELD(in, 0xa0, int32_t) = c;
    FIELD(in, 0xa4, int32_t) = 0;
    FIELD(in, 0xa8, int32_t) = b;
    FIELD(in, 0xac, int32_t) = d;
    FIELD(in, 0xb0, int32_t) = 0;
    FIELD(in, 0xb4, int32_t) = 0;
    FIELD(in, 0xb8, int32_t) = 0;
    FIELD(in, 0xbc, int32_t) = 0x40000000;
    FIELD(in, 0x94, uint16_t) = 0x60;
    FIELD(in, 0x96, uint16_t) = 0x60;
    FIELD(in, 0x90, uint32_t) = point_size << 16;
    FIELD(in, 0x98, uint32_t) = 0x16a0a;
    FIELD(in, 0xc0, uint16_t) = 0;
    in[0xcc] = 0;
    FIELD(in, 0xc8, uint32_t) = 0;
    in[0xcd] = 0;
    in[0xce] = 1;
    in[0xcf] = 1;
    FIELD(in, 0xd2, uint16_t) = ox;
    FIELD(in, 0xd4, uint16_t) = oy;
    uint16_t sc = 0;
    if (ox == 1) {
        t->scan_control = 0;
    } else if ((flags & 0x30) == 0) {
        sc = (uint16_t)(t->scan_control | (oy > 1 ? 0x71 : 0x51));
        t->scan_control = sc;
    } else if ((flags & 0x40) == 0) {
        sc = (uint16_t)(((flags >> 3) & 2) | 1);
        t->scan_control = sc;
    } else {
        t->scan_control = 0;
    }
    if ((sc & 1) && (flags & 0x80)) {
        sc |= 0x80;
        t->scan_control = sc;
    }
    FIELD(in, 0xd0, uint16_t) = sc;
    FIELD(in, 0xc2, uint16_t) = (flags & 2) ? 0x14 : 0;
    FIELD(in, 0xc4, uint16_t) = (flags & 2) ? 0x14 : 0;
    in[0xd6] = (uint8_t)(~flags & 1);
    int e = ORIG(fs3_fn, RVA_fs__NewTransformation)(in, t->info, 1);
    if (e == 0x1901) t->skip = 1;   /* and the original still throws */
    else if (e) e = ORIG(fs3_fn, RVA_fs__NewTransformation)(in, t->info, 0);
    fs_check(e);
}

static void reset_memory_bases(idwr_tt *t) {
    for (int i = 5; i <= 8; i++) {
        SIZE(t->info, i) = 0;
        free(BASE(t->input, i));
        BASE(t->input, i) = NULL;
    }
}

enum { RI_EMPTY = 0, RI_OUTLINE = 1, RI_STRIKE = 2 };

static int strike_glyph(idwr_tt *t, uint32_t flags);

/* RasterizeInternal: RI_OUTLINE when fs__Contour ran, RI_EMPTY for an empty glyph (or after
 * the 0x1901 transform), RI_STRIKE for a glyph the active strike holds and sbit.c draws.
 * `scaler_sbit` (the GDI-compatible metrics): the scaler searches the strike itself, as in
 * DWrite (input + 0x95 = !(flags & 0x40): fs_NewGlyph's sbit_SearchForBitmap, then fs__Contour's
 * and fs_FindBitMapSize's strike branches, reimpl/truetype/scaler_sbit.c), and a strike glyph
 * is RI_OUTLINE with info + 0xa8 set */
static int rasterize_internal(idwr_tt *t, uint32_t flags, uint32_t *rf, int scaler_sbit) {
    uint8_t *in = t->input;
    if (t->skip) {
        *rf = (t->scan_control & 1) ? 0 : 3;
        return RI_EMPTY;
    }
    reset_memory_bases(t);
    FIELD(in, 0x90, uint16_t) = 0xffff;
    FIELD(in, 0x92, uint16_t) = t->glyph;
    in[0x94] = 0;
    in[0x95] = scaler_sbit ? !(flags & 0x40) : 1;   /* else sbit.c stands for the scaler's search */
    fs_check(ORIG(fs2_fn, RVA_fs_NewGlyph)(in, t->info));
    int strike = scaler_sbit ? FIELD(t->info, 0xa8, int16_t) != 0 : strike_glyph(t, flags);
    /* bold: the ClearType compatibility word, for axis-aligned transforms below 51 ppem */
    int32_t compat = 0;
    const int32_t *m = t->matrix;
    if ((flags & 2) && t->overscale_y == 1 && ((m[2] == 0 && m[1] == 0) || (m[0] == 0 && m[3] == 0))) {
        if (t->overscale_x == 1) {
            if ((uint8_t)(t->ppem - 1) < 0x32) compat = 1;
        } else if (((t->overscale_x - 6) & 0xfffd) == 0 && (uint8_t)(t->ppem - 1) < 0x32 && m[2] == 0 && m[1] == 0 &&
                   m[0] > 0 && m[3] > 0) {
            compat = 3;
        }
    }
#ifdef IDWR_NEGATIVE_CONTROL_BOLDWORD
    compat = 0;   /* texture_check.py --control boldword */
#endif
    FIELD(in, 0x8c, int32_t) = compat;
    uint32_t f = 3;
    if (t->overscale_x != 1) {
        if ((t->scan_control & 0x10) == 0) {
#ifdef IDWR_NEGATIVE_CONTROL_RASTERFLAGS
            if ((flags & 0x40) == 0) f = 0;   /* texture_check.py --control rasterflags */
#else
            if ((flags & 0x40) == 0) f = 2;
#endif
#ifdef IDWR_NEGATIVE_CONTROL_OVERLAY
            else f = compat == 3 ? 6 : 2;   /* texture_check.py --control overlay */
#else
            else f = compat == 3 ? 6 : 3;
#endif
        } else if ((flags & 0x40) && compat == 3) {
            f = strike ? 6 : 4;
        } else if (!((flags & 0x40) && strike)) {
            f = 0;
        }
    }
    *rf = f;
    if (strike && !scaler_sbit) return RI_STRIKE;
    in[0x90] = 1;
    int e = ORIG(fs3_fn, RVA_fs__Contour)(in, t->info, 1);
    if (e == 0x1409) return RI_EMPTY;
    if (e) fs_check(ORIG(fs3_fn, RVA_fs__Contour)(in, t->info, 0));
    return RI_OUTLINE;
}

/* sbit_SearchForBitmap's answer: the scaler searches the strike at its own ppem (client +
 * 0x3da, + 0x3dc) when the transformation is a multiple of 90 degrees (rotation code client +
 * 0x3e2 < 4: no flip, no skew, no oblique shear) */
static int strike_glyph(idwr_tt *t, uint32_t flags) {
    if (!(flags & 0x40) || t->overscale_y != 1) return 0;
    const uint8_t *cl = BASE(t->input, 0);
    uint16_t rot = FIELD(cl, 0x3e2, uint16_t), px = FIELD(cl, 0x3da, uint16_t), py = FIELD(cl, 0x3dc, uint16_t);
#ifdef IDWR_NEGATIVE_CONTROL_STRIKEROT
    rot = 0;   /* texture_check.py --control strikerot: strikes under any transform */
#endif
    if (rot >= 4 || px != py || px == 0 || px > 255) return 0;
    t->strike_rot = rot;
    t->strike_ppem = (uint8_t)px;
    return idwr_strike_state(t->font, IDWR_MODE_ALIASED, (float)px, t->glyph) == IDWR_STRIKE_GLYPH;
}

/* the strike glyph from sbit.c as a 1-bit GlyphBitmap (sbit_GetBitmap): emboldened in the
 * strike's own space under bold simulation (OR-ed with itself shifted by the scaler's bold
 * shifts, one pixel right at strike sizes), turned by the transformation's multiple of 90
 * degrees, then, for image format 5, cut to its ink box (a blank bitmap keeps its cell) */
static void strike_bitmap_subpixel(idwr_tt *t, idwr_tt_bitmap *out);

/* the scaler cuts a strike bitmap to its ink box only for image format 5, whose metrics are
 * the index subtable's (one box for every glyph); other formats keep the glyph's own box,
 * blank columns or a negative bearing included */
static int strike_trims(const idwr_tt *t) {
    int format = 0, index_metrics = 0;
    if (!idwr_strike_entry_kind(t->font, IDWR_MODE_ALIASED, (float)t->strike_ppem, t->glyph, &format, &index_metrics))
        return 0;
#ifdef IDWR_NEGATIVE_CONTROL_STRIKEBOX
    return 1;   /* texture_check.py --control strikebox: every strike bitmap cut to its ink */
#endif
    return index_metrics;
}

static void strike_bitmap(idwr_tt *t, idwr_tt_bitmap *out) {
    if (FIELD(t->input, 0x8c, int32_t) == 3) {
        strike_bitmap_subpixel(t, out);
        return;
    }
    /* sbit_GetBitmap's bold shifts (client + 0x410 x, + 0x412 y: the scaler's bold shifts,
     * swapped under a 90-degree rotation; 0 without bold simulation) */
    const uint8_t *cl = BASE(t->input, 0);
    int32_t sx = FIELD(cl, 0x410, int16_t), sy = FIELD(cl, 0x412, int16_t);
#ifdef IDWR_NEGATIVE_CONTROL_STRIKEBOLD
    sx = sy = 0;   /* texture_check.py --control strikebold */
#endif
    idwr_strike_glyph sg;
    /* a composite is assembled on its grown canvas (idwr_strike_composite): the bits its components
     * put past the glyph's width stay, and the bold smear stays inside the canvas */
    uint32_t gx = sx > 0 ? (uint32_t)sx : 0, gy = sy > 0 ? (uint32_t)sy : 0;
    int comp = idwr_strike_composite(t->font, IDWR_MODE_ALIASED, (float)t->strike_ppem, t->glyph, gx, gy, &sg, NULL, 0);
    if (comp == -2 ? idwr_strike_bitmap(t->font, IDWR_MODE_ALIASED, (float)t->strike_ppem, t->glyph, &sg, NULL, 0) != 1
                   : comp != 1)
        idwr_raise(HR_FILEFORMAT);
    size_t n = (size_t)sg.width * sg.height;
    uint8_t *src = malloc(n ? n : 1);
    if (!src) idwr_raise(IDWR_HR_E_OUTOFMEMORY);
    if ((comp == -2 ? idwr_strike_bitmap(t->font, IDWR_MODE_ALIASED, (float)t->strike_ppem, t->glyph, &sg, src, n)
                    : idwr_strike_composite(t->font, IDWR_MODE_ALIASED, (float)t->strike_ppem, t->glyph, gx, gy, &sg, src,
                                            n)) != 1) {
        free(src);
        idwr_raise(HR_FILEFORMAT);
    }
    /* bold in the strike's own space: smeared sx pixels right and sy rows down. sbit_Embolden 0x18014ef90
     * reads only the glyph's own box (the first width - dx pixels of a row, the rows above the last dy), so
     * a composite's bits past it (on the grown canvas) are dropped here */
    int32_t L = sg.bearing_x, T = sg.bearing_y, w0 = (int32_t)sg.width, h0 = (int32_t)sg.height, cw = w0;
    int32_t w = w0 + (w0 ? sx : 0), h = h0 + (h0 ? sy : 0);
    if (comp == 1) {   /* the canvas is already grown */
        w = w0, h = h0;
        if (w0) w0 -= (int32_t)gx;
        if (h0) h0 -= (int32_t)gy;
    }
    uint8_t *bsrc = calloc((size_t)(w ? w : 1) * (size_t)(h ? h : 1), 1);
    if (!bsrc) {
        free(src);
        idwr_raise(IDWR_HR_E_OUTOFMEMORY);
    }
    for (int32_t r = 0; r < h0; r++)
        for (int32_t k = 0; k < w0; k++)
            if (src[(size_t)r * cw + k])
                for (int32_t j = 0; j <= sy && r + j < h; j++)
                    for (int32_t i = 0; i <= sx && k + i < w; i++) bsrc[(size_t)(r + j) * w + k + i] = 1;
    free(src);
    /* rotation (y up: code 1 is (x, y) -> (-y, x), 2 is (-x, -y), 3 is (y, -x)) of the cell
     * [L, L + w) x [T - h, T), pixels by their centres */
    uint16_t rot = t->strike_rot;
    int32_t BW = (rot & 1) ? h : w, BH = (rot & 1) ? w : h, L2, T2;
    if (rot == 0) L2 = L, T2 = T;
    else if (rot == 1) L2 = -T, T2 = L + w;
    else if (rot == 2) L2 = -(L + w), T2 = -(T - h);
    else L2 = T - h, T2 = -L;
    uint8_t *px = calloc((size_t)(BW ? BW : 1) * (size_t)(BH ? BH : 1), 1);
    if (!px) {
        free(bsrc);
        idwr_raise(IDWR_HR_E_OUTOFMEMORY);
    }
    for (int32_t r = 0; r < h; r++)
        for (int32_t k = 0; k < w; k++) {
            if (!bsrc[(size_t)r * w + k]) continue;
            /* doubled centre coordinates, y up */
            int32_t cx = 2 * (L + k) + 1, cy = 2 * (T - r) - 1, nx, ny;
            if (rot == 0) nx = cx, ny = cy;
            else if (rot == 1) nx = -cy, ny = cx;
            else if (rot == 2) nx = -cx, ny = -cy;
            else nx = cy, ny = -cx;
            int32_t col = (nx - 2 * L2 - 1) / 2, row = (2 * T2 - ny - 1) / 2;
            px[(size_t)row * BW + col] = 1;
        }
    free(bsrc);
    uint32_t sw = (uint32_t)BW, sh = (uint32_t)BH;
    uint32_t x0 = sw, x1 = 0, y0 = sh, y1 = 0;
    for (uint32_t y = 0; y < sh; y++)
        for (uint32_t x = 0; x < sw; x++)
            if (px[(size_t)y * sw + x]) {
                if (x < x0) x0 = x;
                if (x + 1 > x1) x1 = x + 1;
                if (y < y0) y0 = y;
                if (y + 1 > y1) y1 = y + 1;
            }
    if (x0 >= x1 || !strike_trims(t)) x0 = 0, x1 = sw, y0 = 0, y1 = sh;
    uint32_t ow = x1 - x0, oh = y1 - y0;
    uint32_t stride = ((ow + 31) >> 3) & ~3u;
    size_t words = (size_t)stride / 4 * oh;
    if (words > t->strike_cap) {
        free(t->strike_bits);
        t->strike_bits = malloc(words * 4);
        t->strike_cap = t->strike_bits ? words : 0;
        if (!t->strike_bits) {
            free(px);
            idwr_raise(IDWR_HR_E_OUTOFMEMORY);
        }
    }
    if (words) memset(t->strike_bits, 0, words * 4);
    for (uint32_t y = 0; y < oh; y++)
        for (uint32_t x = 0; x < ow; x++)
            if (px[(size_t)(y + y0) * sw + x + x0]) t->strike_bits[(size_t)y * (stride / 4) + (x >> 5)] |= 1u << (x & 31);
    free(px);
    out->left = L2 + (int32_t)x0;
    out->top = T2 - (int32_t)y0;
    out->width = ow;
    out->height = oh;
    out->stride = (int32_t)stride;
    out->bits = t->strike_bits;
}

/* ClearType bold of a strike glyph (compatibility word 3, fs_ContourScan's strike branch):
 * the strike bitmap without sbit's own bold, each pixel repeated over the horizontal
 * overscale (sbit_ExpandSubpixelFromMono), then fsc_OverscaleToBold with the scaler's
 * vertical ppem as strength; the overscaled bitmap is the result (rows of
 * ROWBYTES(width * overscale), the rect's x scaled) */
#define RVA_sbit_ExpandSubpixelFromMono 0xdad2c
#define RVA_fsc_OverscaleToBold 0x26e0
static void strike_bitmap_subpixel(idwr_tt *t, idwr_tt_bitmap *out) {
    idwr_strike_glyph sg;
    if (t->strike_rot != 0) idwr_raise(IDWR_ERR_UNMODELLED);
    /* a composite on its canvas grown by the bold width: the bits past the glyph's width show */
    int32_t bw = FIELD(BASE(t->input, 0), 0x410, int16_t);
    uint32_t gx = bw > 0 ? (uint32_t)bw : 0;
    int comp = idwr_strike_composite(t->font, IDWR_MODE_ALIASED, (float)t->strike_ppem, t->glyph, gx, 0, &sg, NULL, 0);
    if (comp == -2 ? idwr_strike_bitmap(t->font, IDWR_MODE_ALIASED, (float)t->strike_ppem, t->glyph, &sg, NULL, 0) != 1
                   : comp != 1)
        idwr_raise(HR_FILEFORMAT);
    size_t n = (size_t)sg.width * sg.height;
    uint8_t *px = malloc(n ? n : 1);
    if (!px) idwr_raise(IDWR_HR_E_OUTOFMEMORY);
    if ((comp == -2 ? idwr_strike_bitmap(t->font, IDWR_MODE_ALIASED, (float)t->strike_ppem, t->glyph, &sg, px, n)
                    : idwr_strike_composite(t->font, IDWR_MODE_ALIASED, (float)t->strike_ppem, t->glyph, gx, 0, &sg, px,
                                            n)) != 1) {
        free(px);
        idwr_raise(HR_FILEFORMAT);
    }
    uint32_t x0 = sg.width, x1 = 0, y0 = sg.height, y1 = 0;
    for (uint32_t y = 0; y < sg.height; y++)
        for (uint32_t x = 0; x < sg.width; x++)
            if (px[(size_t)y * sg.width + x]) {
                if (x < x0) x0 = x;
                if (x + 1 > x1) x1 = x + 1;
                if (y < y0) y0 = y;
                if (y + 1 > y1) y1 = y + 1;
            }
    if (x0 >= x1 || !strike_trims(t)) x0 = 0, x1 = sg.width, y0 = 0, y1 = sg.height;
    uint32_t w = x1 - x0, h = y1 - y0, ov = t->overscale_x;
    /* the rect keeps the bold width sbit_GetMetrics gives (client + 0x410), the bits do not */
    uint32_t wr = w + (comp == 1 && w ? 0 : (uint32_t)FIELD(BASE(t->input, 0), 0x410, int16_t));
    uint32_t mrb = ((wr + 31) >> 3) & ~3u, rb = ((wr * ov + 31) >> 3) & ~3u;
    uint8_t *mono = calloc((size_t)mrb * h + 1, 1), *buf = calloc((size_t)rb * h + 4, 1), *tmp = calloc((size_t)rb * h + 4, 1);
    if (!mono || !buf || !tmp) {
        free(px), free(mono), free(buf), free(tmp);
        idwr_raise(IDWR_HR_E_OUTOFMEMORY);
    }
    for (uint32_t y = 0; y < h; y++)   /* most significant bit first, as the sbit code works */
        for (uint32_t x = 0; x < w; x++)
            if (px[(size_t)(y + y0) * sg.width + x + x0]) mono[(size_t)y * mrb + (x >> 3)] |= (uint8_t)(0x80 >> (x & 7));
    free(px);
    ORIG(void (*)(const uint8_t *, uint8_t *, uint16_t, uint16_t, uint16_t, uint16_t, uint16_t),
         RVA_sbit_ExpandSubpixelFromMono)(mono, buf, (uint16_t)wr, (uint16_t)h, (uint16_t)mrb, (uint16_t)rb, (uint16_t)ov);
    free(mono);
    int32_t L = (sg.bearing_x + (int32_t)x0) * (int32_t)ov, T = sg.bearing_y - (int32_t)y0;
    _Alignas(8) uint8_t src[0x28], dst[0x28];
    memset(src, 0, sizeof src);
    FIELD(src, 0, int16_t) = (int16_t)rb;
    FIELD(src, 2, int16_t) = (int16_t)T;
    FIELD(src, 4, int16_t) = (int16_t)L;
    FIELD(src, 6, int16_t) = (int16_t)(T - (int32_t)h);
    FIELD(src, 8, int16_t) = (int16_t)(L + (int32_t)(wr * ov));
    FIELD(src, 0x1c, int32_t) = (int32_t)(rb * h);
    memcpy(dst, src, sizeof dst);
    FIELD(src, 0x20, uint8_t *) = buf;
    FIELD(dst, 0x20, uint8_t *) = tmp;
    const uint8_t *cl = BASE(t->input, 0);
    ORIG(void (*)(uint16_t, uint16_t, uint8_t *, uint8_t *), RVA_fsc_OverscaleToBold)(
        FIELD(cl, 0x3dc, uint16_t), (uint16_t)ov, src, dst);
    free(tmp);
    size_t words = (size_t)rb / 4 * h;
    if (words > t->strike_cap) {
        free(t->strike_bits);
        t->strike_bits = malloc(words * 4);
        t->strike_cap = t->strike_bits ? words : 0;
        if (!t->strike_bits) {
            free(buf);
            idwr_raise(IDWR_HR_E_OUTOFMEMORY);
        }
    }
    if (words) memset(t->strike_bits, 0, words * 4);
    for (size_t i = 0; i < (size_t)rb * h; i++)   /* back to DWrite's bit order */
        for (int b = 0; b < 8; b++)
            if (buf[i] & (0x80 >> b)) ((uint8_t *)t->strike_bits)[i] |= (uint8_t)(1u << b);
    free(buf);
    out->left = L;
    out->top = T;
    out->width = wr * ov;
    out->height = h;
    out->stride = (int32_t)rb;
    out->bits = t->strike_bits;
}

/* GetBitmap / GetBitmapInternal: 0, or the fs error. `bounds`: the rect only (no scan) */
static int get_bitmap(idwr_tt *t, uint32_t flags, idwr_tt_bitmap *out, int bounds, int scaler_sbit) {
    uint32_t rf = 0;
    memset(out, 0, sizeof *out);
    int r = rasterize_internal(t, flags, &rf, scaler_sbit);
    t->from_strike = r == RI_STRIKE;
    out->flags = rf;
    if (r == RI_EMPTY) return 0;
    if (r == RI_STRIKE) {
        strike_bitmap(t, out);
        if (bounds) out->stride = 0, out->bits = NULL;
        return 0;
    }
    int e = ORIG(fs2_fn, RVA_fs_FindBitMapSize)(t->input, t->info);
    if (e) return e;
    uint8_t *rec = t->info + ((rf & 1) ? 0x58 : 0xe8);
    out->height = (uint32_t)((int32_t)FIELD(rec, 0xe, int16_t) - FIELD(rec, 0xa, int16_t));
    out->width = (uint32_t)((int32_t)FIELD(rec, 0x10, int16_t) - FIELD(rec, 0xc, int16_t));
    out->left = FIELD(rec, 0xc, int16_t);
    out->top = FIELD(rec, 0xe, int16_t);
    if (bounds) return 0;
    for (int i = 5; i <= 8; i++) {
        int32_t n = SIZE(t->info, i);
        BASE(t->input, i) = n ? calloc(1, (size_t)n) : NULL;
        if (n && !BASE(t->input, i)) idwr_raise(IDWR_HR_E_OUTOFMEMORY);
    }
    e = ORIG(fs2_fn, RVA_fs_ContourScan)(t->input, t->info);
    if (e) return e;
    out->stride = FIELD(rec, 8, int16_t);
    out->bits = FIELD(rec, 0, const uint32_t *);
    return 0;
}

void idwr_tt_bitmap_get(idwr_tt *t, uint16_t glyph, idwr_tt_bitmap *out) {
    t->glyph = glyph;
    RASTERIZER_TRY(c)
    int e = get_bitmap(t, t->flags, out, 0, 0);
    if ((e & 0xff00) == 0x1800 && (t->flags & 0x40)) e = get_bitmap(t, t->flags & ~0x40u, out, 0, 0);
    RASTERIZER_END(c);
    fs_check(e);
}

/* ---- GetMetrics 0x18006a3b0: the GDI-compatible glyph metrics ---- */

/* FontRasterizer::DisplayToEmX / DisplayToEmY 0x180069ef0 / 0x180069e28: a device vector (16.16)
 * to em units (16.16) through TransformF; without rotation or skew the device value is first
 * rounded to whole pixels */
static float fixed_float_rounded(int32_t v) {
    uint32_t r = ((uint32_t)v + 0x8000u) & 0xffff0000u;
    return r == 0x80000000u ? -INFINITY : (float)(int32_t)r * 1.5258789e-05f;
}
static int32_t display_to_em_x(const float *m, int32_t x, int32_t y) {
    if (m[2] == 0.0f && m[1] == 0.0f) return float_fixed(fixed_float_rounded(x) * m[0]);
    return float_fixed(fixed_float(x) * m[0] + fixed_float(y) * m[1]);
}
static int32_t display_to_em_y(const float *m, int32_t x, int32_t y) {
    if (m[2] == 0.0f && m[1] == 0.0f) return float_fixed(fixed_float_rounded(y) * m[3]);
    return float_fixed(fixed_float(y) * m[3] + m[2] * fixed_float(x));
}

/* FontRasterizer::GlyphBoundingBox::AddPoint 0x18006a2e8 */
typedef struct {
    float m[4];
    int set;
    float x0, x1, y0, y1;
} bbox;
static void bbox_add(bbox *b, float x, float y) {
    int diag = b->m[2] == 0.0f && b->m[1] == 0.0f;
    float tx = diag ? x * b->m[0] : b->m[1] * y + x * b->m[0];
    float ty = diag ? y * b->m[3] : y * b->m[3] + b->m[2] * x;
    if (!b->set) {
        b->x0 = b->x1 = tx, b->y0 = b->y1 = ty, b->set = 1;
        return;
    }
    if (tx < b->x0) b->x0 = tx;
    if (b->x1 <= tx && tx != b->x1) b->x1 = tx;
    if (ty < b->y0) b->y0 = ty;
    if (!(ty < b->y1 || ty == b->y1)) b->y1 = ty;
}

/* GetDesignBounds 0x180069fbc: the bitmap's box, or with bits each row's first and last ink
 * pixels (their left edges), through TransformF, in em units 16.16: {min x, min y, max y, max x} */
static void design_bounds(const idwr_tt *t, const idwr_tt_bitmap *b, int32_t out[4]) {
    float sx = 1.0f, sy = -1.0f;
    if ((b->flags & 1) == 0) sx = 1.0f / (float)t->overscale_x, sy = -1.0f / (float)t->overscale_y;
    bbox box;
    memset(&box, 0, sizeof box);
    memcpy(box.m, t->inv, sizeof box.m);
    int32_t row = -b->top, left = b->left;
    if (!b->bits) {
        if (b->width && b->height) {
            float yb = (float)((int32_t)b->height + row) * sy, xr = (float)((int32_t)b->width + left) * sx;
            bbox_add(&box, (float)left * sx, (float)row * sy);
            bbox_add(&box, xr, (float)row * sy);
            bbox_add(&box, (float)left * sx, yb);
            bbox_add(&box, xr, yb);
        }
    } else {
        int32_t words = b->stride / 4;
        const uint32_t *line = b->bits;
        if (words > 0 && (int32_t)b->height > 0)
            for (uint32_t r = 0; r < b->height; r++, row++, line += words) {
                const uint32_t *p = line, *q = line + words - 1;
                if (p < q) {
                    while (p < q && *p == 0) p++;
                    while (p < q && *q == 0) q--;
                }
                if (*p == 0) continue;
                int lo = 0, hi = 31;
                while (!((*p >> lo) & 1)) lo++;
                while (!((*q >> hi) & 1)) hi--;
                float x0 = (float)(lo + left + (int32_t)(p - line) * 32) * sx;
                float x1 = (float)(hi + left + (int32_t)(q - line) * 32) * sx;
                float ya = (float)row * sy, yb = (float)(row + 1) * sy;
                bbox_add(&box, x0, ya);
                bbox_add(&box, x0, yb);
                bbox_add(&box, x1, ya);
                bbox_add(&box, x1, yb);
            }
    }
    out[0] = float_fixed(box.x0);
    out[1] = float_fixed(box.y0);
    out[2] = float_fixed(box.y1);
    out[3] = float_fixed(box.x1);
}

/* TryGetCompatibleDisplayWidth 0x180069b54: the hdmx width at the ppem, or the linearly scaled
 * advance where LTSH (or head flags without bits 2 and 4) says the glyph scales linearly; the
 * full-width rule; one more pixel under bold. In em units 16.16; 0 when not applicable. */
static int compatible_width(const idwr_tt *t, int32_t *out) {
#ifdef IDWR_NEGATIVE_CONTROL_HDMX
    return 0;   /* no hdmx/LTSH widths: the hinted advances everywhere */
#endif
    if (!should_check_hdmx(t)) return 0;
    uint16_t g = t->glyph;
    int32_t w = 0;
    if (t->hdmx_rec == NULL) {
        const uint8_t *h = t->font->head;
        if (h && t->font->head_len > 0x11 && (h[0x11] & 0x14) && !t->full_width) {
            if (!t->ltsh) return 0;
            if ((uint32_t)g + 4 >= t->ltsh_len) idwr_raise(HR_FILEFORMAT);
            if (t->ppem < t->ltsh[g + 4]) return 0;
        }
        uint32_t hl, ml;
        const uint8_t *hhea = idwr_font_table(t->font, IDWR_TAG('h', 'h', 'e', 'a'), &hl);
        const uint8_t *hmtx = idwr_font_table(t->font, IDWR_TAG('h', 'm', 't', 'x'), &ml);
        uint16_t n = hhea && hl >= 36 ? idwr_u16(hhea + 34) : 0;
        if (n == 0) return 0;
        if (n <= g) g = (uint16_t)(n - 1);
        if (!hmtx || 4u * g + 2 > ml) idwr_raise(HR_FILEFORMAT);
        uint16_t adv = idwr_u16(hmtx + 4 * g);
        if (adv) w = (iabs(t->matrix[0]) / (int32_t)t->upem) * (int32_t)adv;
    } else {
        if ((uint32_t)g + 2 >= t->hdmx_len) idwr_raise(HR_FILEFORMAT);
        uint8_t px = t->hdmx_rec[g + 2];
        if (px) w = (int32_t)px << 16;
    }
#ifndef IDWR_NEGATIVE_CONTROL_FULLWIDTH
    if (t->full_width && (((uint32_t)w + 0x8000u) & 0xffff0000u) != t->avg_width) w = (int32_t)(t->avg_width * 2);
#endif
    if (w) w += (int32_t)((t->flags & 2) * 0x8000);
    *out = fixdiv((int32_t)(((uint32_t)w + 0x8000u) & 0xffff0000u), iabs(t->matrix[0]));
    return 1;
}

static int32_t to_units(uint16_t upem, int32_t v) { return (int32_t)(((int64_t)upem * v + 0x8000) >> 16); }

static void tt_glyph_metrics(idwr_tt *t, uint16_t glyph, int full_width, int32_t out[8]);
void idwr_tt_glyph_metrics(idwr_tt *t, uint16_t glyph, int full_width, int32_t out[8]) {
    RASTERIZER_TRY(c)
    tt_glyph_metrics(t, glyph, full_width, out);
    RASTERIZER_END(c);
}
static void tt_glyph_metrics(idwr_tt *t, uint16_t glyph, int full_width, int32_t out[8]) {
    t->glyph = glyph;
    t->full_width = (uint8_t)(full_width != 0);
    const int32_t *m = t->matrix;
    /* GetBitmap(true): the rect only, for a transform without rotation or skew (or a quarter turn) */
    int axis = m[2] == 0 ? (m[1] == 0 && m[0] != 0 && m[3] != 0) : (m[0] == 0 && m[3] == 0 && m[1] != 0);
    idwr_tt_bitmap b;
    /* the scaler searches the strike, as in DWrite: a strike glyph's metrics, vertical ones
     * included, are sbit_GetMetrics' (fs_FindBitMapSize's strike branch). A strike holds glyphs
     * only under a multiple of 90 degrees, where `axis` asks for the rect alone (no scan). */
    int sbit = 1;
#ifdef IDWR_NEGATIVE_CONTROL_SBITMETRICS
    sbit = 0;   /* sbit_metrics_check.py / linux_check.py: sbit.c's strike, the bitmap's corner */
#endif
    if (!axis) sbit = 0;
    int e = get_bitmap(t, t->flags, &b, axis, sbit);
    if ((e & 0xff00) == 0x1800 && (t->flags & 0x40)) e = get_bitmap(t, t->flags & ~0x40u, &b, axis, sbit);
    fs_check(e);
    memset(out, 0, 8 * sizeof *out);
    out[7] = (int32_t)b.flags;
    if (t->skip) return;
    int32_t box[4], adv;
    design_bounds(t, &b, box);
    const uint8_t *info = t->info;
    if (!compatible_width(t, &adv)) adv = display_to_em_x(t->inv, FIELD(info, 0x48, int32_t), FIELD(info, 0x4c, int32_t));
    int over = (b.flags & 1) == 0;
    int32_t left = (int32_t)FIELD(info, over ? 0xf4 : 0x64, int16_t) * 65536;
    int32_t top = (int32_t)FIELD(info, over ? 0xf6 : 0x66, int16_t) * 65536;
    if (over) left /= (int32_t)FIELD(t->input, 0xd2, uint16_t), top /= (int32_t)FIELD(t->input, 0xd4, uint16_t);
    int32_t vx = FIELD(info, 0xd4, int32_t), vy = FIELD(info, 0xd8, int32_t);
    int32_t ax = FIELD(info, 0xcc, int32_t), ay = FIELD(info, 0xd0, int32_t);
    if (t->from_strike) {
        /* only without the scaler's search (IDWR_NEGATIVE_CONTROL_SBITMETRICS, or a transform
         * that is not a multiple of 90 degrees, where no strike glyph is drawn): sbit.c's bitmap,
         * its corner and no vertical vectors */
        left = b.left * 65536, top = b.top * 65536;
        vx = vy = ax = ay = 0;
    }
    int32_t vorg = display_to_em_y(t->inv, left - vx, top - vy);
    int32_t advh = display_to_em_y(t->inv, ax, ay);
    uint16_t u = t->upem;
    out[0] = to_units(u, adv);
    out[1] = to_units(u, advh);
    out[2] = to_units(u, vorg);
    out[3] = to_units(u, box[0]);
    out[4] = to_units(u, adv - box[3]);
    out[5] = to_units(u, vorg - box[2]);
    out[6] = to_units(u, box[1] - vorg + advh);
}

#endif /* IDWR_STANDALONE */
