/*
 * GDI-compatible glyph metrics of a idwr_face, as DWrite.dll 10.0.26100.9711 computes them:
 *   DWriteFontFace::GetGdiCompatibleGlyphMetrics 0x18004a160 / GetGdiCompatibleGlyphAdvances
 *     0x180049cb0                       argument checks
 *   FontFace::GetGlyphMetrics 0x18004a3d4 / GetGlyphAdvances 0x18004a8d0
 *                                       the rasterizer transform and RenderingFlags; the design
 *                                       metrics (with the simulations) unless a matrix entry is 0
 *                                       (grid fit), and unless the font matrix is unitsPerEm
 *                                       without simulations; advances at em = unitsPerEm, no bold,
 *                                       a translation and 1 pixel per DIP are design advances
 *   MakeRasterizerTransform 0x18004bec0 em * pixels per DIP (rounded: GDI-compatible) through the
 *                                       transform, a uniform scale snapped to whole pixels, the
 *                                       strike ppem byte
 *   ::GetGlyphMetrics 0x18004c394       GlyphMetricsRasterizationParameters: point size round(em *
 *                                       0.75), overscale 6x1; one rasterizer, NewTransform, then
 *                                       per glyph NewGlyph(glyph, IsCompatibleFullWidthGlyph) and
 *                                       GetMetrics (reimpl/idwr/ttraster.c idwr_tt_glyph_metrics)
 *   OpenTypeFontFaceBuilder::WriteCompatibleFullWidthGlyphBits 0x18009ab50 and
 *   ReadCompatibleFullWidthGlyphBits 0x18009ace8
 *                                       the full-width glyphs of a monospaced face whose OS/2 code
 *                                       pages give 932, 936, 949 or 950 (GetCompatibleFontCodePage
 *                                       0x18009ac90, IsMonospacedFont 0x18005def8): every glyph a BMP
 *                                       code point in DWrite's always-full-width ranges, or turned
 *                                       into two bytes by the code page, maps to
 *                                       (reimpl/idwr/fullwidth_tables.h, reimpl/gen_fullwidth.py)
 * The hinted metrics need the scaler: -DIDWR_STANDALONE builds only. A CFF face's come from
 * CffRasterizer (idwr/cffraster.c, builds with -DIDWR_CFF): the same NewTransform, NewGlyph and
 * GetMetrics calls through its vtable, on a rasterizer created for the call as ::GetGlyphMetrics
 * does (CffRasterizer::Implementation::GetMetrics works at the design matrix with the rendering
 * flags).
 */
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "idwr.h"
#include "face.h"

#ifndef IDWR_STANDALONE
void idwr_face_gdi_release(idwr_face *f) { (void)f; }

int32_t idwr_face_get_gdi_compatible_glyph_metrics(idwr_face *face, float em_size, float pixels_per_dip,
                                                  const idwr_matrix *transform, int use_gdi_natural,
                                                  const uint16_t *glyphs, uint32_t count, idwr_glyph_metrics *out,
                                                  int sideways) {
    (void)face, (void)em_size, (void)pixels_per_dip, (void)transform, (void)use_gdi_natural, (void)glyphs, (void)count,
        (void)out, (void)sideways;
    return IDWR_HR_E_NOTIMPL;
}

int32_t idwr_face_get_gdi_compatible_glyph_advances(idwr_face *face, float em_size, float pixels_per_dip,
                                                   const idwr_matrix *transform, int use_gdi_natural, int sideways,
                                                   uint32_t count, const uint16_t *glyphs, int32_t *advances) {
    (void)face, (void)em_size, (void)pixels_per_dip, (void)transform, (void)use_gdi_natural, (void)sideways,
        (void)count, (void)glyphs, (void)advances;
    return IDWR_HR_E_NOTIMPL;
}
#else
#include "ttraster.h"
#include "fullwidth_tables.h"
#include "../truetype/native.h"
#ifdef IDWR_CFF
#include "cffraster.h"
#endif

#pragma STDC FP_CONTRACT OFF
#ifdef __clang__
#pragma clang fp contract(off)
#endif

typedef struct {
    int full_ready;
    uint32_t *full_bits;     /* FontFace + 0x150, NULL when the face has none */
    idwr_tt *tt;              /* the rasterizer, kept between calls (fs_NewSfnt runs once) */
} gdi_state;

void idwr_face_gdi_release(idwr_face *f) {
    gdi_state *g = f->gdi;
    if (!g) return;
    idwr_tt_close(g->tt);
    free(g->full_bits);
    free(g);
    f->gdi = NULL;
}

/* ---- the compatible full-width glyphs ---- */

/* GetCompatibleFontCodePage: OS/2 ulCodePageRange1 bits 17 (932), 20 (950), 18 (936), 19 (949) */
static uint32_t compatible_code_page(const idwr_font *font) {
    uint32_t len;
    const uint8_t *os2 = idwr_font_table(font, IDWR_TAG('O', 'S', '/', '2'), &len);
    if (!os2 || len < 80) return 0;
    uint8_t b = os2[0x4f] & 0x1e;
    if (!b) return 0;
    if (b & 2) return 932;
    if (b >> 4) return 950;
    if (b & 4) return 936;
    return b & 8 ? 949 : 0;
}


static int in_ranges16(const uint16_t (*r)[2], size_t n, uint32_t c) {
    size_t lo = 0, hi = n;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (c < r[mid][0]) hi = mid;
        else if (c > r[mid][1]) lo = mid + 1;
        else return 1;
    }
    return 0;
}

static int full_width_code_point(uint32_t cp, uint32_t c) {
    for (size_t k = 0; k < sizeof FULLWIDTH_ALWAYS / sizeof *FULLWIDTH_ALWAYS; k++) {
        if (FULLWIDTH_ALWAYS[k][1] < c) continue;
        if (c >= FULLWIDTH_ALWAYS[k][0]) return 1;
        break;
    }
#define R(t) in_ranges16(t, sizeof t / sizeof *t, c)
    switch (cp) {
    case 932: return R(FULLWIDTH_DBCS_932);
    case 936: return R(FULLWIDTH_DBCS_936);
    case 949: return R(FULLWIDTH_DBCS_949);
    case 950: return R(FULLWIDTH_DBCS_950);
    }
#undef R
    return 0;
}

static gdi_state *state(idwr_face *f) {
    if (!f->gdi) f->gdi = calloc(1, sizeof(gdi_state));
    if (!f->gdi) idwr_raise(IDWR_HR_E_OUTOFMEMORY);
    return f->gdi;
}

/* FontFace::IsCompatibleFullWidthGlyph */
static int full_width_glyph(idwr_face *f, uint16_t glyph) {
    gdi_state *g = state(f);
    if (!g->full_ready) {
        g->full_ready = 1;
        uint32_t cp = compatible_code_page(f->font);
        size_t words = ((size_t)f->glyph_count + 31) >> 5;
#ifndef IDWR_NEGATIVE_CONTROL_FULLWIDTH
        if (cp && words && idwr_font_is_monospaced(f->font)) {
#else
        if (0) {
#endif
            g->full_bits = calloc(words, sizeof *g->full_bits);
            if (!g->full_bits) idwr_raise(IDWR_HR_E_OUTOFMEMORY);
            for (uint32_t c = 1; c < 0x10000; c++) {
                uint16_t gid = idwr_font_glyph_index(f->font, c);
                if (gid && (gid >> 5) < words && full_width_code_point(cp, c)) g->full_bits[gid >> 5] |= 1u << (gid & 31);
            }
        }
    }
    return g->full_bits && glyph < f->glyph_count && (g->full_bits[glyph >> 5] >> (glyph & 31) & 1);
}

/* ---- the rasterizer transform ---- */

static int zero_tol(float v) { return v < 1.5258789e-05f && v > -1.5258789e-05f; }

static int32_t rhu(float v) {
    int32_t i = (v >= -2147483648.0f && v < 2147483648.0f) ? (int32_t)nearbyintf(v) : INT32_MIN;
    return (float)i - v == -0.5f ? i + 1 : i;
}

static int32_t float_fixed(float v) {
    v = v * 65536.0f;
    v = v * 65536.0f;
    int64_t x = (v >= -9223372036854775808.0f && v < 9223372036854775808.0f) ? (int64_t)v : INT64_MIN;
    x = (x + 0x8000) >> 16;
    return x < INT32_MIN ? INT32_MIN : x > INT32_MAX ? INT32_MAX : (int32_t)x;
}

static float fabs_bits(float v) {
    union { float f; uint32_t u; } x;
    x.f = v;
    x.u &= 0x7fffffffu;
    return x.f;
}

/* MakeRasterizerTransform(em, sx, sy, transform, sideways, round = 1, &ppem): the 16.16 font
 * matrix {xx, xy, yx, yy} (y down). Returns IDWR_HR_E_INVALIDARG for a scale out of [0, 32768). */
static int32_t rasterizer_transform(float em, float sx, float sy, const idwr_matrix *t, int sideways, int32_t m[4],
                                    uint8_t *ppem) {
    float ey = sy * em;
    *ppem = 0;
    memset(m, 0, 4 * sizeof *m);
    if (!t) {
        float ex = sx * em;
        int32_t ix = rhu(ex) << 16, iy = ix;
        if (ey != ex) iy = rhu(ey) << 16;
        int32_t y = iy;
        if (sideways) {
            m[2] = iy, m[1] = -ix, m[0] = 0;
        } else {
            m[3] = iy, m[0] = ix;
        }
        int32_t a = y < 0 ? -y : y;
        if (a <= 0xffffff) *ppem = (uint8_t)(a >> 16);
        return 0;
    }
    float a, b, c, d;
    if (sideways) a = 0, b = ey, c = -em * sx, d = 0;
    else a = sx * em, b = 0, c = 0, d = ey;
    float x5 = t->m11 * a + t->m21 * c, x4 = t->m12 * a + t->m22 * c;
    float x3 = t->m11 * b + t->m21 * d, x6 = t->m12 * b + t->m22 * d;
    int uniform = 0;
    float u = 0, v = 0;
    if (!zero_tol(x4)) {
        if (zero_tol(x5) && zero_tol(x6)) u = x4, v = x3, uniform = 1;
    } else if (zero_tol(x3)) {
        u = x5, v = x6, uniform = 1;
    }
    if (uniform) {
        float au = fabs_bits(u), av = fabs_bits(v);
        if (zero_tol(au - av)) {
            if (0.0f > au || au >= 32768.0f) return IDWR_HR_E_INVALIDARG;
            int32_t p = rhu(au);
            if ((uint32_t)p <= 0xff) *ppem = (uint8_t)p;
            if (!zero_tol(au)) {
                float s = (float)(uint32_t)p / au;
                x5 = x5 * s, x4 = x4 * s, x3 = x3 * s, x6 = x6 * s;
            }
        }
    }
    m[0] = float_fixed(x5);
    m[2] = float_fixed(-x4);
    m[1] = float_fixed(-x3);
    m[3] = float_fixed(x6);
    return 0;
}

/* ---- FontFace::GetGlyphMetrics ---- */

static int is_translation(const idwr_matrix *t) {
    if (!t) return 1;
    uint32_t b[4];
    memcpy(b, t, sizeof b);
    return b[0] == 0x3f800000u && b[1] == 0 && b[2] == 0 && b[3] == 0x3f800000u;
}

/* RenderingFlags for the measuring mode (1 GDI classic, 2 GDI natural); 0 in *hinted when DWrite
 * takes the design metrics */
static uint32_t gdi_flags(const idwr_face *f, const int32_t m[4], int measuring, int sideways, int *hinted) {
    uint32_t flags = (f->simulations & 1) * 2;
    if (f->simulations & 2) flags |= 4 + (sideways ? 8 : 0);
    *hinted = 0;
    if (m[0] == 0 || m[2] == 0 || m[1] == 0 || m[3] == 0) flags |= 1;
    if (!(flags & 1)) return flags;
    flags |= measuring == 1 ? 0x10 : measuring == 2 ? 0x20 : 0;
    if (idwr_is_legacy_east_asian(f->font)) flags |= 0x40;
    int32_t up = (int32_t)f->metrics.designUnitsPerEm << 16;
    *hinted = !(m[0] == up && m[1] == 0 && m[2] == 0 && m[3] == up && f->simulations == 0);
    return flags;
}

/* A glyph whose rasterization throws FileFormatException (an fs error, a SafeInt trap the
 * rasterizer translates): GlyphMetricsRasterizationState's per-glyph step 0x18008e304 runs NewGlyph
 * and GetMetrics under catch (FileFormatException &) (funclet 0x18018d0b7), which takes the
 * face's design metrics of the glyph instead (its metrics source, vt[1](glyph, 0x1ff)): those of
 * the instance, without the simulations' adjustments. The run goes on with the next glyph, on
 * the same rasterizer. Any other exception leaves the call. */
static void fileformat_fallback(idwr_face *f, uint16_t g, int code, idwr_glyph_metrics *out) {
#ifdef IDWR_NEGATIVE_CONTROL_GDIFALLBACK
    idwr_raise(code);   /* linux_check.py --extra-dir: the whole call fails, as before */
#endif
    if (code != IDWR_HR_E_FILEFORMAT) idwr_raise(code);
    memset(out, 0, sizeof *out);
    idwr_get_design_glyph_metrics_instance(f->font, f->coords, f->axes, g, out);
}

/* A glyph id met earlier in the call (ConstrainGlyphIndex 0x1800a6c44 applied): the loop of
 * 0x18008df14 rasterizes each glyph once and points every later entry of the same glyph at its
 * metrics. The index of the first one, or -1. */
static int64_t earlier(idwr_face *f, const uint16_t *glyphs, uint32_t i) {
#ifdef IDWR_NEGATIVE_CONTROL_GDIRASTERIZER
    return -1;
#endif
    uint16_t g = glyphs[i] < f->glyph_count ? glyphs[i] : 0;
    for (uint32_t j = 0; j < i; j++)
        if ((glyphs[j] < f->glyph_count ? glyphs[j] : 0) == g) return j;
    return -1;
}

/* the hinted metrics of `count` glyphs (::GetGlyphMetrics 0x18004c394). Each call makes its own
 * rasterizer: 0x18008f014 calls CreateRasterizer 0x1800910f0, which allocates a new
 * Implementation (0x3d8 bytes, constructor 0x180092ae0: fs_NewSfnt, the font program), then
 * NewTransform. So nothing a glyph program leaves in the rasterizer (twilight points, storage, the
 * CVT, a font program that failed) reaches the next call. */
#ifdef IDWR_CFF
/* ::GetGlyphMetrics on a CFF face: CreateRasterizer (CffRasterizer), NewTransform(point size, m,
 * 6, 1, flags, ppem), then NewGlyph(glyph, full width) and GetMetrics, whose GlyphMetrics are
 * {advanceWidth, advanceHeight, verticalOriginY, lsb, rsb, tsb, bsb} as idwr_tt_glyph_metrics' */
static void cff_hinted_metrics(idwr_face *f, float em, const int32_t m[4], uint8_t ppem, uint32_t flags,
                               const uint16_t *glyphs, uint32_t count, idwr_glyph_metrics *out) {
    uint8_t *r = idwr_cff_rasterizer_create(f->font, f->axes ? f->coords : NULL, f->axes);
    idwr_catch c;
    idwr_catch_push(&c);
    if (IDWR_SETJMP(c.buf)) {
        idwr_catch_pop(&c);
        idwr_cff_rasterizer_destroy(r);
        idwr_raise(c.code);
    }
    void **vt = *(void ***)r;
    ((void (*)(uint8_t *, uint32_t, const int32_t *, uint32_t, uint32_t, uint32_t, uint32_t))vt[1])(
        r, (uint32_t)rhu(em * 0.75f), m, 6, 1, flags, ppem);
    for (uint32_t i = 0; i < count; i++) {
        uint16_t g = glyphs[i] < f->glyph_count ? glyphs[i] : 0;
        int64_t e = earlier(f, glyphs, i);
        if (e >= 0) {
            out[i] = out[e];
            continue;
        }
        int32_t gm[8] = {0};
        idwr_catch gc;
        idwr_catch_push(&gc);
        if (IDWR_SETJMP(gc.buf)) {
            idwr_catch_pop(&gc);
            fileformat_fallback(f, g, gc.code, &out[i]);
            continue;
        }
        ((void (*)(uint8_t *, uint32_t, uint32_t))vt[2])(r, g, (uint32_t)full_width_glyph(f, g));
        ((void (*)(uint8_t *, int32_t *))vt[4])(r, gm);
        idwr_catch_pop(&gc);
        out[i].leftSideBearing = gm[3];
        out[i].advanceWidth = (uint32_t)gm[0];
        out[i].rightSideBearing = gm[4];
        out[i].topSideBearing = gm[5];
        out[i].advanceHeight = (uint32_t)gm[1];
        out[i].bottomSideBearing = gm[6];
        out[i].verticalOriginY = gm[2];
    }
    idwr_catch_pop(&c);
    idwr_cff_rasterizer_destroy(r);
}
#endif

static void hinted_metrics(idwr_face *f, float em, const int32_t m[4], uint8_t ppem, uint32_t flags,
                           const uint16_t *glyphs, uint32_t count, idwr_glyph_metrics *out) {
#ifdef IDWR_CFF
    if (idwr_font_is_cff(f->font)) {
        cff_hinted_metrics(f, em, m, ppem, flags, glyphs, count, out);
        return;
    }
#endif
    gdi_state *gs = state(f);
#ifndef IDWR_NEGATIVE_CONTROL_GDIRASTERIZER
    idwr_tt_close(gs->tt), gs->tt = NULL;   /* linux_check.py --texture-control gdirasterizer: kept */
#endif
    if (!gs->tt) gs->tt = idwr_tt_open(f->font, f->axes ? f->coords : NULL, f->axes);
    idwr_tt *t = gs->tt;
    if (!t) idwr_raise(IDWR_HR_E_FILEFORMAT);
    idwr_catch c;
    idwr_catch_push(&c);
    if (IDWR_SETJMP(c.buf)) {
        idwr_catch_pop(&c);
        idwr_tt_close(gs->tt);
        gs->tt = NULL;
        idwr_raise(c.code);
    }
    idwr_tt_transform(t, (uint32_t)rhu(em * 0.75f), m, 6, 1, flags, ppem);
    for (uint32_t i = 0; i < count; i++) {
        uint16_t g = glyphs[i] < f->glyph_count ? glyphs[i] : 0;
        int64_t e = earlier(f, glyphs, i);
        if (e >= 0) {
            out[i] = out[e];
            continue;
        }
        int32_t r[8];
        idwr_catch gc;
        idwr_catch_push(&gc);
        if (IDWR_SETJMP(gc.buf)) {
            idwr_catch_pop(&gc);
            fileformat_fallback(f, g, gc.code, &out[i]);
            continue;
        }
        idwr_tt_glyph_metrics(t, g, full_width_glyph(f, g), r);
        idwr_catch_pop(&gc);
        out[i].leftSideBearing = r[3];
        out[i].advanceWidth = (uint32_t)r[0];
        out[i].rightSideBearing = r[4];
        out[i].topSideBearing = r[5];
        out[i].advanceHeight = (uint32_t)r[1];
        out[i].bottomSideBearing = r[6];
        out[i].verticalOriginY = r[2];
    }
    idwr_catch_pop(&c);
#ifndef IDWR_NEGATIVE_CONTROL_GDIRASTERIZER
    idwr_tt_close(gs->tt), gs->tt = NULL;
#endif
}

static int32_t glyph_metrics(idwr_face *f, float em, float ppd, const idwr_matrix *t, int measuring, int sideways,
                             const uint16_t *glyphs, uint32_t count, idwr_glyph_metrics *out) {
    int32_t m[4];
    uint8_t ppem;
    int32_t hr = rasterizer_transform(em, ppd, ppd, t, sideways, m, &ppem);
    if (hr) return hr;
    int hinted;
    uint32_t flags = gdi_flags(f, m, measuring, sideways, &hinted);
    if (!hinted) return idwr_face_get_design_glyph_metrics(f, glyphs, count, out, sideways);
    idwr_catch c;
    idwr_catch_push(&c);
    if (IDWR_SETJMP(c.buf)) {
        idwr_catch_pop(&c);
        return c.code;
    }
    hinted_metrics(f, em, m, ppem, flags, glyphs, count, out);
    idwr_catch_pop(&c);
    return 0;
}

static int bad_matrix(const idwr_matrix *t) {
    return t && (t->m11 != t->m11 || t->m12 != t->m12 || t->m21 != t->m21 || t->m22 != t->m22 || t->dx != t->dx ||
                 t->dy != t->dy);
}

int32_t idwr_face_get_gdi_compatible_glyph_metrics(idwr_face *f, float em, float ppd, const idwr_matrix *t,
                                                  int use_gdi_natural, const uint16_t *glyphs, uint32_t count,
                                                  idwr_glyph_metrics *out, int sideways) {
    if (out) memset(out, 0, (size_t)count * sizeof *out);
    if (!f || !glyphs || !(em > 0.0f) || !(ppd > 0.0f) || bad_matrix(t)) return IDWR_HR_E_INVALIDARG;
    return glyph_metrics(f, em, ppd, t, use_gdi_natural ? 2 : 1, sideways, glyphs, count, out);
}

int32_t idwr_face_get_gdi_compatible_glyph_advances(idwr_face *f, float em, float ppd, const idwr_matrix *t,
                                                   int use_gdi_natural, int sideways, uint32_t count,
                                                   const uint16_t *glyphs, int32_t *advances) {
    if (advances) memset(advances, 0, (size_t)count * sizeof *advances);
    if (!f || (count && !glyphs) || em < 0.0f || !(ppd > 0.0f) || bad_matrix(t)) return IDWR_HR_E_INVALIDARG;
    if (em == 0.0f || count == 0) return 0;
    if (em == (float)f->metrics.designUnitsPerEm && !(f->simulations & 1) && is_translation(t) && ppd == 1.0f)
        return idwr_face_get_design_glyph_advances(f, count, glyphs, advances, sideways);
    idwr_glyph_metrics *m = malloc((size_t)count * sizeof *m);
    if (!m) return IDWR_HR_E_OUTOFMEMORY;
    int32_t hr = glyph_metrics(f, em, ppd, t, use_gdi_natural ? 2 : 1, sideways, glyphs, count, m);
    if (hr == 0)
        for (uint32_t i = 0; i < count; i++) advances[i] = (int32_t)(sideways ? m[i].advanceHeight : m[i].advanceWidth);
    free(m);
    return hr;
}
#endif
