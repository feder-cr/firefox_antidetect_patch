/*
 * A font face as IDWriteFontResource::CreateFontFace(simulations, axis values) makes it, and
 * the metrics calls the engine makes on it (notes/metrics.md, "Faces with simulations and
 * variable instances"), as DWrite.dll 10.0.26100.9711 computes them:
 *   DWriteFontFace::GetMetrics 0x1800cdb40 / 0x1800d3cf0   the face's metrics (InitializeFontMetrics
 *                                        0x18005f2d8: MVAR deltas), oblique: glyph box and
 *                                        sub/superscript x shifted by 0x5700 / 65536 of their y
 *   FontFace::GetDesignGlyphMetrics 0x1800525a4             AdjustGlyphMetricsForBoldSimulation
 *                                        0x18009cad0, AdjustGlyphMetricsForObliqueSimulation 0x18009c9f4
 *   FontFace::GetDesignGlyphAdvances 0x180048e7c            the advance array, or the design
 *                                        metrics plus the bold widening
 *   FontFace::GetGdiCompatibleMetrics 0x1800a4808            every field rounded to whole pixels
 *                                        and back; ascent and descent from VDMX
 *                                        (OpenTypeFontFaceBuilder::ReadVerticalDeviceMetrics 0x1800764b0)
 *   DWriteFontFace::GetRecommendedRenderingMode 0x1800735b0   gasp lists with the MVAR gsp deltas
 * The GDI-compatible glyph metrics, which need the TrueType scaler, are in gdimetrics.c.
 */
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "idwr.h"
#include "idwr_internal.h"
#include "face.h"

#define T(a, b, c, d) IDWR_TAG(a, b, c, d)
#define OBLIQUE 0x5700 /* tan of the oblique simulation's slant, 16.16 */

idwr_face *idwr_face_create(const idwr_font *font, uint32_t simulations, const idwr_axis_value *values, uint32_t count) {
    if (!font || (simulations & ~3u)) return NULL;
    idwr_face *f = calloc(1, sizeof *f);
    if (!f) return NULL;
    f->font = font;
    f->simulations = simulations;
    f->glyph_count = idwr_font_glyph_count(font);
    int n = idwr_normalize_axes(font, values, count, f->coords, IDWR_FACE_MAX_AXES);
    if (n < 0) {
        free(f);
        return NULL;
    }
    f->axes = (uint16_t)n;
    f->weight = idwr_face_weight(font, values, count);
    idwr_metrics_info info;
    idwr_font_metrics_default(font, f->coords, f->axes, &f->metrics, &info);
    idwr_face_apply_mvar(font, f->coords, f->axes, &info, &f->metrics);
    return f;
}

void idwr_face_destroy(idwr_face *f) {
    if (!f) return;
    idwr_face_gdi_release(f);
    idwr_face_color_release(f);
    free(f);
}

uint32_t idwr_face_simulations(const idwr_face *f) { return f->simulations; }

int idwr_face_get_weight(const idwr_face *f) { return (f->simulations & 1) ? 700 : f->weight; }

/* InitializeFontMetrics: the MVAR deltas on the default metrics, by the branch each field came
 * from; nothing without a valid MVAR. Ascent and descent are clamped to 0..0xffff, the descent
 * as the absolute value of the varied raw field. */
void idwr_face_apply_mvar(const idwr_font *font, const float *coords, uint16_t axes, const idwr_metrics_info *info,
                         idwr_font_metrics *m) {
    if (!idwr_mvar_valid(font, axes)) return;
#define D(a, b, c, d) idwr_mvar_delta(font, coords, axes, T(a, b, c, d))
    if (info->os2) {
        if (info->asc_src == 2 || info->asc_src == 3) {
            int32_t v = (info->asc_src == 2 ? D('h', 'c', 'l', 'a') : D('h', 'a', 's', 'c')) + info->asc_raw;
            m->ascent = (uint16_t)(v < 0 ? 0 : v > 0xffff ? 0xffff : v);
        }
        if (info->desc_src == 2 || info->desc_src == 3) {
            int32_t v = (info->desc_src == 2 ? D('h', 'c', 'l', 'd') : D('h', 'd', 's', 'c')) + info->desc_raw;
            v = v < 0 ? -v : v;
            m->descent = (uint16_t)(v > 0xffff ? 0xffff : v);
        }
        if (info->gap_src == 3) m->lineGap = (int16_t)(D('h', 'l', 'g', 'p') + (int16_t)info->gap_raw);
        if (info->xh_src == 2) m->xHeight = (uint16_t)(m->xHeight + D('x', 'h', 'g', 't'));
        if (info->ch_src == 2) m->capHeight = (uint16_t)(m->capHeight + D('c', 'p', 'h', 't'));
        m->subscriptSizeX = (int16_t)(m->subscriptSizeX + D('s', 'b', 'x', 's'));
        m->subscriptSizeY = (int16_t)(m->subscriptSizeY + D('s', 'b', 'y', 's'));
        m->subscriptPositionX = (int16_t)(m->subscriptPositionX + D('s', 'b', 'x', 'o'));
        m->subscriptPositionY = (int16_t)(m->subscriptPositionY - D('s', 'b', 'y', 'o'));
        m->superscriptSizeX = (int16_t)(m->superscriptSizeX + D('s', 'p', 'x', 's'));
        m->superscriptSizeY = (int16_t)(m->superscriptSizeY + D('s', 'p', 'y', 's'));
        m->superscriptPositionX = (int16_t)(m->superscriptPositionX + D('s', 'p', 'x', 'o'));
        m->superscriptPositionY = (int16_t)(m->superscriptPositionY + D('s', 'p', 'y', 'o'));
        if (info->st_src == 2)
            m->strikethroughThickness = (uint16_t)(m->strikethroughThickness + D('s', 't', 'r', 's'));
        else if (info->st_src == 6 && info->post)
            m->strikethroughThickness = (uint16_t)(m->strikethroughThickness + D('u', 'n', 'd', 's'));
        m->strikethroughPosition = (int16_t)(m->strikethroughPosition + D('s', 't', 'r', 'o'));
    }
    if (info->post) {
        m->underlinePosition = (int16_t)(m->underlinePosition + D('u', 'n', 'd', 'o'));
        if (info->ul_src == 5) m->underlineThickness = (uint16_t)(m->underlineThickness + D('u', 'n', 'd', 's'));
    }
#undef D
}

/* DWriteFontFace::GetMetrics (DWRITE_FONT_METRICS1). Oblique: x += y * 0x5700 >> 16 on the
 * glyph box (right from top, left from bottom) and the sub/superscript positions. */
static int16_t shear(int16_t y) { return (int16_t)((int32_t)y * OBLIQUE >> 16); }

static void oblique_metrics(idwr_font_metrics *m) {
    m->glyphBoxRight = (int16_t)(m->glyphBoxRight + shear(m->glyphBoxTop));
    m->glyphBoxLeft = (int16_t)(m->glyphBoxLeft + shear(m->glyphBoxBottom));
    m->subscriptPositionX = (int16_t)(m->subscriptPositionX + shear(m->subscriptPositionY));
    m->superscriptPositionX = (int16_t)(m->superscriptPositionX + shear(m->superscriptPositionY));
}

void idwr_face_get_metrics(const idwr_face *f, idwr_font_metrics *out) {
    *out = f->metrics;
#ifndef IDWR_NEGATIVE_CONTROL_OBLIQUEMETRICS
    if (f->simulations & IDWR_SIM_OBLIQUE) oblique_metrics(out);
#endif
}

/* ConstrainGlyphIndex: ids past the glyph count are glyph 0 */
static uint16_t constrain(const idwr_face *f, uint16_t g) { return g < f->glyph_count ? g : 0; }

/* the design metrics of one glyph of the instance, no simulation (::GetDesignGlyphMetrics) */
static int base_metrics(const idwr_face *f, uint16_t g, idwr_glyph_metrics *m) {
    memset(m, 0, sizeof *m);
    return idwr_get_design_glyph_metrics_instance(f->font, f->coords, f->axes, constrain(f, g), m);
}

/* GlyphHasSize: ink in both directions, in 64-bit arithmetic */
static int has_size(const idwr_glyph_metrics *m) {
    int64_t w = (int64_t)(uint64_t)m->advanceWidth - m->rightSideBearing - m->leftSideBearing;
    int64_t h = (int64_t)(uint64_t)m->advanceHeight - m->bottomSideBearing - m->topSideBearing;
    return w > 0 && h > 0;
}

/* AdjustGlyphMetricsForBoldSimulation: (unitsPerEm + 25) / 50 on each glyph with ink: added to
 * a non-zero advance (or taken from the rsb when the advance is 0), and likewise vertically
 * (advance height and vertical origin, or the top side bearing) */
static void bold_glyph(idwr_glyph_metrics *m, uint32_t upem) {
    if (!has_size(m)) return;
    int32_t d = (int32_t)((upem + 25) / 50);
    if (m->advanceWidth == 0) m->rightSideBearing -= d;
    else m->advanceWidth += (uint32_t)d;
    if (m->advanceHeight == 0) {
        m->topSideBearing -= d;
    } else {
        m->verticalOriginY += d;
        m->advanceHeight += (uint32_t)d;
    }
}

/* AdjustGlyphMetricsForObliqueSimulation: the horizontal bearings take the slant of the ink's
 * bottom and top; sideways, the vertical ones take the slant of half the advance */
static void oblique_glyph(idwr_glyph_metrics *m, int sideways) {
    if (!has_size(m)) return;
    if (sideways) {
        int32_t half = (int32_t)((m->advanceWidth >> 1) * (uint32_t)OBLIQUE) >> 16;
        m->verticalOriginY -= half;
        m->topSideBearing += (m->leftSideBearing * OBLIQUE >> 16) - half;
        m->bottomSideBearing += half - ((int32_t)((m->advanceWidth - (uint32_t)m->rightSideBearing) * (uint32_t)OBLIQUE) >> 16);
    } else {
        m->leftSideBearing += ((m->bottomSideBearing - (int32_t)m->advanceHeight) + m->verticalOriginY) * OBLIQUE >> 16;
        m->rightSideBearing -= (m->verticalOriginY - m->topSideBearing) * OBLIQUE >> 16;
    }
}

int32_t idwr_face_get_design_glyph_metrics(const idwr_face *f, const uint16_t *glyphs, uint32_t count,
                                          idwr_glyph_metrics *out, int sideways) {
    if (count && (!glyphs || !out)) return IDWR_HR_E_INVALIDARG;
    for (uint32_t i = 0; i < count; i++) {
        if (!base_metrics(f, glyphs[i], &out[i])) return IDWR_HR_E_FILEFORMAT;
#ifndef IDWR_NEGATIVE_CONTROL_BOLDMETRICS
        if (f->simulations & IDWR_SIM_BOLD) bold_glyph(&out[i], f->metrics.designUnitsPerEm);
#endif
        if (f->simulations & IDWR_SIM_OBLIQUE) oblique_glyph(&out[i], sideways);
    }
    return 0;
}

/* FontFace::GetDesignGlyphAdvances. Not sideways and not bold: the face's advance array (hmtx;
 * the HVAR advances of an instance with an item store). Otherwise the design metrics' advance
 * width (height sideways), plus (unitsPerEm + 25) / 50 under bold for a glyph with an advance
 * and ink. The oblique simulation does not change advances. */
int32_t idwr_face_get_design_glyph_advances(const idwr_face *f, uint32_t count, const uint16_t *glyphs, int32_t *out,
                                           int sideways) {
    if (count && (!glyphs || !out)) return IDWR_HR_E_INVALIDARG;
    for (uint32_t i = 0; i < count; i++) {
        uint16_t g = constrain(f, glyphs[i]);
        int32_t a;
        if (!sideways && !(f->simulations & IDWR_SIM_BOLD) && f->axes && idwr_hvar_advance(f->font, f->coords, f->axes, g, &a)) {
            out[i] = a;
            continue;
        }
        idwr_glyph_metrics m;
        if (!base_metrics(f, g, &m)) return IDWR_HR_E_FILEFORMAT;
        a = (int32_t)(sideways ? m.advanceHeight : m.advanceWidth);
        if ((f->simulations & IDWR_SIM_BOLD) && a != 0 &&
            (int32_t)m.advanceWidth - m.rightSideBearing - m.leftSideBearing > 0 &&
            (int32_t)m.advanceHeight - m.bottomSideBearing - m.topSideBearing > 0)
            a += (int32_t)((f->metrics.designUnitsPerEm + 25u) / 50u);
        out[i] = a;
    }
    return 0;
}

/* ---- GetGdiCompatibleMetrics ---- */

static int32_t round_half_up(float x) {
    int32_t r = (int32_t)nearbyintf(x);
    return (float)r - x == -0.5f ? r + 1 : r;
}

/* IsZeroWithTolerance */
static int near_zero(float v) { return v < 1.5258789e-05f && v > -1.5258789e-05f; }

/* OpenTypeFontFaceBuilder::ReadVerticalDeviceMetrics: the group of the first ratio record with
 * bCharSet 1 and xRatio 0 or 1; NULL (n 0) when there is none */
static const uint8_t *vdmx_group(const idwr_font *font, uint16_t *n) {
    uint32_t len;
    const uint8_t *v = idwr_font_table(font, T('V', 'D', 'M', 'X'), &len);
    *n = 0;
    if (!v || len < 6) return NULL;
    uint16_t ratios = idwr_u16(v + 4);
    if ((len - 6) / 4 < ratios) return NULL;
    uint32_t offs = 6 + 4u * ratios;
    if (len < offs || (len - offs) / 2 < ratios) return NULL;
    for (uint16_t i = 0; i < ratios; i++) {
        if (v[6 + 4 * i] != 1 || v[7 + 4 * i] >= 2) continue;
        uint32_t at = idwr_u16(v + offs + 2 * i);
        if (len < at || len - at < 4) return NULL;
        uint16_t recs = idwr_u16(v + at);
        at += 4;
        if (at > len || recs > (len - at) / 6) return NULL;
        *n = recs;
        return v + at;
    }
    return NULL;
}

/* round(v / unit) (when it fits a float's integers) times unit, rounded */
static int16_t snap(int32_t v, float unit) {
    float p = (float)v / unit;
    if (fabsf(p) < 8388608.0f) p = (float)round_half_up(p);
    return (int16_t)round_half_up(p * unit);
}

int32_t idwr_face_get_gdi_compatible_metrics(const idwr_face *f, float em, float ppd, const idwr_matrix *t,
                                            idwr_font_metrics *out) {
    memset(out, 0, sizeof *out);
    if (!(em > 0.0f) || !(ppd > 0.0f)) return IDWR_HR_E_INVALIDARG;
    if (t && (t->m11 != t->m11 || t->m12 != t->m12 || t->m21 != t->m21 || t->m22 != t->m22 || t->dx != t->dx ||
              t->dy != t->dy))
        return IDWR_HR_E_INVALIDARG;
    idwr_face_get_metrics(f, out);
    float scale = 1.0f;
    if (t) {
        /* a uniform scale, or a quarter turn of one: its size; otherwise the length of the
         * transformed y unit vector */
        int uniform = 0;
        float a = 0, b = 0;
        if (near_zero(t->m12)) {
            if (near_zero(t->m21)) a = t->m11, b = t->m22, uniform = 1;
        } else if (near_zero(t->m11) && near_zero(t->m22)) {
            a = t->m12, b = t->m21, uniform = 1;
        }
        if (uniform && near_zero(fabsf(a) - fabsf(b))) {
            scale = fabsf(a);
        } else {
            float y = t->m12 * 0.0f + t->m22, x = t->m11 * 0.0f + t->m21;
            scale = sqrtf(x * x + y * y);
        }
    }
    float px = em * ppd * scale;
    if (near_zero(px)) return 0;
    float unit = (float)out->designUnitsPerEm / px;
    if (near_zero(unit)) return 0;
    int32_t ppem = round_half_up(px);
    uint16_t n;
    const uint8_t *g = vdmx_group(f->font, &n);
    const uint8_t *rec = NULL;
    for (uint16_t lo = 0, hi = n; lo < hi;) {   /* binary search on yPelHeight */
        uint16_t mid = (uint16_t)(lo + (hi - lo) / 2);
        int32_t d = ppem - (int32_t)idwr_u16(g + 6 * mid);
        if (d < 0) hi = mid;
        else if (d > 0) lo = (uint16_t)(mid + 1);
        else { rec = g + 6 * mid; break; }
    }
    if (rec && 0.0f < px && px < 255.0f) {
        out->ascent = (uint16_t)round_half_up((float)idwr_i16(rec + 2) * unit);
        out->descent = (uint16_t)round_half_up((float)idwr_i16(rec + 4) * -unit);
    } else {
        out->ascent = (uint16_t)snap(out->ascent, unit);
        out->descent = (uint16_t)snap(out->descent, unit);
    }
    out->lineGap = snap(out->lineGap, unit);
    out->capHeight = (uint16_t)snap(out->capHeight, unit);
    out->xHeight = (uint16_t)snap(out->xHeight, unit);
    out->underlinePosition = snap(out->underlinePosition, unit);
    out->underlineThickness = (uint16_t)snap(out->underlineThickness, unit);
    out->strikethroughPosition = snap(out->strikethroughPosition, unit);
    out->strikethroughThickness = (uint16_t)snap(out->strikethroughThickness, unit);
    out->glyphBoxLeft = snap(out->glyphBoxLeft, unit);
    out->glyphBoxTop = snap(out->glyphBoxTop, unit);
    out->glyphBoxRight = snap(out->glyphBoxRight, unit);
    out->glyphBoxBottom = snap(out->glyphBoxBottom, unit);
    out->subscriptPositionX = snap(out->subscriptPositionX, unit);
    out->subscriptPositionY = snap(out->subscriptPositionY, unit);
    out->subscriptSizeX = snap(out->subscriptSizeX, unit);
    out->subscriptSizeY = snap(out->subscriptSizeY, unit);
    out->superscriptPositionX = snap(out->superscriptPositionX, unit);
    out->superscriptPositionY = snap(out->superscriptPositionY, unit);
    out->superscriptSizeX = snap(out->superscriptSizeX, unit);
    out->superscriptSizeY = snap(out->superscriptSizeY, unit);
    return 0;
}

/* IsMonospacedFont 0x18005def8 (the face flag IDWriteFontFace1::IsMonospacedFont reads): the
 * PANOSE proportion (9 for families 0, 2, 4; 3 for families 3, 5), else post.isFixedPitch */
int idwr_font_is_monospaced(const idwr_font *font) {
    uint32_t len;
    const uint8_t *os2 = idwr_font_table(font, T('O', 'S', '/', '2'), &len);
    uint8_t family = 0, proportion = 0;
    if (os2 && len >= 42) family = os2[32], proportion = os2[35];
    if (family == 0 || family == 2 || family == 4) {
        if (proportion == 9) return 1;
    } else if (family == 3 || family == 5) {
        if (proportion == 3) return 1;
    }
    const uint8_t *post = idwr_font_table(font, T('p', 'o', 's', 't'), &len);
    if (!post || len < 16) return 0;
    return (post[12] | post[13] | post[14] | post[15]) != 0;
}

int idwr_face_recommended_rendering_mode(const idwr_face *f, float em, float ppd, int measuring, int smoothing) {
    return idwr_recommended_rendering_mode_sim(f->font, f->coords, f->axes, em, ppd, measuring, smoothing, f->simulations,
                                               f->weight);
}
