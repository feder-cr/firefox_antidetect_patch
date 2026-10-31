/*
 * Font and glyph design metrics: the model of notes/metrics.md, verified in Python by
 * scripts/metrics.py and here by reimpl/idwr_check.py.
 */
#include <stdlib.h>
#include "idwr_internal.h"
#if defined(IDWR_STANDALONE) && defined(IDWR_CFF)
#include "cffraster.h"
#endif

#define T(a, b, c, d) IDWR_TAG(a, b, c, d)

static int32_t trunc_div(int32_t a, int32_t b) {
    int32_t q = abs(a) / abs(b);
    return (a >= 0) == (b > 0) ? q : -q;
}

static int32_t clamp(int32_t v, int32_t lo, int32_t hi) { return v < lo ? lo : v > hi ? hi : v; }

/* glyf header box (xMin, yMin, xMax, yMax); zeros for an empty glyph or a font without glyf */
static void glyph_bbox(const idwr_font *f, uint16_t gid, int32_t box[4]) {
    uint32_t loca_len, glyf_len;
    const uint8_t *loca = idwr_font_table(f, T('l', 'o', 'c', 'a'), &loca_len);
    const uint8_t *glyf = idwr_font_table(f, T('g', 'l', 'y', 'f'), &glyf_len);
    box[0] = box[1] = box[2] = box[3] = 0;
    if (!loca || !glyf || !loca_len || !glyf_len || !f->head || f->head_len < 52) return;
    uint32_t a, b;
    if (idwr_i16(f->head + 50) == 1) {
        if (4 * (uint32_t)gid + 8 > loca_len) return;
        const uint8_t *p = loca + 4 * gid;
        a = (uint32_t)idwr_u16(p) << 16 | idwr_u16(p + 2);
        b = (uint32_t)idwr_u16(p + 4) << 16 | idwr_u16(p + 6);
    } else {
        if (2 * (uint32_t)gid + 4 > loca_len) return;
        a = 2u * idwr_u16(loca + 2 * gid);
        b = 2u * idwr_u16(loca + 2 * gid + 2);
    }
    if (b < a || b - a < 10 || a + 10 > glyf_len) return;
    for (int i = 0; i < 4; i++) box[i] = idwr_i16(glyf + a + 2 + 2 * i);
}

/* (advance, side bearing) from an hmtx/vmtx-style table with `count` long records */
static int long_metrics(const uint8_t *t, uint32_t len, uint16_t count, uint16_t gid, int32_t *adv, int32_t *sb) {
    if (count == 0) return 0;
    if (gid < count) {
        if (4 * (uint32_t)gid + 4 > len) return 0;
        *adv = idwr_u16(t + 4 * gid);
        *sb = idwr_i16(t + 4 * gid + 2);
        return 1;
    }
    uint32_t at = 4 * (uint32_t)count + 2 * (uint32_t)(gid - count);
    if (at + 2 > len) return 0;
    *adv = idwr_u16(t + 4 * (count - 1));
    *sb = idwr_i16(t + at);
    return 1;
}

/* CalculateTypoAscenderDescender: the vertical fallback of glyph metrics */
void idwr_typo_ascender_descender(const idwr_font *f, int32_t *asc, int32_t *desc) {
    uint32_t n;
    const uint8_t *os2 = idwr_font_table(f, T('O', 'S', '/', '2'), &n);
    if (os2 && n >= 78) {
        *asc = idwr_i16(os2 + 68);
        if (*asc < 0) {
            *asc = idwr_u16(os2 + 74);
            *desc = abs(idwr_i16(os2 + 76));
        } else {
            *desc = abs(idwr_i16(os2 + 70));
        }
    } else {
        const uint8_t *hhea = idwr_font_table(f, T('h', 'h', 'e', 'a'), &n);
        *asc = hhea && n >= 8 ? idwr_u16(hhea + 4) : 0;
        *desc = hhea && n >= 8 ? idwr_i16(hhea + 6) : 0;  /* not negated: a negative one gives 0 */
    }
    *asc = *asc < 1 ? 0 : *asc > 0x7fff ? 0x7fff : *asc;
    *desc = *desc < 1 ? 0 : *desc > 0x7fff ? 0x7fff : *desc;
}

/* OpenTypeFontMetricsBuilder::GetGlyphMetricsFromBlackBox; for a CFF face, in builds with the
 * CFF rasterizer, OpenTypeCffFontMetricsBuilder::GetGlyphMetrics (idwr/cffraster.c) */
int idwr_get_design_glyph_metrics(const idwr_font *f, uint16_t gid, idwr_glyph_metrics *out) {
#if defined(IDWR_STANDALONE) && defined(IDWR_CFF)
    if (idwr_font_is_cff(f)) return idwr_cff_design_glyph_metrics(f, NULL, 0, gid, out);
#endif
    uint32_t hhea_len, hmtx_len, vhea_len, vmtx_len;
    const uint8_t *hhea = idwr_font_table(f, T('h', 'h', 'e', 'a'), &hhea_len);
    const uint8_t *hmtx = idwr_font_table(f, T('h', 'm', 't', 'x'), &hmtx_len);
    if (!hhea || hhea_len < 36 || !hmtx) return 0;
    int32_t adv, lsb, box[4], adv_h, tsb;
    if (!long_metrics(hmtx, hmtx_len, idwr_u16(hhea + 34), gid, &adv, &lsb)) return 0;
    glyph_bbox(f, gid, box);
    const uint8_t *vhea = idwr_font_table(f, T('v', 'h', 'e', 'a'), &vhea_len);
    const uint8_t *vmtx = idwr_font_table(f, T('v', 'm', 't', 'x'), &vmtx_len);
    if (vhea && vmtx && vhea_len >= 36 && vmtx_len) {
        if (!long_metrics(vmtx, vmtx_len, idwr_u16(vhea + 34), gid, &adv_h, &tsb)) return 0;
    } else {
        int32_t a, d;
        idwr_typo_ascender_descender(f, &a, &d);
        adv_h = a + d;
        tsb = a - box[3];
    }
    out->leftSideBearing = lsb;
    out->advanceWidth = (uint32_t)adv;
    out->rightSideBearing = adv - lsb - box[2] + box[0];
    out->topSideBearing = tsb;
    out->advanceHeight = (uint32_t)adv_h;
    out->bottomSideBearing = adv_h - tsb - (box[3] - box[1]);
    out->verticalOriginY = tsb + box[3];
    idwr_design_metrics_16(out);   /* GetGlyphMetricsFromBlackBox computes in 16 bits */
    return 1;
}

/* InitializeDefaultFontMetrics: ComputeXHeightFromGlyphMetrics and
 * ComputeCapsHeightFromGlyphMetrics (character, scale / 128) */
static const uint16_t xheight_chars[][2] = {
    {0x78, 128}, {0x3c4, 128}, {0x433, 128}, {0x578, 128}, {0x5dd, 96}, {0x62f, 128}, {0x717, 128}, {0x784, 128},
    {0x930, 64}, {0xa30, 64}, {0xaa0, 64}, {0xb20, 64}, {0xbb0, 64}, {0xc10, 128}, {0xc89, 64}, {0xd30, 85},
    {0xdb0, 64}, {0xe20, 96}, {0xec0, 64}, {0xf40, 128}, {0x1010, 128}, {0x10f2, 128}, {0x1100, 64},
    {0x1210, 64}, {0x13a0, 64}, {0x14c0, 128}, {0x1681, 128}, {0x17a0, 64}, {0x1884, 128}};
static const uint16_t capheight_chars[][2] = {{0x48, 128}};

static int xh_ok(int32_t v, int32_t upem) { return upem < 10 * v && 10 * v < 9 * upem; }
static int ch_ok(int32_t v, int32_t upem) { return upem < 10 * v && v <= upem; }

/* GetYMaxFromOpenTypeFace::GetYMaxFromChar: verticalOriginY - topSideBearing of the face's
 * design metrics (the glyf box of the default instance, the varied box of an instance) */
static int glyph_ymax(const idwr_font *f, const float *coords, uint16_t axes, uint16_t g, int32_t *ymax) {
    if (!axes) {
        int32_t box[4];
        glyph_bbox(f, g, box);
        *ymax = box[3];
        return 1;
    }
    idwr_glyph_metrics m;
    if (!idwr_get_design_glyph_metrics_instance(f, coords, axes, g, &m)) return 0;
    *ymax = (int16_t)m.verticalOriginY - (int16_t)m.topSideBearing;
    return 1;
}

static int32_t glyph_height(const idwr_font *f, const float *coords, uint16_t axes, const uint16_t (*chars)[2],
                            size_t n, int32_t upem, int (*ok)(int32_t, int32_t)) {
    for (size_t i = 0; i < n; i++) {
        uint16_t g = idwr_font_glyph_index(f, chars[i][0]);
        int32_t ymax;
        if (!g || !glyph_ymax(f, coords, axes, g, &ymax)) continue;
        int32_t v = trunc_div((int32_t)chars[i][1] * ymax, 128);
        if (ok(v, upem)) return v;
    }
    return 0;
}

/* InitializeDefaultFontMetrics (no MVAR). `info` records the branch each varied field came
 * from (DefaultMetricsInfo), which decides the MVAR tags InitializeFontMetrics adds. */
void idwr_font_metrics_default(const idwr_font *f, const float *coords, uint16_t axes, idwr_font_metrics *m,
                              idwr_metrics_info *info) {
    uint32_t hhea_len, os2_len, post_len, pclt_len;
    const uint8_t *hhea = idwr_font_table(f, T('h', 'h', 'e', 'a'), &hhea_len);
    const uint8_t *os2 = idwr_font_table(f, T('O', 'S', '/', '2'), &os2_len);
    const uint8_t *post = idwr_font_table(f, T('p', 'o', 's', 't'), &post_len);
    const uint8_t *pclt = idwr_font_table(f, T('P', 'C', 'L', 'T'), &pclt_len);
    *m = (idwr_font_metrics){0};
    *info = (idwr_metrics_info){0};
    m->designUnitsPerEm = 0x800;
    if (f->head && f->head_len >= 44) {
        m->designUnitsPerEm = idwr_u16(f->head + 18);
        m->glyphBoxLeft = idwr_i16(f->head + 36);
        m->glyphBoxBottom = idwr_i16(f->head + 38);
        m->glyphBoxRight = idwr_i16(f->head + 40);
        m->glyphBoxTop = idwr_i16(f->head + 42);
    }
    int32_t upem = m->designUnitsPerEm;
    int typo = 0;
    if (os2 && os2_len >= 64) {
        info->os2 = 1;
        m->subscriptPositionX = idwr_i16(os2 + 14);
        m->subscriptPositionY = (int16_t)-idwr_i16(os2 + 16);
        m->subscriptSizeX = idwr_i16(os2 + 10);
        m->subscriptSizeY = idwr_i16(os2 + 12);
        m->superscriptPositionX = idwr_i16(os2 + 22);
        m->superscriptPositionY = idwr_i16(os2 + 24);
        m->superscriptSizeX = idwr_i16(os2 + 18);
        m->superscriptSizeY = idwr_i16(os2 + 20);
        typo = (os2[63] & 0x80) != 0;  /* fsSelection USE_TYPO_METRICS */
    }
    if (typo && os2_len >= 74) {
        int32_t a = idwr_i16(os2 + 68);
        m->ascent = (uint16_t)(a < 0 ? 0 : a);
        m->descent = (uint16_t)clamp(-idwr_i16(os2 + 70), 0, 0xffff);
        m->lineGap = idwr_i16(os2 + 72);
        m->hasTypographicMetrics = 1;
        info->asc_src = info->desc_src = info->gap_src = 3;
        info->asc_raw = a, info->desc_raw = idwr_i16(os2 + 70), info->gap_raw = idwr_i16(os2 + 72);
    } else {
        int32_t h_asc = 0, h_desc = 0, h_gap = 0, w_asc = 0, w_desc = 0;
        if (hhea && hhea_len >= 10) {
            h_asc = idwr_i16(hhea + 4);
            h_desc = abs(idwr_i16(hhea + 6));
            h_gap = idwr_i16(hhea + 8);
            if (h_gap < 0) h_gap = 0;
        }
        if (os2 && os2_len >= 78) {
            w_asc = idwr_u16(os2 + 74);
            w_desc = idwr_u16(os2 + 76);
        }
        int aat_only = (idwr_font_has_table(f, T('m', 'o', 'r', 't')) || idwr_font_has_table(f, T('m', 'o', 'r', 'x'))) &&
                       !(idwr_font_has_table(f, T('G', 'P', 'O', 'S')) || idwr_font_has_table(f, T('G', 'D', 'E', 'F')) ||
                         idwr_font_has_table(f, T('G', 'S', 'U', 'B')));
        if (!aat_only && (w_asc || w_desc)) {
            m->ascent = (uint16_t)w_asc;
            m->descent = (uint16_t)clamp(w_desc, 0, 0xffff);
            m->lineGap = (int16_t)clamp(h_gap + h_desc + h_asc - w_desc - w_asc, 0, 0x7fff);
            info->asc_src = info->desc_src = 2, info->gap_src = 6;
            info->asc_raw = w_asc, info->desc_raw = w_desc;
        } else {
            m->ascent = (uint16_t)(h_asc & 0xffff);
            m->descent = (uint16_t)clamp(h_desc, 0, 0xffff);
            m->lineGap = (int16_t)clamp(h_gap, 0, 0x7fff);
            info->asc_src = info->desc_src = info->gap_src = 1;
            info->asc_raw = h_asc, info->desc_raw = h_desc;
        }
    }
    if (post && post_len >= 12) {
        info->post = 1;
        m->underlinePosition = idwr_i16(post + 8);
        m->underlineThickness = (uint16_t)idwr_i16(post + 10);
        info->ul_src = 5;
        if (m->underlineThickness == 0) m->underlineThickness = (uint16_t)((upem + 7) / 14), info->ul_src = 6;
    } else {
        m->underlinePosition = (int16_t)trunc_div(upem + 5, -10);
        m->underlineThickness = (uint16_t)((upem + 7) / 14);
    }
    if (os2 && os2_len >= 30) {
        int32_t so = idwr_i16(os2 + 26);
        m->strikethroughThickness = so >= 1 ? (uint16_t)so : m->underlineThickness;
        m->strikethroughPosition = idwr_i16(os2 + 28);
        info->st_src = so >= 1 ? 2 : 6;
    } else {
        m->strikethroughThickness = m->underlineThickness;
        m->strikethroughPosition = (int16_t)(upem / 3);
    }
    int32_t xh = 0, ch = 0;
    if (os2 && os2_len >= 90 && idwr_u16(os2) >= 2) {
        xh = idwr_u16(os2 + 86);
        ch = idwr_u16(os2 + 88);
        int xok = xh_ok(xh, upem), cok = !(upem < ch || 10 * ch <= upem);
        if (!xok) xh = 0;
        if (!cok) ch = 0;
        if (xok && cok) {
            m->xHeight = (uint16_t)xh;
            m->capHeight = (uint16_t)ch;
            info->xh_src = info->ch_src = 2;
            return;
        }
    }
    if (pclt && pclt_len >= 18) {
        if (!xh) xh = idwr_u16(pclt + 10);
        if (!ch) ch = idwr_u16(pclt + 16);
        int xok = xh_ok(xh, upem), cok = !(upem < ch || 10 * ch <= upem);
        if (!xok) xh = 0;
        if (!cok) ch = 0;
        if (xok && cok) {
            m->xHeight = (uint16_t)xh;
            m->capHeight = (uint16_t)ch;
            info->xh_src = info->ch_src = 4;
            return;
        }
    }
    int pictorial = os2 && os2_len > 32 && os2[32] == 5;
    if (!idwr_font_is_symbol(f) && !pictorial) {
        if (!xh) xh = glyph_height(f, coords, axes, xheight_chars, sizeof xheight_chars / sizeof *xheight_chars, upem, xh_ok);
        if (!ch) ch = glyph_height(f, coords, axes, capheight_chars, 1, upem, ch_ok);
    }
    m->xHeight = (uint16_t)(xh ? xh : upem >> 1);
    m->capHeight = (uint16_t)(ch ? ch : upem * 70 / 100);
    info->xh_src = info->ch_src = 6;
}

/* InitializeDefaultFontMetrics, without MVAR */
void idwr_get_font_metrics(const idwr_font *f, idwr_font_metrics *m) {
    idwr_metrics_info info;
    idwr_font_metrics_default(f, NULL, 0, m, &info);
}
