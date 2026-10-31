/*
 * DWriteFontFace::GetRecommendedRenderingMode with the default rendering params: the model of
 * notes/recommended-mode.md (OpenTypeFontFaceBuilder::ReadRenderingModeRanges for the gasp
 * lists, then the size, gasp and hinting-table rules).
 */
#include <math.h>
#include "idwr_internal.h"

#define GASP_DOGRAY 2
#define GASP_SYMMETRIC_SMOOTHING 8
#define MAX_RANGES 256

/* GetGaspVariationTag */
static uint32_t gsp_tag(uint32_t n) { return IDWR_TAG('g', 's', 'p', '0' + (n < 10 ? n : 0)); }

/* the sorted ppem boundaries at which `flag` toggles; an odd lower_bound index means the flag
 * is set. Version 0 builds only the DOGRAY list. Returns the list length, 0 for no list.
 * For an instance with a valid MVAR, the first range's rangeMaxPPEM gets the 'gsp0' delta and
 * every later one the delta of 'gsp<count>' (count = the number of gasp ranges, when below 10):
 * ReadRenderingModeRanges passes the range count, not the range index, to GetGaspVariationTag.
 * The sums wrap at 16 bits. */
static int gasp_list(const idwr_font *f, const float *coords, uint16_t axes, uint16_t flag, uint16_t *out) {
    uint32_t len;
    const uint8_t *gasp = idwr_font_table(f, IDWR_TAG('g', 'a', 's', 'p'), &len);
    if (!gasp || len < 4) return 0;
    uint16_t version = idwr_u16(gasp), count = idwr_u16(gasp + 2);
    if (count == 0 || 4 + 4 * (uint32_t)count > len) return 0;
    if (version == 0 && flag != GASP_DOGRAY) return 0;
    int vary = idwr_mvar_valid(f, axes);
    int16_t d0 = vary ? idwr_mvar_delta(f, coords, axes, gsp_tag(0)) : 0;
    int16_t dn = vary && count < 10 ? idwr_mvar_delta(f, coords, axes, gsp_tag(count)) : 0;
#ifdef IDWR_NEGATIVE_CONTROL_GSPINDEX
    int16_t dk[10] = {0};   /* the delta of the range's own index: what the spec says, not DWrite */
    for (uint16_t k = 0; vary && k < 10; k++) dk[k] = idwr_mvar_delta(f, coords, axes, gsp_tag(k));
#endif
    uint16_t prev_max = (uint16_t)(idwr_u16(gasp + 4) + d0), prev_behavior = idwr_u16(gasp + 6);
    int n = 0;
    if (prev_behavior & flag) out[n++] = 0;
    out[n++] = prev_max;
    for (uint16_t i = 1; i < count && n < MAX_RANGES; i++) {
        uint16_t max_ppem = (uint16_t)(idwr_u16(gasp + 4 + 4 * i) + dn), behavior = idwr_u16(gasp + 6 + 4 * i);
#ifdef IDWR_NEGATIVE_CONTROL_GSPINDEX
        if (vary && count < 10) max_ppem = (uint16_t)(idwr_u16(gasp + 4 + 4 * i) + dk[i]);
#endif
        if (max_ppem <= prev_max) continue;  /* a record that does not grow the range is skipped */
        if ((behavior ^ prev_behavior) & flag) out[n++] = max_ppem;
        else out[n - 1] = max_ppem;
        prev_max = max_ppem;
        prev_behavior = behavior;
    }
    out[n - 1] = 0xffff;  /* the last range always reaches 0xffff */
    return n;
}

/* lower_bound of the ppem in the list, the ppem an unsigned int clamped to 0xFFFF
 * (GetRenderingModeFromFontSize 0x180073a4c, IsCompatibleGraycaleFontSize 0x18007214c): the
 * INT32_MIN that cvtss2si gives for NaN is 0x80000000 there, so it looks up 0xFFFF */
static int flag_set(const uint16_t *lst, int n, int32_t ppem) {
    int i = 0;
#ifdef IDWR_NEGATIVE_CONTROL_MODEEDGE
    while (i < n && (int32_t)lst[i] < (ppem > 0xffff ? 0xffff : ppem)) i++;  /* the earlier signed lookup */
#else
    uint32_t p = (uint32_t)ppem > 0xffff ? 0xffff : (uint32_t)ppem;
    while (i < n && lst[i] < p) i++;  /* lower_bound */
#endif
    return i & 1;
}

int idwr_gasp_flag_instance(const idwr_font *font, const float *coords, uint16_t axes, uint16_t flag, int32_t ppem) {
    uint16_t lst[MAX_RANGES];
    int n = gasp_list(font, coords, axes, flag, lst);
    return n ? flag_set(lst, n, ppem) : -1;
}

int idwr_gasp_flag(const idwr_font *font, uint16_t flag, int32_t ppem) {
    return idwr_gasp_flag_instance(font, NULL, 0, flag, ppem);
}

/* cvtss2si (round to nearest even), then +1 when that landed exactly 0.5 below x */
static int32_t round_half_up(float x) {
    int32_t r = idwr_cvtss2si(x);
    return (float)r - x == -0.5f ? r + 1 : r;
}

/* DWriteFontFace::GetRecommendedRenderingMode (0x1800735b0), params mode DEFAULT:
 *   s = (pixelsPerDip * 96 / 96) * emSize in float (a NaN pixelsPerDip * 96 takes the transform
 *       branch at 0x18007397c, which ends in a NaN s too: IsZeroWithTolerance 0x18004c348 keeps xmm0);
 *   s >= 100 (ordered) -> OUTLINE;
 *   NATURAL measuring: !(s > 0) -> NATURAL (comiss / jbe at 0x1800736d6: NaN too); ppem = cvtss2si(s),
 *       +1 when exactly 0.5 below; GetRenderingModeFromFontSize with ppem unsigned;
 *   GDI_NATURAL -> GDI_NATURAL; GDI_CLASSIC by the compatible smoothing: none -> ALIASED, ClearType ->
 *       GDI_CLASSIC, grayscale -> IsCompatibleGraycaleFontSize (0x18007214c): the gasp DOGRAY list at
 *       ppem; without one, weight >= 600 or ppem (unsigned) outside 9..17 -> GDI_CLASSIC, else ALIASED.
 *       The weight is the face's (idwr_face_weight: with STAT the wght value, FontStyleAttributes::
 *       GetWeight 0x1800e43dc, else the OS/2 weight resolved with the names' weight words, nameweight.c). */
int idwr_recommended_rendering_mode_instance(const idwr_font *f, const float *coords, uint16_t axes, float em, float ppd,
                                            int measuring, int smoothing) {
    /* the weight comes from the instance's axis values, which normalized coordinates do not keep: it is
     * known for the default instance (every coordinate 0); idwr_face_recommended_rendering_mode has it */
    int at_default = 1;
    for (uint16_t i = 0; coords && i < axes; i++) at_default &= coords[i] == 0.0f;
    int weight = at_default ? idwr_face_weight(f, NULL, 0) : -1;
    return idwr_recommended_rendering_mode_sim(f, coords, axes, em, ppd, measuring, smoothing, 0, weight);
}

/* `simulations`: the face's; the bold one (FontFace +0x36 bit 0) passes IsCompatibleGraycaleFontSize's
 * weight test before the weight is read */
int idwr_recommended_rendering_mode_sim(const idwr_font *f, const float *coords, uint16_t axes, float em, float ppd,
                                       int measuring, int smoothing, uint32_t simulations, int weight) {
    float ppd96 = ppd * 96.0f;
    float s = ppd96 / 96.0f * em;
    uint16_t lst[MAX_RANGES];
#ifdef IDWR_NEGATIVE_CONTROL_MODEEDGE
    /* the earlier model: emSize * pixelsPerDip, s <= 0 (NaN goes on to the lookup), a signed ppem */
    s = em * ppd;
    if (s >= 100.0f) return IDWR_MODE_OUTLINE;
    if (measuring == IDWR_MEASURING_NATURAL) {
        if (s <= 0.0f) return IDWR_MODE_NATURAL;
        int32_t sp = round_half_up(s);
        int n = gasp_list(f, coords, axes, GASP_SYMMETRIC_SMOOTHING, lst);
        if (n) {
            int i = 0;
            while (i < n && (int32_t)lst[i] < (sp > 0xffff ? 0xffff : sp)) i++;
            return i & 1 ? IDWR_MODE_NATURAL_SYMMETRIC : IDWR_MODE_NATURAL;
        }
        int hinted = idwr_font_has_table(f, IDWR_TAG('f', 'p', 'g', 'm')) || idwr_font_has_table(f, IDWR_TAG('p', 'r', 'e', 'p')) ||
                     idwr_font_has_table(f, IDWR_TAG('c', 'v', 't', ' '));
        if (idwr_font_face_type(f) == IDWR_FACE_CFF || !hinted) return IDWR_MODE_NATURAL_SYMMETRIC;
        return sp > 20 ? IDWR_MODE_NATURAL_SYMMETRIC : IDWR_MODE_NATURAL;
    }
#endif
    if (s >= 100.0f) return IDWR_MODE_OUTLINE;
    if (measuring == IDWR_MEASURING_NATURAL) {
        if (!(s > 0.0f)) return IDWR_MODE_NATURAL;
        uint32_t ppem = (uint32_t)round_half_up(s);
        int n = gasp_list(f, coords, axes, GASP_SYMMETRIC_SMOOTHING, lst);
        if (n) return flag_set(lst, n, (int32_t)ppem) ? IDWR_MODE_NATURAL_SYMMETRIC : IDWR_MODE_NATURAL;
        int hinted = idwr_font_has_table(f, IDWR_TAG('f', 'p', 'g', 'm')) || idwr_font_has_table(f, IDWR_TAG('p', 'r', 'e', 'p')) ||
                     idwr_font_has_table(f, IDWR_TAG('c', 'v', 't', ' '));
        /* GetRenderingModeFromFontSize 0x180073a4c without a list: CFF -> NATURAL_SYMMETRIC; then the
         * "ppem > 20" rule unless the face is neither hinted ([*face] + 0x60 bit 0x40) nor legacy East
         * Asian ([face + 0x108] + 0x14 bit 0, OpenTypeFontFaceBuilder::IsLegacyEastAsian) */
        if (idwr_font_face_type(f) == IDWR_FACE_CFF) return IDWR_MODE_NATURAL_SYMMETRIC;
#ifndef IDWR_NEGATIVE_CONTROL_MODELEGACY
        if (!hinted && !idwr_is_legacy_east_asian(f)) return IDWR_MODE_NATURAL_SYMMETRIC;
#else
        if (!hinted) return IDWR_MODE_NATURAL_SYMMETRIC;
#endif
        return ppem > 20 ? IDWR_MODE_NATURAL_SYMMETRIC : IDWR_MODE_NATURAL;
    }
    if (measuring == IDWR_MEASURING_GDI_NATURAL) return IDWR_MODE_GDI_NATURAL;
    if (smoothing == 1) return IDWR_MODE_ALIASED;
    if (smoothing == 3) return IDWR_MODE_GDI_CLASSIC;
    uint32_t ppem = (uint32_t)round_half_up(s);
    int n = gasp_list(f, coords, axes, GASP_DOGRAY, lst);
    if (n) return flag_set(lst, n, (int32_t)ppem) ? IDWR_MODE_GDI_CLASSIC : IDWR_MODE_ALIASED;
#ifdef IDWR_NEGATIVE_CONTROL_GRAYWEIGHT
    return -1;  /* the earlier model: grayscale without a DOGRAY list not modelled */
#endif
    if (simulations & IDWR_SIM_BOLD) return IDWR_MODE_GDI_CLASSIC;
    /* the face's weight (FontFace data + 0x58, 0x180024a54): ReadFontFace's (idwr_face_weight) */
    if (weight < 0) return -1;
    return weight >= 600 || ppem - 9 > 8 ? IDWR_MODE_GDI_CLASSIC : IDWR_MODE_ALIASED;
}

int idwr_recommended_rendering_mode(const idwr_font *f, float em, float ppd, int measuring, int smoothing) {
    return idwr_recommended_rendering_mode_instance(f, NULL, 0, em, ppd, measuring, smoothing);
}
