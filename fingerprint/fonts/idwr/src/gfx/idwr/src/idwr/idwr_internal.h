#ifndef IDWR_INTERNAL_H
#define IDWR_INTERNAL_H
#include <math.h>
#include "idwr.h"
#include "../truetype/fp_exact.h"  /* no FMA contraction */

struct idwr_font {
    const uint8_t *data;
    size_t size;
    const uint8_t *dir;  /* the face's table records */
    uint16_t ntables;
    int collection;
    const uint8_t *head;
    uint32_t head_len;
    void *cff;           /* the CFF path's rasterizers for metrics (idwr/cffraster.c), or NULL */
};

static inline uint16_t idwr_u16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }
static inline int16_t idwr_i16(const uint8_t *p) { return (int16_t)idwr_u16(p); }

/* SSE's float -> int32 conversions on every CPU. A C cast of a NaN or out-of-range float is undefined,
 * and the instructions differ there: x86-64's cvtss2si / cvttss2si give INT32_MIN, aarch64's fcvtns /
 * fcvtzs saturate (and give 0 for NaN). DWrite runs the x86-64 ones. */
static inline int32_t idwr_cvtss2si(float v) {  /* round to nearest even (the default rounding mode) */
    return v >= -2147483648.0f && v < 2147483648.0f ? (int32_t)nearbyintf(v) : INT32_MIN;
}
static inline int32_t idwr_cvttss2si(float v) {  /* toward zero */
    return v >= -2147483648.0f && v < 2147483648.0f ? (int32_t)v : INT32_MIN;
}

/* SSE's addss / mulss on every CPU where the result is NaN. x86-64 returns a NaN operand quieted (the
 * first one when both are), and for an invalid operation (inf * 0, inf - inf) the "real indefinite"
 * 0xFFC00000; aarch64 generates the default NaN 0x7FC00000 there. DWrite runs the x86-64 ones, and an
 * infinite em size makes them reach its outlines. */
static inline float idwr_sse_nan(float r, float a, float b) {
    if (r == r) return r;
    union { float f; uint32_t u; } x;
#ifndef IDWR_NEGATIVE_CONTROL_SSENAN
    if (a != a) x.f = a, x.u |= 0x400000u;
    else if (b != b) x.f = b, x.u |= 0x400000u;
    else x.u = 0xffc00000u;
#else
    x.f = r;   /* the CPU's own NaN */
#endif
    return x.f;
}
static inline float idwr_mulss(float a, float b) { return idwr_sse_nan(a * b, a, b); }
static inline float idwr_addss(float a, float b) { return idwr_sse_nan(a + b, a, b); }

int idwr_font_has_table(const idwr_font *font, uint32_t tag);
/* the gasp `flag` at ppem: 1 or 0, or -1 when the face has no list for it */
int idwr_gasp_flag(const idwr_font *font, uint16_t flag, int32_t ppem);
int idwr_hvar_advance(const idwr_font *font, const float *coords, uint16_t axes, uint16_t glyph, int32_t *advance);
IDWR_API int idwr_strike_entry_kind(const idwr_font *font, int mode, float em, uint16_t glyph, int *image_format,
                                  int *index_metrics);
/* a composite strike glyph (image formats 8, 9) on its canvas grown by the bold shifts (idwr/sbit.c):
 * 1, 0 (no strike glyph), -1 (DWrite's sbit code fails: the glyph is empty), -2 (not a composite) */
int idwr_strike_composite(const idwr_font *font, int mode, float em, uint16_t glyph, uint32_t grow_x, uint32_t grow_y,
                         idwr_strike_glyph *out, uint8_t *pixels, size_t capacity);
int idwr_hvar_glyph_metrics(const idwr_font *font, const float *coords, uint16_t axes, uint16_t glyph,
                           idwr_glyph_metrics *out);
/* DWrite keeps glyph design metrics in DesignGlyphMetrics, seven 16-bit fields, and
 * FontFace::GetDesignGlyphMetrics widens them: the advances zero-extended, the bearings and the
 * vertical origin sign-extended. A negative instance advance (Roboto Flex at wdth 25, wght 100:
 * -13) comes out as 65523. */
static inline void idwr_design_metrics_16(idwr_glyph_metrics *m) {
#ifndef IDWR_NEGATIVE_CONTROL_WRAP16
    m->advanceWidth = (uint16_t)m->advanceWidth;
    m->advanceHeight = (uint16_t)m->advanceHeight;
    m->leftSideBearing = (int16_t)m->leftSideBearing;
    m->rightSideBearing = (int16_t)m->rightSideBearing;
    m->topSideBearing = (int16_t)m->topSideBearing;
    m->bottomSideBearing = (int16_t)m->bottomSideBearing;
    m->verticalOriginY = (int16_t)m->verticalOriginY;
#else
    (void)m;
#endif
}

/* DefaultMetricsInfo: where InitializeDefaultFontMetrics took each field that MVAR can vary.
 * Sources: 1 hhea, 2 OS/2 (win or table value), 3 OS/2 typo, 4 PCLT, 5 post, 6 computed. */
typedef struct {
    int os2, post;
    int asc_src, desc_src, gap_src, xh_src, ch_src, ul_src, st_src;
    int32_t asc_raw, desc_raw, gap_raw;
} idwr_metrics_info;
void idwr_font_metrics_default(const idwr_font *f, const float *coords, uint16_t axes, idwr_font_metrics *m,
                              idwr_metrics_info *info);
/* MetricVariationsTable: 1 and the store's delta for `tag` (0 when the tag has no record), or
 * 0 when MVAR is absent or not valid (IsValid) */
int idwr_mvar_valid(const idwr_font *font, uint16_t axes);
int16_t idwr_mvar_delta(const idwr_font *font, const float *coords, uint16_t axes, uint32_t tag);
/* the gasp lists of OpenTypeFontFaceBuilder::ReadRenderingModeRanges for an instance (MVAR gsp
 * tags); coords may be NULL (axes 0) */
/* idwr_glyph_run_analyze on a variable instance (normalized coordinates; axes 0 for none) */
int idwr_glyph_run_analyze_instance(const idwr_font *font, const float *coords, uint16_t axes, const idwr_glyph_run *run,
                                   idwr_glyph_run_analysis *a, float *origins);
int idwr_gasp_flag_instance(const idwr_font *font, const float *coords, uint16_t axes, uint16_t flag, int32_t ppem);
/* idwr_recommended_rendering_mode_instance on a face with simulations (IDWR_SIM_*) and its weight
 * (idwr_face_weight; -1: not known) */
int idwr_recommended_rendering_mode_sim(const idwr_font *font, const float *coords, uint16_t axes, float em, float ppd,
                                       int measuring, int smoothing, uint32_t simulations, int weight);
int idwr_face_weight(const idwr_font *font, const idwr_axis_value *values, uint32_t count);
/* idwr/nameweight.c: GetFontWeightFromFace (0x180021fc4, the OS/2 rule); a STAT table as ReadFontFace
 * tests it; FontDifferentiator's weight (0x180021114 + 0xb2, -1 where DWrite throws) */
int idwr_font_os2_weight(const idwr_font *font);
int idwr_font_has_stat(const idwr_font *font);
int idwr_font_name_weight(const idwr_font *font);
/* MacRomanCharToUnicode (idwr/fallback.c) */
uint32_t idwr_macroman_to_unicode(uint32_t c);

/* a face DWrite rasterizes with CffRasterizer: a 'CFF ' or 'CFF2' table (ResolveFontFaceType) */
int idwr_font_is_cff(const idwr_font *font);
/* CalculateTypoAscenderDescender (idwr/metrics.c): OS/2's typographic ascender and descender
 * (Windows metrics for a negative ascender), else hhea's, clamped to [0, 0x7fff] */
void idwr_typo_ascender_descender(const idwr_font *f, int32_t *asc, int32_t *desc);
/* HorizontalMetricVariationsTable::GetAdvanceDelta, VerticalMetricVariationsTable::
 * GetAdvanceHeightDelta and GetTsbDelta: the delta of `glyph` from the table `tag` (HVAR or
 * VVAR) through the DeltaSetIndexMap at offset `map_field` of its header (8 the advance map,
 * 0xc the tsb map); without that map the glyph id is the index, or the delta is 0 when
 * `map_required`. 0 without an item variation store. */
int16_t idwr_metric_variation_delta(const idwr_font *font, uint32_t tag, uint32_t map_field, int map_required,
                                   const float *coords, uint16_t axes, uint16_t glyph);

/* The commands of one glyph as an IFontRasterizer's GetOutline returns them (int32 words: 2
 * move to x y, 3 line to x y, 4 cubic x1 y1 x2 y2 x3 y3, 5 close, 6 quadratic x1 y1 x2 y2; 16.16
 * font units at ppem = unitsPerEm). The words stay valid until the next call. 0, or -1. */
typedef int (*idwr_commands_fn)(void *ctx, uint16_t glyph, const int32_t **words, uint32_t *count);
/* idwr_glyph_run_outline_points with the commands of each glyph from `commands` (the CFF path) */
int idwr_glyph_run_outline_commands(const idwr_font *font, const float *coords, uint16_t axes, int simulations,
                                   float em_size, const uint16_t *glyphs, const float *advances,
                                   const idwr_glyph_offset *offsets, uint32_t count, int sideways, int right_to_left,
                                   idwr_commands_fn commands, void *ctx, idwr_path_cmd *out, int capacity);

#endif
