/*
 * The run-level part of IDWriteGlyphRunAnalysis (DWrite.dll 10.0.26100.9711), without DWrite:
 * GlyphRunAnalysis::GlyphRunAnalysis 0x1800fa644 up to rasterizing (steps a-f, h, i and the
 * reordering of j in the comment at the top of reimpl/truetype/glyphrun_layout.c, which is the
 * spec), ComputeGlyphOrigins 0x180049844 (with MetricsConverter and the design metrics
 * simulations), GetTransformMaxScaleFactor 0x180075424, the two PartitionArray instances, and
 * the bounds getters GetAlphaTextureBounds 0x1800c6100 / GetRunBitmapBounds 0x1801233d0.
 * The formulas follow the port tt_GlyphRunAnalysis_ctor; FontFace fields come from the font:
 *   [*face]+0x60 bit 0x10  head.flags bit 3 (integer ppem);
 *   [*face]+0x60 bit 4     IsThinFontFamily (the family name in a list of 6);
 *   face+0x128             gasp SYMMETRIC_GRIDFIT ranges (idwr_gasp_flag);
 *   [face+0x108]+0x14 & 1  IsLegacyEastAsian (idwr_is_legacy_east_asian);
 *   [*face]+0x58           GetFontWeightFromFace (OS/2 usWeightClass, fsSelection, head).
 * Checked against DWrite's own objects by reimpl/glyphrun_params_check.py.
 */
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "idwr_internal.h"

/* single precision, evaluated exactly as written (no FMA contraction) */
#pragma STDC FP_CONTRACT OFF
#ifdef __clang__
#pragma clang fp contract(off)
#endif

/* ---------------------------------------------------------------------------------------- */
/* float helpers, bit-exact with the original's SSE sequences                                */
/* ---------------------------------------------------------------------------------------- */

/* cvtss2si: round to nearest even; NaN and out of range give INT32_MIN */
static int32_t cvt_rne(float v) {
    if (!(v >= -2147483648.0f && v < 2147483648.0f)) return INT32_MIN;
    return (int32_t)nearbyintf(v);
}

/* the original's round half up: i = cvtss2si(v); if ((float)i - v == -0.5) i++ */
static int32_t rhu(float v) {
    int32_t i = cvt_rne(v);
    if ((float)i - v == -0.5f) i++;
    return i;
}

/* IsZeroWithTolerance 0x18004c348 */
static int zero_tol(float v) { return 1.52587890625e-05f > v && v > -1.52587890625e-05f; }

/* Fixed16_16::FloatToFixedInt32 0x18004c304: v * 65536 * 65536 truncated to int64 (cvttss2si:
 * NaN and out of range give INT64_MIN), + 0x8000, >> 16, clamped to int32 */
static int32_t float_to_fixed(float v) {
    v = v * 65536.0f;
    v = v * 65536.0f;
    int64_t t = (v >= -9223372036854775808.0f && v < 9223372036854775808.0f) ? (int64_t)v : INT64_MIN;
    t = (t + 0x8000) >> 16;
    if (t < INT32_MIN) return INT32_MIN;
    if (t > INT32_MAX) return INT32_MAX;
    return (int32_t)t;
}

/* andps with 0x7fffffff */
static float fabs_bits(float v) {
    union { float f; uint32_t u; } x;
    x.f = v;
    x.u &= 0x7fffffffu;
    return x.f;
}

/* maxss a, b */
static float maxss(float a, float b) { return a > b ? a : b; }

/* ---------------------------------------------------------------------------------------- */
/* rendering mode attributes                                                                  */
/* ---------------------------------------------------------------------------------------- */

/* DWrite.dll RVA 0x1d8420: uint32[antialias][DWRITE_RENDERING_MODE1]. Byte 0 = overscale
 * x | y << 4, byte 1 = their log2, bits 16 ClearType, 17 grayscale, 19 GDI-compatible,
 * 20 natural, 21 GDI natural, 22 grayscale vertical, 23 strikes, byte 3 = raster type << 2 |
 * ClearType filter margin. Modes 0 (default) and 6 (outline) have no overscale. The
 * constructor reads this table for the requested mode. */
static const uint32_t MODE_ATTRIBUTES[2][8] = {
    { 0xfc000000u, 0x00b80011u, 0x06bd0016u, 0x069d0016u, 0x06910016u, 0x0a110056u, 0xfc000000u, 0x0a110056u },
    { 0xfc000000u, 0x00b80011u, 0x0cba0318u, 0x0c9a0318u, 0x0c920318u, 0x10522244u, 0xfc000000u, 0x14121122u },
};

/* GetRenderingModeAttributes 0x1800fb630 reads two other tables (ClearType 0x1e1268,
 * grayscale 0x1e1240, 10 modes each), which differ from the one above: the natural symmetric
 * modes 5 and 7 have bit 23 (strikes) set, and 8, 9 are GDI classic / natural symmetric
 * (ClearType 6x5, grayscale 4x4). The constructor uses it for the NATURAL switch. */
static const uint32_t GET_MODE_ATTRIBUTES[2][10] = {
    { 0xfc000000u, 0x00b80011u, 0x06bd0016u, 0x069d0016u, 0x06910016u, 0x0a910056u, 0xfc000000u, 0x0a910056u,
      0x0abd0056u, 0x0a9d0056u },
    { 0xfc000000u, 0x00b80011u, 0x0cba0318u, 0x0c9a0318u, 0x0c920318u, 0x10d22244u, 0xfc000000u, 0x14921122u,
      0x10fa2244u, 0x10da2244u },
};

uint32_t idwr_rendering_mode_attributes(int mode, int aa) {
    if ((unsigned)mode >= 10 || (unsigned)aa >= 2) return 0;
    return GET_MODE_ATTRIBUTES[aa][mode];
}

/* GetTransformMaxScaleFactor 0x180075424 */
float idwr_transform_max_scale_factor(const idwr_matrix *m) {
    if (m == NULL) return 1.0f;
    if (m->m12 == 0.0f && m->m21 == 0.0f) {
        float r = fabs_bits(m->m22);
        if (fabs_bits(m->m22) <= fabs_bits(m->m11)) r = fabs_bits(m->m11);
        return r;
    }
    float a = m->m11 * m->m11 + m->m12 * m->m12;
    float b = m->m22 * m->m22 + m->m21 * m->m21;
    float c = m->m22 * m->m12 + m->m21 * m->m11;
    float d = a - b;
    float s = sqrtf(c * 4.0f * c + d * d);
    return sqrtf((b + a + s) * 0.5f);
}

/* ---------------------------------------------------------------------------------------- */
/* font accessors                                                                             */
/* ---------------------------------------------------------------------------------------- */

/* idwr_font_weight: idwr/nameweight.c */

/* OpenTypeFontFaceBuilder::IsThinFontFamily 0x18005f1d8: the list at 0x180199bf8 */
static const char *const THIN_FAMILIES[6] = {
    "Courier New", "Fixed Miriam Transparent", "Miriam Fixed", "Rod", "Rod Transparent", "Simplified Arabic Fixed",
};

/* the Windows (platform 3) name `id`, US English first, as ASCII; 0 if absent or not ASCII */
static int name_ascii(const idwr_font *f, uint16_t id, char *out, size_t cap) {
    uint32_t len;
    const uint8_t *t = idwr_font_table(f, IDWR_TAG('n', 'a', 'm', 'e'), &len);
    if (!t || len < 6) return 0;
    uint16_t count = idwr_u16(t + 2), storage = idwr_u16(t + 4);
    if (6 + 12 * (uint32_t)count > len) return 0;
    const uint8_t *best = NULL;
    for (uint16_t i = 0; i < count; i++) {
        const uint8_t *r = t + 6 + 12 * i;
        if (idwr_u16(r) != 3 || idwr_u16(r + 6) != id) continue;
        uint16_t enc = idwr_u16(r + 2);
        if (enc != 0 && enc != 1) continue;
        if (idwr_u16(r + 4) == 0x409) { best = r; break; }
        if (!best) best = r;
    }
    if (!best) return 0;
    uint32_t n = idwr_u16(best + 8), off = (uint32_t)storage + idwr_u16(best + 10);
    if (off + n > len || n / 2 + 1 > cap) return 0;
    for (uint32_t i = 0; i < n / 2; i++) {
        uint16_t c = idwr_u16(t + off + 2 * i);
        if (c == 0 || c > 0x7f) return 0;
        out[i] = (char)c;
    }
    out[n / 2] = 0;
    return 1;
}

int idwr_font_is_thin_family(const idwr_font *f) {
    char family[128];
    if (!name_ascii(f, 1, family, sizeof family)) return 0;
    for (int i = 0; i < 6; i++)
        if (strcmp(family, THIN_FAMILIES[i]) == 0) return 1;
    return 0;
}

/* GlyphHasSize 0x18009db2c */
static int glyph_has_size(const idwr_glyph_metrics *m) {
    int64_t w = (int64_t)m->advanceWidth - m->rightSideBearing - m->leftSideBearing;
    int64_t h = (int64_t)m->advanceHeight - m->bottomSideBearing - m->topSideBearing;
    return w > 0 && h > 0;
}

/* FontFace::GetDesignGlyphMetrics 0x1800525a4: design metrics, then
 * AdjustGlyphMetricsForBoldSimulation 0x18009cad0 and ...ForObliqueSimulation 0x18009c9f4.
 * A CFF face's instance takes the instance's metrics (OpenTypeCffFontMetricsBuilder on the
 * instance's CffRasterizer); a TrueType instance still the default instance's. */
static void design_metrics(const idwr_font *f, const float *coords, uint16_t axes, uint16_t upem, uint32_t sims,
                           int sideways, uint16_t glyph, idwr_glyph_metrics *m) {
    memset(m, 0, sizeof *m);
    if (axes && idwr_font_is_cff(f))
        idwr_get_design_glyph_metrics_instance(f, coords, axes, glyph, m);
    else
        idwr_get_design_glyph_metrics(f, glyph, m);
    if (sims & IDWR_SIM_BOLD) {
        int32_t k = (int32_t)((upem + 0x19u) / 0x32u);
        if (glyph_has_size(m)) {
            if (m->advanceWidth == 0) m->rightSideBearing -= k;
            else m->advanceWidth += (uint32_t)k;
            if (m->advanceHeight == 0) {
                m->topSideBearing -= k;
            } else {
                m->verticalOriginY += k;
                m->advanceHeight += (uint32_t)k;
            }
        }
    }
    if (sims & IDWR_SIM_OBLIQUE) {
        if (!glyph_has_size(m)) return;
        if (sideways) {
            int32_t h = (int32_t)((m->advanceWidth >> 1) * 0x5700u) >> 16;
            m->verticalOriginY -= h;
            m->topSideBearing += (m->leftSideBearing * 0x5700 >> 16) - h;
            m->bottomSideBearing += h - ((int32_t)((m->advanceWidth - (uint32_t)m->rightSideBearing) * 0x5700u) >> 16);
        } else {
            m->leftSideBearing += (((int32_t)m->bottomSideBearing - (int32_t)m->advanceHeight) + m->verticalOriginY) * 0x5700 >> 16;
            m->rightSideBearing -= (m->verticalOriginY - m->topSideBearing) * 0x5700 >> 16;
        }
    }
}

/* ---------------------------------------------------------------------------------------- */
/* ComputeGlyphOrigins 0x180049844                                                            */
/* ---------------------------------------------------------------------------------------- */

/* MetricsConverter::MetricsConverter 0x180049674: {pixel scale, DIP factor} */
typedef struct { float px, k; } metrics_converter;

static metrics_converter converter(uint16_t upem_u16, int measuring, float em, float scale, const float *m) {
    float px = 1.0f, upem = (float)upem_u16;
    if (measuring != 0) {
        float s = 1.0f;
        int ok = 1;
        if (m != NULL) {
            float u, v;
            s = m[1];
            if (zero_tol(s)) {
                if (!zero_tol(m[2])) ok = 0;
                u = m[0];
                v = m[3];
            } else {
                if (!zero_tol(m[0]) || !zero_tol(m[3])) ok = 0;
                u = s;
                v = m[2];
            }
            if (ok) {
                s = fabs_bits(u);
                if (!zero_tol(s - fabs_bits(v))) ok = 0;
            }
        }
        if (ok) px = (em * scale * s) / upem;
    }
    metrics_converter c = { px, em / (px * upem) };
    return c;
}

/* MetricsConverter::DesignToClientUnits 0x180049428 */
static float design_to_client(const metrics_converter *c, int32_t v) {
    float f = (float)v;
    if (c->px != 1.0f && (f = f * c->px, fabs_bits(f) < 8388608.0f)) {
        int32_t i = cvt_rne(f);
        if ((float)i - f == -0.5f) i++;
        f = (float)i;
    }
    return f * c->k;
}

/* origins[2 * count]: x, y per glyph in DIPs; returns `horizontal` */
static int glyph_origins(const idwr_font *f, const idwr_glyph_run *run, float em, float scale, const float *matrix,
                         const idwr_glyph_metrics *metrics, float x, float y, float *o) {
    uint32_t n = run->count;
    int horizontal = 1;
    int rtl = run->bidi_level & 1, sideways = run->sideways != 0;
    const float *adv = run->advances;
    if (n == 0) return horizontal;
    if (rtl || sideways || adv == NULL) {
        uint16_t upem = f->head && f->head_len >= 20 ? idwr_u16(f->head + 18) : 0;
        metrics_converter c = converter(upem, run->measuring_mode, em, scale, matrix);
        if (sideways) {
            horizontal = 0;
            for (uint32_t i = 0; i < n; i++) {
                const idwr_glyph_metrics *g = &metrics[i];
                float w = design_to_client(&c, (int32_t)g->advanceWidth);
                float h = design_to_client(&c, (int32_t)g->advanceHeight);
                float a = adv ? adv[i] : h;
                float px = x;
                if (rtl) {
                    px = x - h;
                    x = x - a;
                } else {
                    x = x + a;
                }
                o[2 * i] = px;
                o[2 * i] = design_to_client(&c, g->verticalOriginY) + o[2 * i];
                o[2 * i + 1] = w * 0.5f + y;
            }
        } else {
            for (uint32_t i = 0; i < n; i++) {
                float a = design_to_client(&c, (int32_t)metrics[i].advanceWidth);
                if (rtl) {
                    o[2 * i] = x - a;
                    if (adv) a = adv[i];
                    x = x - a;
                } else {
                    o[2 * i] = x;
                    x = x + a;
                }
                o[2 * i + 1] = y;
            }
        }
    } else {
        for (uint32_t i = 0; i < n; i++) {
            o[2 * i] = x;
            o[2 * i + 1] = y;
            x = x + adv[i];
        }
    }
    if (run->offsets != NULL) {
        horizontal = 0;
        for (uint32_t i = 0; i < n; i++) {
            if (rtl) {
                o[2 * i] = o[2 * i] - run->offsets[i].advanceOffset;
            } else {
                o[2 * i] = run->offsets[i].advanceOffset + o[2 * i];
            }
            o[2 * i + 1] = o[2 * i + 1] - run->offsets[i].ascenderOffset;
        }
    }
    if (matrix != NULL) {
        if (matrix[1] != 0.0f || matrix[2] != 0.0f) horizontal = 0;
        for (uint32_t i = 0; i < n; i++) {
            float ox = o[2 * i], oy = o[2 * i + 1];
            o[2 * i] = oy * matrix[2] + ox * matrix[0] + matrix[4];
            o[2 * i + 1] = ox * matrix[1] + oy * matrix[3] + matrix[5];
        }
    }
    return horizontal;
}

/* ---------------------------------------------------------------------------------------- */
/* GlyphRunAnalysis::GlyphRunAnalysis, before GetGlyphData                                    */
/* ---------------------------------------------------------------------------------------- */

int idwr_glyph_run_analyze(const idwr_font *font, const idwr_glyph_run *run, idwr_glyph_run_analysis *a, float *origins) {
    return idwr_glyph_run_analyze_instance(font, NULL, 0, run, a, origins);
}

int idwr_glyph_run_analyze_instance(const idwr_font *font, const float *coords, uint16_t axes, const idwr_glyph_run *run,
                                   idwr_glyph_run_analysis *a, float *origins) {
    memset(a, 0, sizeof *a);
    uint32_t count = run->count;
    int aa = run->antialias, mode = run->rendering_mode;
    float scale = run->pixels_per_dip, x = run->origin_x, y = run->origin_y;
    a->count = count;
    a->antialias = (uint32_t)aa;
    a->grayscale = aa == 1;
    int head_int_ppem = font->head && font->head_len >= 18 && (idwr_u16(font->head + 16) & 8);
    a->thin_family = (uint8_t)idwr_font_is_thin_family(font);

    float mstore[6];
    const float *matrix = NULL;
    if (run->transform != NULL) {
        const idwr_matrix *t = run->transform;
        mstore[0] = t->m11; mstore[1] = t->m12; mstore[2] = t->m21;
        mstore[3] = t->m22; mstore[4] = t->dx; mstore[5] = t->dy;
        matrix = mstore;
        /* a transform that is only a uniform scale folds into the scale and the origin */
        const float tol = 1.52587890625e-05f;
        if (tol > matrix[1] && matrix[1] > -tol && tol > matrix[2] && matrix[2] > -tol) {
            float d = matrix[3] - matrix[0];
            if (tol > d && d > -tol) {
                x = x + matrix[4] / matrix[0];
                y = y + matrix[5] / matrix[0];
                scale = scale * matrix[0];
                matrix = NULL;
            }
        }
    }
    a->scale = scale;
    a->origin_x = x;
    a->origin_y = y;
    if (matrix != NULL) {
        a->has_transform = 1;
        a->transform = *run->transform;
    }
    if ((unsigned)mode >= 8 || (unsigned)aa >= 2) return IDWR_E_INVALIDARG;
    uint32_t attr = MODE_ATTRIBUTES[aa][mode];
    a->attributes = attr;
    a->rendering_mode = mode;
    if ((attr & 0xf) == 0) return IDWR_E_INVALIDARG;

    int sideways = run->sideways != 0;
    int round_em = (attr >> 19 & 1) || head_int_ppem;
    float run_em = run->em_size;
    float s = run_em * scale;
    int32_t m[4] = { 0, 0, 0, 0 };
    uint8_t ppem = 0;
    if (matrix == NULL) {
        int32_t f = round_em ? (int32_t)((uint32_t)rhu(s) << 16) : float_to_fixed(s);
        if (!sideways) { m[0] = f; m[3] = f; }
        else { m[1] = (int32_t)(0u - (uint32_t)f); m[2] = f; }
        int32_t v = (int32_t)(0u - (uint32_t)f);
        if (v < 0) v = f;
        if (v <= 0xffffff) ppem = (uint8_t)(v >> 16);
    } else {
        float r0x, r0y, r1x, r1y;
        if (!sideways) { r0x = s; r0y = 0.0f; r1x = 0.0f; r1y = s; }
        else { r0x = 0.0f; r0y = -run_em * scale; r1x = s; r1y = 0.0f; }
        float pa = matrix[2] * r0y + matrix[0] * r0x;
        float pb = matrix[3] * r0y + matrix[1] * r0x;
        float pc = matrix[2] * r1y + matrix[0] * r1x;
        float pd = matrix[3] * r1y + matrix[1] * r1x;
        int axis = 0;
        float p = 0, q = 0;
        if (zero_tol(pb)) {
            if (zero_tol(pc)) { axis = 1; p = pa; q = pd; }
        } else if (zero_tol(pa) && zero_tol(pd)) {
            axis = 1; p = pb; q = pc;
        }
        if (axis) {
            p = fabs_bits(p);
            q = fabs_bits(q);
            if (zero_tol(p - q)) {
                if (0.0f > p || p >= 32768.0f) return IDWR_E_INVALIDARG;
                int32_t ip = rhu(p);
                ppem = (uint32_t)ip > 0xff ? 0 : (uint8_t)ip;
                if (round_em && !zero_tol(p)) {
                    float k = (float)(uint32_t)ip / p;
                    pa = k * pa; pb = k * pb; pc = k * pc; pd = k * pd;
                }
            }
        }
        m[0] = float_to_fixed(pa);
        m[2] = float_to_fixed(-pb);
        m[1] = float_to_fixed(-pc);
        m[3] = float_to_fixed(pd);
    }

    /* natural mode under a rotation or skew: natural symmetric, or 2x2 above 172.8 dpi */
    if ((m[0] != 0 || m[3] != 0) && (m[2] != 0 || m[1] != 0) && mode == 4 && run->keep_mode == 0) {
        float dpi = scale * 96.0f;
        dpi = dpi * 100.0f;
        int m2 = !(dpi >= 17280.0f) ? 5 : 7;
        attr = GET_MODE_ATTRIBUTES[aa][m2];
        a->attributes = attr;
        a->rendering_mode = m2;
    }

    /* grid fit DEFAULT: DISABLED or ENABLED */
    int gridfit = run->grid_fit;
    if (gridfit == 0) {
        float dpi = scale * 96.0f;
        float sc = dpi / 96.0f;
        int kind;
        if (dpi == dpi && matrix == NULL) {
            kind = sideways ? 3 : 1;
        } else {
            float M[6] = { sc, 0.0f, 0.0f, sc, 0.0f, 0.0f };
            if (matrix != NULL) {
                M[0] = sc * matrix[0];
                M[1] = sc * matrix[1];
                M[2] = sc * matrix[2];
                M[3] = sc * matrix[3];
            }
            int k;
            float u = 0, v = 0;
            if (zero_tol(M[1])) {
                if (zero_tol(M[2])) { k = 1; u = M[3]; v = M[0]; }
                else k = 4;
            } else if (zero_tol(M[0])) {
                if (zero_tol(M[3])) { k = 3; u = M[2]; v = M[1]; }
                else k = 6;
            } else if (zero_tol(M[2])) {
                k = 4;
            } else {
                k = zero_tol(M[3]) ? 6 : 0;
            }
            if (k == 1 || k == 3) {
                sc = maxss(fabs_bits(u), fabs_bits(v));
                kind = sideways ? (k ^ 2) : k;
            } else {
                kind = k;
                idwr_matrix mm = { M[0], M[1], M[2], M[3], M[4], M[5] };
                sc = idwr_transform_max_scale_factor(&mm);
            }
        }
        float pf = sc * run_em;
        uint32_t pp = (uint32_t)rhu(pf);
        gridfit = 1;
        if (kind & 5) {
            if (attr >> 19 & 1) {
                gridfit = 2;
            } else if (attr >> 20 & 1) {
                if (pp > 0xffff) pp = 0xffff;
                else if (pp < 1) pp = 1;
                /* the gasp SYMMETRIC_GRIDFIT ranges; no list means enabled */
                if (idwr_gasp_flag_instance(font, coords, axes, 4, (int32_t)pp) != 0) gridfit = 2;
            }
        }
    }
    a->grid_fit = gridfit;

    uint32_t flags = 0;
    if (count != 0) {
        if (!idwr_is_legacy_east_asian(font)) a->attributes &= ~0x800000u;
        attr = a->attributes;
        flags = (run->simulations & 1) * 2;
        if (run->simulations & 2) flags |= (sideways ? 8u : 0u) + 4;
        if (gridfit == 2) flags |= 1;
        if (flags & 1) {
            if (attr >> 19 & 1) flags |= (attr >> 21 & 1) ? 0x10 : 0x20;
            if ((attr >> 23 & 1) || ((attr & 0xf) == 1 && (attr & 0xf0) == 0x10)) flags |= 0x40;
        }
        if (attr >> 17 & 1) flags |= 0x80;
        memcpy(a->raster.matrix, m, sizeof m);
        a->raster.point_size = rhu(run_em * 0.75f);
        a->raster.overscale_x = attr & 0xf;
        a->raster.overscale_y = attr >> 4 & 0xf;
        a->raster.flags = flags;
        a->raster.ppem = ppem;
        /* GlyphBitmapRasterizationState: thin fonts get their 1-bit bitmaps widened */
        int weight = (run->simulations & 1) ? 700 : idwr_font_weight(font);
        a->spread = a->thin_family && a->raster.overscale_x > 1 && weight <= 500 && (flags & 0x42) == 0;

        const idwr_glyph_metrics *metrics = run->glyph_metrics;
        idwr_glyph_metrics local[64];
        idwr_glyph_metrics *heap = NULL;
        int need = (run->bidi_level & 1) || sideways || run->advances == NULL;
        if (need && metrics == NULL) {
            if (run->measuring_mode != IDWR_MEASURING_NATURAL) return IDWR_E_NEED_METRICS;
            idwr_glyph_metrics *dst = local;
            if (count > 64) {
                heap = (idwr_glyph_metrics *)malloc(sizeof *heap * count);
                if (!heap) return IDWR_E_INVALIDARG;
                dst = heap;
            }
            uint16_t upem = font->head && font->head_len >= 20 ? idwr_u16(font->head + 18) : 0;
            /* non-sideways runs only read the advance, which oblique does not change */
            for (uint32_t i = 0; i < count; i++)
                design_metrics(font, coords, axes, upem, run->simulations, sideways, run->glyphs[i], &dst[i]);
            metrics = dst;
        }
        a->horizontal = (uint8_t)glyph_origins(font, run, run_em, scale, matrix, metrics, x, y, origins);
        free(heap);
    }
    return 0;
}

/* ---------------------------------------------------------------------------------------- */
/* GlyphRunAnalysis::GlyphRunAnalysis, after GetGlyphData                                     */
/* ---------------------------------------------------------------------------------------- */

/* ComputeGlyphBitmapPositions 0x1800c6b08 (and Internal<1> 0x1800c6cac when horizontal) */
static void bitmap_positions(const idwr_glyph_layout *const *lay, uint32_t n, float scale, uint32_t ox, uint32_t oy,
                             int horizontal, const float *org, int32_t *pos, int32_t *rect) {
    float sx = (float)(int64_t)ox * scale;
    float sy = (float)(int64_t)oy * scale;
    int32_t left = INT32_MAX, top = INT32_MAX, right = INT32_MIN, bottom = INT32_MIN;
    int32_t y0 = n ? rhu(sy * org[1]) : 0;
    for (uint32_t i = 0; i < n; i++) {
        const idwr_glyph_layout *l = lay[i];
        int32_t px = rhu(sx * org[2 * i]) + l->left;
        int32_t py = (horizontal ? y0 : rhu(sy * org[2 * i + 1])) - l->top;
        pos[2 * i] = px;
        pos[2 * i + 1] = py;
        if (l->width != 0 && l->height != 0) {
            int32_t r = l->width + px, b = l->height + py;
            if (px < left) left = px;
            if (py < top) top = py;
            if (r > right) right = r;
            if (b > bottom) bottom = b;
        }
    }
    rect[0] = left; rect[1] = top; rect[2] = right; rect[3] = bottom;
}

static void swap_slots(uint32_t i, uint32_t j, uint32_t *order, const idwr_glyph_layout **lay, float *org, int32_t *pos) {
    uint32_t t = order[i]; order[i] = order[j]; order[j] = t;
    const idwr_glyph_layout *l = lay[i]; lay[i] = lay[j]; lay[j] = l;
    if (org) {
        float a = org[2 * i], b = org[2 * i + 1];
        org[2 * i] = org[2 * j]; org[2 * i + 1] = org[2 * j + 1];
        org[2 * j] = a; org[2 * j + 1] = b;
    }
    if (pos) {
        int32_t a = pos[2 * i], b = pos[2 * i + 1];
        pos[2 * i] = pos[2 * j]; pos[2 * i + 1] = pos[2 * j + 1];
        pos[2 * j] = a; pos[2 * j + 1] = b;
    }
}

void idwr_glyph_run_place(idwr_glyph_run_analysis *a, const idwr_glyph_layout *layouts, const float *origins,
                         uint32_t *order, int32_t *positions) {
    uint32_t count = a->count;
    memset(a->rect, 0, sizeof a->rect);
    memset(a->aliased_rect, 0, sizeof a->aliased_rect);
    a->outline_count = 0;
    a->odd_count = 0;
    float scale = a->scale;
    if (count != 0) {
        const idwr_glyph_layout *small_lay[64];
        float small_org[128];
        const idwr_glyph_layout **lay = count <= 64 ? small_lay : (const idwr_glyph_layout **)malloc(sizeof *lay * count);
        float *org = count <= 64 ? small_org : (float *)malloc(sizeof *org * 2 * count);
        if (!lay || !org) {
            if (lay != small_lay) free(lay);
            if (org != small_org) free(org);
            return;
        }
        uint32_t any = 0, all = 0xffffffffu;
        for (uint32_t i = 0; i < count; i++) {
            order[i] = i;
            lay[i] = &layouts[i];
            org[2 * i] = origins[2 * i];
            org[2 * i + 1] = origins[2 * i + 1];
            any |= layouts[i].flags;
            all &= layouts[i].flags;
        }
        if (any & 4) a->grayscale = 1;

        /* PartitionArray 0x1800fb708: glyphs with layout flag 1 (embedded bitmaps) go last */
        uint32_t n;
        if ((any & 1) == 0) {
            n = count;
        } else if (all & 1) {
            n = 0;
        } else {
            uint32_t i = 0, j = count - 1;
            if (j != 0) {
                do {
                    if ((lay[i]->flags & 1) == 0) {
                        i++;
                    } else {
                        if ((lay[j]->flags & 1) == 0) {
                            swap_slots(i, j, order, lay, org, NULL);
                            i++;
                        }
                        j--;
                    }
                } while (i < j);
            }
            if (i == j && (lay[i]->flags & 1) == 0) i++;
            n = i;
        }
        a->outline_count = n;
        uint32_t attr = a->attributes;
        if (n != 0) {
            /* layout flag 2 (or the GDI-compatible modes): whole-pixel glyph origins */
            if (!((any & 2) == 0 && (attr & 0x80000) == 0)) {
                float rs = 1.0f / scale;
                for (uint32_t i = 0; i < n; i++) {
                    if ((lay[i]->flags & 2) || (attr & 0x80000)) {
                        float v = scale * org[2 * i];
                        if (8388608.0f > fabs_bits(v)) v = (float)rhu(v);
                        org[2 * i] = v * rs;
                    }
                }
            }
            bitmap_positions(lay, n, scale, attr & 0xf, attr >> 4 & 0xf, a->horizontal, org, positions, a->rect);
            /* vertical overscale: the rows start and end on a whole output pixel */
            int32_t d = attr >> 4 & 0xf;
            if (d > 1) {
                int32_t *R = a->rect;
                int32_t r = R[1] % d;
                int32_t t = R[1] - r;
                if (r < 0) t -= d;
                R[1] = t;
                r = R[3] % d;
                R[3] = r > 0 ? (d - r) + R[3] : R[3] - r;
            }
        }
        if (n < count)
            bitmap_positions(lay + n, count - n, scale, 1, 1, a->horizontal, org + 2 * n, positions + 2 * n,
                             a->aliased_rect);

        /* 2x2: PartitionArray 0x180151b8c moves the glyphs at an odd (x ^ y) subpixel first */
        if ((attr & 0xf) == 2 && count > 1) {
            uint32_t i = 0, j = count - 1;
            do {
                if (((positions[2 * i + 1] ^ positions[2 * i]) & 1) == 0) {
                    uint32_t k = j--;
                    if (((positions[2 * k + 1] ^ positions[2 * k]) & 1) != 0) {
                        swap_slots(i, k, order, lay, NULL, positions);
                        i++;
                    }
                } else {
                    i++;
                }
            } while (i < j);
            if (i == j && ((positions[2 * i + 1] ^ positions[2 * i]) & 1) != 0) i++;
            a->odd_count = i;
        }
        if (lay != small_lay) free(lay);
        if (org != small_org) free(org);
    }

    /* empty runs: the rects collapse to the (transformed) origin */
    float x = a->origin_x, y = a->origin_y;
    if (a->has_transform) {
        const idwr_matrix *t = &a->transform;
        float nx = y * t->m21 + x * t->m11;
        nx = nx + t->dx;
        float ny = x * t->m12;
        ny = ny + y * t->m22;
        ny = ny + t->dy;
        x = nx;
        y = ny;
    }
    int32_t *R = a->rect;
    if (R[2] <= R[0] || R[3] <= R[1]) {
        int32_t v = rhu(x * scale) * (int32_t)(a->attributes & 0xf);
        R[0] = v; R[2] = v;
        v = rhu(y * scale) * (int32_t)(a->attributes >> 4 & 0xf);
        R[1] = v; R[3] = v;
    }
    int32_t *A = a->aliased_rect;
    if (A[2] <= A[0] || A[3] <= A[1]) {
        int32_t v = rhu(x * scale);
        A[0] = v; A[2] = v;
        v = rhu(y * scale);
        A[1] = v; A[3] = v;
    }
}

/* ---------------------------------------------------------------------------------------- */
/* bounds                                                                                     */
/* ---------------------------------------------------------------------------------------- */

static int32_t floor_div(int32_t v, int32_t d) {
    int32_t r = v % d;
    if (r < 0) v -= d;
    return (v - r) / d;
}

/* RoundUpToMultiple 0x180014844 */
static int32_t round_up_multiple(int32_t v, uint16_t d) {
    int32_t r = v % (int32_t)d;
    if (r > 0) return ((int32_t)d - r) + v;
    return v - r;
}

static void run_rect_to_pixels(uint32_t attr, int32_t *r) {
    uint32_t lx = attr >> 8 & 0xf;
    if (lx != 0) {
        r[0] >>= lx;
        r[2] = (int32_t)((uint32_t)r[2] + ((attr & 0xf) - 1)) >> lx;
        uint32_t oy = attr >> 4 & 0xf;
        if (oy > 1) {
            uint32_t ly = attr >> 12 & 0xf;
            r[1] >>= ly;
            r[3] = (int32_t)(oy + (uint32_t)r[3] - 1) >> ly;
        }
    } else {
        int32_t ox = attr & 0xf;
        r[0] = floor_div(r[0], ox);
        r[2] = round_up_multiple(r[2], (uint16_t)ox) / ox;
        int32_t oy = attr >> 4 & 0xf;
        if (oy > 1) {
            r[1] = floor_div(r[1], oy);
            r[3] = round_up_multiple(r[3], (uint16_t)oy) / oy;
        }
    }
}

int idwr_glyph_run_alpha_texture_bounds(const idwr_glyph_run_analysis *a, int type, int32_t out[4]) {
    if (type != 0 && type != 1) {
        memset(out, 0, 16);
        return IDWR_E_INVALIDARG;
    }
    uint32_t attr = a->attributes;
    if ((uint32_t)type != (attr >> 16 & 1)) {
        memset(out, 0, 16);
        return 0;
    }
    int32_t r[4];
    memcpy(r, a->rect, 16);
    if ((attr >> 16 & 1) && r[2] > r[0] && r[3] > r[1]) {
        int32_t margin = attr >> 24 & 3;
        r[0] -= margin;
        r[2] += margin;
    }
    run_rect_to_pixels(attr, r);
    const int32_t *al = a->aliased_rect;
    if (r[2] > r[0] && r[3] > r[1]) {
        if (al[2] > al[0] && al[3] > al[1]) {
            if (al[3] >= r[3]) r[3] = al[3];
            if (r[1] >= al[1]) r[1] = al[1];
            if (r[0] >= al[0]) r[0] = al[0];
            if (al[2] >= r[2]) r[2] = al[2];
        }
        memcpy(out, r, 16);
    } else {
        memcpy(out, al, 16);
    }
    return 0;
}

int idwr_glyph_run_bitmap_bounds(const idwr_glyph_run_analysis *a, int type, int32_t out[4]) {
    if (type == 0) {
        memcpy(out, a->aliased_rect, 16);
        return 0;
    }
    uint32_t attr = a->attributes;
    if (type != (int32_t)attr >> 26) {
        memset(out, 0, 16);
        return (uint32_t)type < 6 ? 0 : IDWR_E_INVALIDARG;
    }
    int32_t r[4];
    memcpy(r, a->rect, 16);
    if ((attr >> 16 & 1) && !(r[2] <= r[0] || r[3] <= r[1])) {
        int32_t margin = attr >> 24 & 3;
        r[0] -= margin;
        r[2] += margin;
    }
    run_rect_to_pixels(attr, r);
    memcpy(out, r, 16);
    return 0;
}
