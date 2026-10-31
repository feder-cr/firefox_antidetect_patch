/*
 * TrueType scaler, scaling layer (scl_* and the mth_* matrix helpers it uses), ported from
 * DWrite.dll 10.0.26100.9711: FUnits to device space, phantom points, composite offsets,
 * the transform set-up done by scl_InitializeScaling.
 *
 * Global state G (fnt_GlobalGraphicStateType) fields used here, by offset:
 *   0x020 int16 ppem (rounded max scale)     0x022 int16 point size
 *   0x0c8/0x0d0  rounding functions of the component ("child") scales at 0x110/0x120
 *   0x0d8/0x0e0/0x0e8  rounding functions of the x, y and max scales at 0x130/0x140/0x150
 *   scale record (16 bytes): +0 16.16 factor, +4 denominator, +8 numerator, +0xc shift
 *   0x160/0x164  x/y ratio to the larger scale   0x168 positive square  0x169 rotation flags
 *   0x170 x scale == y scale   0x184/0x188 x/y scale (16.16)   0x18c/0x190 unrounded scales
 *   0x195 use the 0x130 scales   0x1b4/0x1b6/0x1b8 emboldening   0x1bc..0x1bf flags
 *   0x1c0 ClearType flags   0x1d4 non-scaled (outline in FUnits) mode
 *
 * The rounding functions are stored and compared as the addresses of the originals
 * (scl_FRound, scl_SRound, scl_FixRound) because unported code compares them too; when the
 * originals are swapped, their entries jump to the ports below.
 *
 * Small (bool/short) parameters are taken as 32-bit integers and narrowed explicitly: the
 * original callers only set the low byte or word of the register or stack slot.
 */
#include "tt_state.h"
#include "orig.h"
#include <string.h>

/* Standalone mode (-DIDWR_STANDALONE): every caller is a C port, so the exports are not the
 * register-preserving thunks of the end of the file but the ports themselves, renamed here
 * to the export names (plain C functions on any ABI). */
#ifdef IDWR_STANDALONE
#define CHKOVF_port                                          tt_CHKOVF
#define mth_CountLowZeros_port                               tt_mth_CountLowZeros
#define scl_FRound_port                                      tt_scl_FRound
#define scl_SRound_port                                      tt_scl_SRound
#define scl_FixRound_port                                    tt_scl_FixRound
#define scl_Scale_port                                       tt_scl_Scale
#define scl_ScaleFromFixedFUnits_port                        tt_scl_ScaleFromFixedFUnits
#define scl_ScaleBack_port                                   tt_scl_ScaleBack
#define scl_ConvertToFixedFUnits_port                        tt_scl_ConvertToFixedFUnits
#define mth_IntelMul_port                                    tt_mth_IntelMul
#define mth_ReduceMatrix_port                                tt_mth_ReduceMatrix
#define mth_FoldPointSizeResolution_port                     tt_mth_FoldPointSizeResolution
#define adjustTrans_port                                     tt_adjustTrans
#define mth_PositiveSquare_port                              tt_mth_PositiveSquare
#define mth_90degRotationFactorForEmboldening_port           tt_mth_90degRotationFactorForEmboldening
#define mth_90degClosestRotationFactor_port                  tt_mth_90degClosestRotationFactor
#define scl_InitializeChildScaling_port                      tt_scl_InitializeChildScaling
#define scl_InitializeScaling_port                           tt_scl_InitializeScaling
#define scl_PostTransformGlyph_port                          tt_scl_PostTransformGlyph
#define scl_ScaleOldCharPoints_port                          tt_scl_ScaleOldCharPoints
#define scl_ScaleOldPhantomPoints_port                       tt_scl_ScaleOldPhantomPoints
#define scl_CopyCurrentCharPoints_port                       tt_scl_CopyCurrentCharPoints
#define scl_CopyCurrentPhantomPoints_port                    tt_scl_CopyCurrentPhantomPoints
#define scl_OriginalCharPointsToCurrentFixedFUnits_port      tt_scl_OriginalCharPointsToCurrentFixedFUnits
#define scl_OriginalPhantomPointsToCurrentFixedFUnits_port   tt_scl_OriginalPhantomPointsToCurrentFixedFUnits
#define scl_ScaleFixedCurrentCharPoints_port                 tt_scl_ScaleFixedCurrentCharPoints
#define scl_ScaleFixedCurrentPhantomPoints_port              tt_scl_ScaleFixedCurrentPhantomPoints
#define scl_ScaleBackCurrentCharPoints_port                  tt_scl_ScaleBackCurrentCharPoints
#define scl_ScaleBackCurrentPhantomPoints_port               tt_scl_ScaleBackCurrentPhantomPoints
#define scl_CalcOrigPhantomPoints_port                       tt_scl_CalcOrigPhantomPoints
#define scl_AdjustOldSideBearingPoints_port                  tt_scl_AdjustOldSideBearingPoints
#define scl_RoundCurrentSideBearingPnt_port                  tt_scl_RoundCurrentSideBearingPnt
#define scl_ShiftCurrentCharPoints_port                      tt_scl_ShiftCurrentCharPoints
#define scl_ScaleDownFromSubPixelOverscale_port              tt_scl_ScaleDownFromSubPixelOverscale
#define scl_AdjustCompatibleMetrics_port                     tt_scl_AdjustCompatibleMetrics
#define scl_CalcComponentOffset_port                         tt_scl_CalcComponentOffset
#define scl_UpdateParentElement_port                         tt_scl_UpdateParentElement
#define scl_IncrementChildElement_port                       tt_scl_IncrementChildElement
#define scl_ScaleAdvanceWidth_port                           tt_scl_ScaleAdvanceWidth
#define scl_ScaleAdvanceHeight_port                          tt_scl_ScaleAdvanceHeight
#define scl_ComputeScaling_port                              tt_scl_ComputeScaling
#endif

int32_t tt_FixMul(int32_t a, int32_t b); /* interp.c, DWRITE_FixMul */
int32_t tt_FixDiv(int32_t a, int32_t b); /* interp.c, DWRITE_FixDiv */

#define RVA_scl_FRound   0xd80a0
#define RVA_scl_SRound   0xd9490
#define RVA_scl_FixRound 0xe4b60
#define RVA_mth_FixXYMul 0x687bc
#define FN_ADDR(rva) idwr_orig(rva)

typedef int32_t (*scl_round_fn)(int32_t *s, int32_t x);

static inline uint16_t last_point(const uint8_t *e) { return (uint16_t)E_EP(e)[E_NC(e) - 1]; }
static inline int32_t abs32(int32_t x) { return x < 0 ? (int32_t)(0u - (uint32_t)x) : x; }
static inline int32_t max32(int32_t a, int32_t b) { return a > b ? a : b; }

/* ======================================================================================
 * Arithmetic helpers
 * ====================================================================================== */

/* CHKOVF: saturate a 64-bit value to int32 */
int32_t CHKOVF_port(int64_t v) {
    if (v > 0x7fffffffLL) return 0x7fffffff;
    if (v < -0x80000000LL) return INT32_MIN;
    return (int32_t)v;
}

/* CompDiv (0x18006da90) as its callers here see it: the full 64-bit quotient in RAX (the
 * zero-divisor result is 0x7fffffff or 0x80000000 as a 64-bit value). interp.c's tt_CompDiv
 * returns only 32 bits. */
static int64_t compdiv64(int32_t d, int64_t n) {
    int64_t half = d / 2;
    n = ((d < 0) == (n < 0)) ? n + half : n - half;
    if (d != 0) return n / d;
    return (int64_t)(n < 0) + 0x7fffffff;
}

/* mth_CountLowZeros (never returns for 0, as the original) */
int32_t mth_CountLowZeros_port(uint32_t x) {
    int32_t n = 0;
    while (!(x & 1)) { x >>= 1; n++; }
    return n;
}

/* ======================================================================================
 * Rounding functions stored in G+0xc8..0xe8, called with a scale record
 * ====================================================================================== */

int32_t scl_FRound_port(int32_t *s, int32_t x) {
    return (int32_t)((uint32_t)(s[1] >> 1) + (uint32_t)x * (uint32_t)s[2]) >> (s[3] & 31);
}

int32_t scl_SRound_port(int32_t *s, int32_t x) {
    uint32_t p = (uint32_t)x * (uint32_t)s[2], h = (uint32_t)(s[1] >> 1);
    if (x >= 0) return (int32_t)(h + p) / s[1];
    return (int32_t)(0u - (uint32_t)((int32_t)(h - p) / s[1]));
}

int32_t scl_FixRound_port(int32_t *s, int32_t x) { return tt_FixMul(x, s[0]); }

/* scl_Scale: dst[i] = round(src[i] * scale), with the rounding function inlined */
static void scale(const int32_t *s, void *fn, const int32_t *src, int32_t *dst, int32_t n) {
    if (n <= 0) return;
    if (fn == FN_ADDR(RVA_scl_FRound)) {
        for (int32_t i = 0; i < n; i++)
            dst[i] = (int32_t)((uint32_t)src[i] * (uint32_t)s[2] + (uint32_t)(s[1] >> 1)) >> (s[3] & 31);
    } else if (fn == FN_ADDR(RVA_scl_SRound)) {
        for (int32_t i = 0; i < n; i++) {
            int32_t v = src[i];
            uint32_t p = (uint32_t)v * (uint32_t)s[2], h = (uint32_t)(s[1] >> 1);
            dst[i] = v < 0 ? (int32_t)(0u - (uint32_t)((int32_t)(h - p) / s[1])) : (int32_t)(h + p) / s[1];
        }
    } else {
        for (int32_t i = 0; i < n; i++) dst[i] = tt_FixMul(src[i], s[0]);
    }
}

void scl_Scale_port(int32_t *s, void *fn, const int32_t *src, int32_t *dst, int32_t n) {
    scale(s, fn, src, dst, n);
}

/* scl_ScaleFromFixedFUnits: the source is in 26.6 FUnits */
static void scale_from_fixed(const int32_t *s, void *fn, const int32_t *src, int32_t *dst, int32_t n) {
    if (fn == FN_ADDR(RVA_scl_FRound) && s[2] < 0x1ff) {
        int sh = (s[3] + 6) & 31;
        for (int32_t i = 0; i < n; i++)
            dst[i] = (int32_t)((uint32_t)src[i] * (uint32_t)s[2] + (uint32_t)(s[1] >> 1)) >> sh;
    } else if (fn == FN_ADDR(RVA_scl_SRound)) {
        int32_t k = s[2] >> 6;
        for (int32_t i = 0; i < n; i++) {
            int32_t v = src[i];
            uint32_t p = (uint32_t)k * (uint32_t)v, h = (uint32_t)(s[1] >> 1);
            dst[i] = v < 0 ? (int32_t)(0u - (uint32_t)((int32_t)(h - p) / s[1])) : (int32_t)(h + p) / s[1];
        }
    } else {
        /* scl_FixRound, and scl_FRound with a numerator >= 0x1ff */
        int32_t k = s[0] >> 6;
        for (int32_t i = 0; i < n; i++) dst[i] = tt_FixMul(src[i], k);
    }
}

void scl_ScaleFromFixedFUnits_port(int32_t *s, void *fn, const int32_t *src, int32_t *dst, int32_t n) {
    scale_from_fixed(s, fn, src, dst, n);
}

/* scl_ScaleBack: device units back to 26.6 FUnits */
static void scale_back(const int32_t *s, const int32_t *src, int32_t *dst, int32_t n) {
    if (n <= 0) return;
    int32_t k = s[0] >> 6;
    for (int32_t i = 0; i < n; i++) dst[i] = tt_FixDiv(src[i], k);
}

void scl_ScaleBack_port(int32_t *s, const int32_t *src, int32_t *dst, int32_t n) { scale_back(s, src, dst, n); }

/* scl_ConvertToFixedFUnits */
static void to_fixed(const int32_t *src, int32_t *dst, int32_t n) {
    for (int32_t i = 0; i < n; i++) dst[i] = (int32_t)((uint32_t)src[i] << 6);
}

void scl_ConvertToFixedFUnits_port(const int32_t *src, int32_t *dst, int32_t n) { to_fixed(src, dst, n); }

/* ======================================================================================
 * Matrix helpers (TransMatrix: 3x3 int32, rows of 3; [0],[1] / [3],[4] the 2x2 part, 16.16)
 * ====================================================================================== */

/* mth_IntelMul: (x, y) = (x*a + y*c, x*b + y*d) with the 2x2 part divided by the scales */
static void intel_mul(int32_t n, int32_t *x, int32_t *y, const int32_t *m, int32_t sx, int32_t sy) {
    if (sx == 0 || sy == 0) {
        while (--n >= 0) { *y++ = 0; *x++ = 0; }
        return;
    }
    int32_t a = m[0], b = m[1], c = m[3], d = m[4];
    if (sx != 0x10000) { a = tt_FixDiv(a, sx); b = tt_FixDiv(b, sx); }
    if (sy != 0x10000) { c = tt_FixDiv(c, sy); d = tt_FixDiv(d, sy); }
    while (--n >= 0) {
        int32_t x0 = *x, y0 = *y;
        *x++ = (int32_t)((uint32_t)tt_FixMul(a, x0) + (uint32_t)tt_FixMul(c, y0));
        *y++ = (int32_t)((uint32_t)tt_FixMul(b, x0) + (uint32_t)tt_FixMul(d, y0));
    }
}

void mth_IntelMul_port(int32_t n, int32_t *x, int32_t *y, const int32_t *m, int32_t sx, int32_t sy) {
    intel_mul(n, x, y, m, sx, sy);
}

/* mth_ReduceMatrix: fold the perspective column into the 2x2 part */
void mth_ReduceMatrix_port(int32_t *m) {
    int32_t m8 = m[8];
    if (m[2] != 0) {
        int64_t k = m[2];
        m[0] = (int32_t)((uint32_t)m[0] - (uint32_t)compdiv64(m8, (int64_t)m[6] * k));
        m[1] = (int32_t)((uint32_t)m[1] - (uint32_t)compdiv64(m8, (int64_t)m[7] * k));
    }
    if (m[5] != 0) {
        int64_t k = m[5];
        m[3] = (int32_t)((uint32_t)m[3] - (uint32_t)compdiv64(m8, (int64_t)m[6] * k));
        m[4] = (int32_t)((uint32_t)m[4] - (uint32_t)compdiv64(m8, (int64_t)m[7] * k));
    }
    m[6] = 0;
    m[7] = 0;
    m[8] = (int32_t)((uint32_t)m8 + 2) >> 14;
}

/* mth_FoldPointSizeResolution: multiply by pointSize * resolution / 72 per axis */
void mth_FoldPointSizeResolution_port(int32_t ps, uint32_t xres, uint32_t yres, int32_t *m) {
    int32_t s = (int32_t)compdiv64(0x48, (int64_t)(int16_t)yres * ps);
    m[1] = tt_FixMul(m[1], s);
    m[4] = tt_FixMul(m[4], s);
    m[7] = tt_FixMul(m[7], s);
    s = (int32_t)compdiv64(0x48, (int64_t)(int16_t)xres * ps);
    m[0] = tt_FixMul(m[0], s);
    m[3] = tt_FixMul(m[3], s);
    m[6] = tt_FixMul(m[6], s);
}

/* adjustTrans: |v| -= trunc16(|v| * 0x51e / 65536) on the 2x2 part (about 2% smaller) */
void adjustTrans_port(int32_t *m) {
    static const int idx[4] = {0, 1, 3, 4};
    for (int k = 0; k < 4; k++) {
        int32_t v = m[idx[k]];
        int32_t a = (int32_t)(0u - (uint32_t)v);
        if (a < 0) a = v;
        uint32_t c = (uint32_t)(a >> 16) * 0x51eu & 0xffff0000u;
        m[idx[k]] = v >= 0 ? (int32_t)((uint32_t)a - c) : (int32_t)(c - (uint32_t)a);
    }
}

/* mth_PositiveSquare */
int32_t mth_PositiveSquare_port(const int32_t *m) {
    return m[0] == m[4] && m[1] == 0 && m[3] == 0 && m[4] >= 0;
}

/* mth_90degRotationFactorForEmboldening: 0..7 for the 8 axis-aligned orientations, 8 otherwise */
static uint16_t rot_emboldening(const int32_t *m) {
    int32_t a = m[0], b = m[1], c = m[3], d = m[4];
    if (c == 0 && b == 0) {
        if (a > 0) return d > 0 ? 0 : 4;
        if (a < 0) return d < 0 ? 2 : 6;
        return 8;
    }
    if (a != 0 || d != 0) return 8;
    if (c < 0) return b > 0 ? 1 : 7;
    if (c > 0) return b < 0 ? 3 : 5;
    return 8;
}

uint32_t mth_90degRotationFactorForEmboldening_port(const int32_t *m) { return rot_emboldening(m); }

/* mth_90degClosestRotationFactor, transcribed from the branch structure of the original */
static uint16_t rot_closest(const int32_t *m) {
    int32_t a = m[0], b = m[1], c = m[3], d = m[4];
    if (a <= 0) goto L_ce46;
    if (b != 0) goto L_cdc6;
    if (c <= 0) goto L_cdea;
    if (d > 0) return 0;
L_cdc6:
    if (b >= 0) goto L_cdda;
    if (c != 0) goto L_cdea;
    if (d <= 0) goto L_cdea;
    return 0;
L_cdda:
    if (b <= 0) goto L_cdea;
    if (c >= 0) goto L_cdea;
    if (d == 0) return 1;
L_cdea:
    if (b == 0) {
        if (c <= 0) return 8;
        if (d < 0) return 4;
    }
    if (b > 0) {
        if (c != 0) return 8;
        return d < 0 ? 4 : 8;
    }
    if (b >= 0) return 8;
    if (c >= 0) return 8;
    return (uint16_t)((d != 0) + 7);
L_ce46:
    if (a != 0) goto L_ce65;
    if (b <= 0) goto L_ce97;
    if (c >= 0) goto L_ced0;
    if (d <= 0) return 8;
    return 1;
L_ce65:                       /* a < 0 */
    if (b != 0) goto L_ce7e;
    if (c >= 0) goto L_cedc;
    if (d < 0) return 2;
L_ce7e:
    if (b <= 0) goto L_ceb1;
    if (c != 0) goto L_cedc;
    if (d >= 0) goto L_cedc;
    return 2;
L_ce97:                       /* a == 0, b <= 0 */
    if (b >= 0) goto L_ceca;
    if (c <= 0) goto L_cf3d;
    if (d < 0) return 3;
    if (a >= 0) return 8;
L_ceb1:
    if (b >= 0) goto L_cedc;
    if (c <= 0) goto L_cedc;
    if (d != 0) goto L_cedc;
    return 3;
L_ceca:                       /* b == 0 */
    goto L_cf33;
L_ced0:
    if (c <= 0) return 8;
    if (d > 0) return 5;
    if (a >= 0) goto L_cf33;
L_cedc:
    if (b > 0) {
        if (c <= 0) return 8;
        return (uint16_t)((d != 0 ? 3 : 0) + 5);
    }
    if (b == 0) {
        if (c >= 0) return 8;
        if (d > 0) return 6;
    }
    if (b >= 0) return 8;
    if (c != 0) return 8;
    return d > 0 ? 6 : 8;
L_cf33:
    if (b >= 0) return 8;
L_cf3d:
    if (c >= 0) return 8;
    return d < 0 ? 7 : 8;
}

uint32_t mth_90degClosestRotationFactor_port(const int32_t *m) { return rot_closest(m); }

/* ======================================================================================
 * Scale set-up
 * ====================================================================================== */

/* Reduce num/den by their common power of two, keep num*64 in range, and fill a scale record;
 * returns the rounding function. zero_den_signed: the inlined copy in scl_InitializeScaling
 * gives 0x7fffffff + (n < 0) for a zero denominator, scl_ComputeScaling gives 0x7fffffff. */
static void *compute_scaling(int32_t *s, int32_t num, int32_t den, int zero_den_signed) {
    uint32_t bits = (uint32_t)num | (uint32_t)den;
    int c = 0;
    while (!(bits & 1)) { bits >>= 1; c++; } /* never returns for 0, as the original */
    c--;
    if (c > 0) { num >>= c; den >>= c; }
    if (num < 0x2000000) num = (int32_t)((uint32_t)num << 6);
    else den >>= 6;
    int64_t n = (int64_t)num << 16, half = den / 2;
    n = ((den < 0) == (n < 0)) ? n + half : n - half;
    if (den != 0) s[0] = CHKOVF_port(n / den);
    else s[0] = zero_den_signed ? CHKOVF_port((int64_t)(n < 0) + 0x7fffffff) : 0x7fffffff;
    if (num > 0x7fff) return FN_ADDR(RVA_scl_FixRound);
    int32_t k = -1;
    if ((den & (den - 1)) == 0 && den != 0) k = mth_CountLowZeros_port((uint32_t)den);
    s[2] = num;
    s[1] = den;
    if (k < 0) return FN_ADDR(RVA_scl_SRound);
    s[3] = k;
    return FN_ADDR(RVA_scl_FRound);
}

void *scl_ComputeScaling_port(int32_t *s, int32_t num, int32_t den) { return compute_scaling(s, num, den, 0); }

/* scl_InitializeChildScaling: scales of a composite component (G+0x110/0x120) */
void scl_InitializeChildScaling_port(uint8_t *G, const int32_t *m, uint32_t upem) {
    int32_t u = (int32_t)((uint32_t)(uint16_t)upem << 16), sx, sy;
    if (G[0x1d4]) {
        sx = sy = u;
    } else {
        sx = max32(abs32(m[0]), abs32(m[1]));
        sy = max32(abs32(m[3]), abs32(m[4]));
    }
    AT(G, 0xc8, void *) = compute_scaling((int32_t *)(G + 0x110), sx, u, 0);
    AT(G, 0xd0, void *) = compute_scaling((int32_t *)(G + 0x120), sy, u, 0);
}

/* scl_InitializeScaling. Parameters as in the PDB signature:
 *   p2 bool, p3 bool (round the scales to integer ppem), m TransMatrix (modified),
 *   p5 bool (point size and resolution already in the matrix), p6 upem, p7 point size 16.16,
 *   p8/p9 x/y resolution, p10/p11 emboldening x/y, p12 emboldening in FUnits, p13,
 *   p14..p16 bools, p17/p18 emboldening offsets out, p19 bool (non-scaled), p20 ppem out. */
int32_t scl_InitializeScaling_port(uint8_t *G, uint32_t p2, uint32_t p3, int32_t *m, uint32_t p5,
                                           uint32_t p6, int32_t p7, uint32_t p8, uint32_t p9, uint32_t p10,
                                           uint32_t p11, uint32_t p12, int32_t p13, uint32_t p14, uint32_t p15,
                                           uint32_t p16, int16_t *p17, int16_t *p18, uint32_t p19, int32_t *p20) {
    uint8_t b2 = (uint8_t)p2, b3 = (uint8_t)p3, b5 = (uint8_t)p5, b19 = (uint8_t)p19;
    uint16_t xres = (uint16_t)p8, yres = (uint16_t)p9, emx = (uint16_t)p10, emy = (uint16_t)p11;
    /* the orthogonality and emboldening code below uses the matrix as passed in, before
     * the point size is folded in (the original keeps it in XMM1/XMM2 across the calls) */
    int32_t om[9];
    memcpy(om, m, sizeof om);

    if (!b5) mth_FoldPointSizeResolution_port(p7, xres, yres, m);
    if (b2 && (emx != 0 || emy != 0)) {
        int32_t r = (int32_t)compdiv64(0x48, (int64_t)p7 * (int16_t)yres);
        if ((uint16_t)(((uint32_t)r + 0x8000) >> 16) > 0x32) adjustTrans_port(m);
    }
    mth_ReduceMatrix_port(m);

    uint32_t u6 = (uint32_t)(uint16_t)p6 << 16;
    G[0x1d4] = b19;
    if (b5) *p20 = (int16_t)(((uint32_t)max32(abs32(m[3]), abs32(m[4])) + 0x8000) >> 16);
    int32_t xs;
    if (b19) {
        AT(G, 0x184, uint32_t) = u6;
        AT(G, 0x188, uint32_t) = u6;
        AT(G, 0x18c, int32_t) = max32(abs32(m[0]), abs32(m[1]));
        AT(G, 0x190, int32_t) = max32(abs32(m[3]), abs32(m[4]));
        xs = (int32_t)u6;
    } else {
        xs = max32(abs32(m[0]), abs32(m[1]));
        AT(G, 0x184, int32_t) = xs;
        AT(G, 0x18c, int32_t) = xs;
        int32_t ys = max32(abs32(m[3]), abs32(m[4]));
        AT(G, 0x188, int32_t) = ys;
        AT(G, 0x190, int32_t) = ys;
        if (b3) {
            xs = (int32_t)(((uint32_t)xs + 0x8000) & 0xffff0000u);
            AT(G, 0x184, int32_t) = xs;
            AT(G, 0x188, uint32_t) = ((uint32_t)ys + 0x8000) & 0xffff0000u;
        }
    }
    void *fx = compute_scaling((int32_t *)(G + 0x130), xs, (int32_t)u6, 1);
    AT(G, 0xd8, void *) = fx;
    void *fy = compute_scaling((int32_t *)(G + 0x140), AT(G, 0x188, int32_t), (int32_t)u6, 1);
    AT(G, 0xe0, void *) = fy;

    int32_t X = AT(G, 0x184, int32_t);
    if (X == 0) return 0x1901;
    int32_t Y = AT(G, 0x188, int32_t);
    if (Y == 0) return 0x1901;
    int32_t big, r164;
    if (X < Y) {
        AT(G, 0xe8, void *) = fy;
        memcpy(G + 0x150, G + 0x140, 16);
        AT(G, 0x160, int32_t) = tt_FixDiv(X, Y);
        big = Y;
        r164 = 0x10000;
    } else {
        AT(G, 0xe8, void *) = AT(G, 0xd8, void *);
        AT(G, 0x160, int32_t) = 0x10000;
        memcpy(G + 0x150, G + 0x130, 16);
        r164 = tt_FixDiv(Y, X);
        big = X;
    }
    AT(G, 0x164, int32_t) = r164;
    if (!b5) *p20 = (int16_t)(((uint32_t)Y + 0x8000) >> 16);
    G[0x170] = AT(G, 0x184, int32_t) == Y;
    AT(G, 0x20, int16_t) = (int16_t)(((uint32_t)big + 0x8000) >> 16);
    AT(G, 0x22, int16_t) = (int16_t)(((uint32_t)p7 + 0x8000) >> 16);
    G[0x168] = m[0] == m[4] && m[1] == 0 && m[3] == 0 && m[4] >= 0;

    /* rotation / skew classification of the original 2x2 part */
    int32_t a = om[0], b = om[1], c = om[3], d = om[4];
    uint8_t rotated, skewed;
    if ((int32_t)((uint32_t)tt_FixMul(a, c) + (uint32_t)tt_FixMul(b, d)) == 0) {
        rotated = !((a == 0 && d == 0) || (b == 0 && c == 0));
        int64_t bb = (int64_t)b * b, aa = (int64_t)a * a, dd = (int64_t)d * d, cc = (int64_t)c * c;
        uint32_t s1 = (uint32_t)CHKOVF_port((bb + 0x8000) >> 16) + (uint32_t)CHKOVF_port((aa + 0x8000) >> 16);
        uint32_t s2 = (uint32_t)CHKOVF_port((dd + 0x8000) >> 16) + (uint32_t)CHKOVF_port((cc + 0x8000) >> 16);
        skewed = s1 != s2;
    } else if (a != 0 && b != 0) {
        rotated = 1;
        skewed = 1;
    } else {
        rotated = 0;
        skewed = !((int32_t)((uint32_t)tt_FixMul(b, b) + (uint32_t)tt_FixMul(a, a)) == 0x10000 &&
                   (int32_t)((uint32_t)tt_FixMul(a, d) - (uint32_t)tt_FixMul(b, c)) == 0x10000);
    }
    G[0x169] = (uint8_t)(rotated | (skewed ? 2 : 0));

    *p17 = 0;
    *p18 = 0;
    G[0x1bc] = (uint8_t)p14;
    G[0x1bd] = b2 ? 2 : (uint8_t)p15 != 0;
    G[0x1bf] = (uint8_t)p16;
    G[0x1be] = b5;
    if (emx == 0 && emy == 0) {
        AT(G, 0x1b4, uint64_t) = 0;
        return 0;
    }

    /* emboldening in pixels: strength (per mille of the ppem) - 10 / 1000 */
    uint32_t ppem;
    if (!b5) ppem = (uint32_t)((int32_t)((uint32_t)compdiv64(0x48, (int64_t)p7 * (int16_t)yres) + 0x8000) >> 16);
    else ppem = *(uint16_t *)p20;
    uint16_t rot = rot_emboldening(m);
    if (rot == 8) rot = rot_closest(m);
    if (b19) {
        *p18 = (int16_t)((int32_t)((uint32_t)(uint16_t)ppem * emy - 10) / 1000);
        ppem = (uint16_t)p6;
    }
    uint32_t pp = (uint16_t)ppem;
    int16_t qy = (int16_t)((int32_t)((uint32_t)emy * pp - 10) / 1000);
    int16_t qx = (int16_t)((int32_t)((uint32_t)emx * pp - 10) / 1000);
    AT(G, 0x1b4, int16_t) = qy;
    AT(G, 0x1b6, int16_t) = (int16_t)(qx + 1);
    if (!b19) *p18 = qy;
    int16_t e = *p18;
    switch (rot) {
    case 0: *p17 = (int16_t)(e + 1); *p18 = (int16_t)-e; break;
    case 1: *p17 = (int16_t)-e; *p18 = (int16_t)~e; break;
    case 2: *p17 = (int16_t)~e; break;
    case 3: *p17 = e; *p18 = (int16_t)(e + 1); break;
    case 4: *p17 = (int16_t)(e + 1); break;
    case 5: *p17 = e; *p18 = (int16_t)~e; break;
    case 6: *p17 = (int16_t)~e; *p18 = (int16_t)-e; break;
    case 7: *p17 = (int16_t)-e; *p18 = (int16_t)(e + 1); break;
    default: *p17 = 0; *p18 = 0; break;
    }

    if (!b19) {
        if (yres != xres) {
            /* non-square resolution: map the emboldening through the inverse matrix */
            int32_t A = (int32_t)((uint32_t)AT(G, 0x1b6, uint16_t) << 16);
            int32_t B = (int32_t)((uint32_t)AT(G, 0x1b4, uint16_t) << 16);
            int32_t det = abs32((int32_t)((uint32_t)tt_FixMul(a, d) - (uint32_t)tt_FixMul(b, c)));
            if (det == 0) {
                AT(G, 0x1b4, int32_t) = 0;
            } else {
                int32_t i0 = tt_FixDiv(a, det), i1 = tt_FixDiv(b, det), i3 = tt_FixDiv(c, det), i4 = tt_FixDiv(d, det);
                int32_t m1[9] = {i0, i1, om[2], i3, i4, om[5], om[6], om[7], om[8]};
                int32_t m2[9] = {i0, (int32_t)(0u - (uint32_t)i1), om[2], (int32_t)(0u - (uint32_t)i3), i4,
                                 om[5], om[6], om[7], om[8]};
                intel_mul(1, &A, &B, m1, 0x10000, 0x10000);
                A = (int32_t)compdiv64((int16_t)yres, (int64_t)A * (int16_t)xres);
                intel_mul(1, &A, &B, m2, 0x10000, 0x10000);
                AT(G, 0x1b6, int16_t) = (int16_t)(((uint32_t)abs32(A) + 0x8000) >> 16);
                AT(G, 0x1b4, int16_t) = (int16_t)(((uint32_t)abs32(B) + 0x8000) >> 16);
            }
        }
        if (!(G[0x169] & 1)) {
            AT(G, 0x1b8, int32_t) = (int32_t)((0u - (uint32_t)p13) << 6);
            return 0;
        }
    }
    int32_t v = (int16_t)p12;
    scale((int32_t *)(G + 0x140), AT(G, 0xe0, void *), &v, (int32_t *)(G + 0x1b8), 1);
    AT(G, 0x1b8, uint32_t) &= 0xffffffc0u;
    return 0;
}

/* ======================================================================================
 * Points (E = fnt_ElementType; the 8 phantom points follow the last outline point)
 * ====================================================================================== */

/* scl_PostTransformGlyph */
void scl_PostTransformGlyph_port(uint8_t *G, uint8_t *E, const int32_t *m) {
    int32_t sx, sy;
    if (G[0x1d4]) { sx = AT(G, 0x184, int32_t); sy = AT(G, 0x188, int32_t); }
    else { sx = AT(G, 0x18c, int32_t); sy = AT(G, 0x190, int32_t); }
    intel_mul((uint16_t)(last_point(E) + 9), E_X(E), E_Y(E), m, sx, sy);
}

/* scl_ScaleOldCharPoints: original (FUnit) coordinates to scaled ones */
void scl_ScaleOldCharPoints_port(uint8_t *E, uint8_t *G) {
    int32_t n = (uint16_t)(last_point(E) + 1);
    if (!G[0x195]) {
        scale((int32_t *)(G + 0x110), AT(G, 0xc8, void *), E_OOX(E), E_OX(E), n);
        scale((int32_t *)(G + 0x120), AT(G, 0xd0, void *), E_OOY(E), E_OY(E), (uint16_t)(last_point(E) + 1));
    } else {
        scale((int32_t *)(G + 0x130), AT(G, 0xd8, void *), E_OOX(E), E_OX(E), n);
        scale((int32_t *)(G + 0x140), AT(G, 0xe0, void *), E_OOY(E), E_OY(E), (uint16_t)(last_point(E) + 1));
    }
}

/* scl_ScaleOldPhantomPoints */
void scl_ScaleOldPhantomPoints_port(uint8_t *E, uint8_t *G) {
    uint32_t i = (uint16_t)(last_point(E) + 1);
    if (!G[0x195]) {
        scale((int32_t *)(G + 0x110), AT(G, 0xc8, void *), E_OOX(E) + i, E_OX(E) + i, 8);
        scale((int32_t *)(G + 0x120), AT(G, 0xd0, void *), E_OOY(E) + i, E_OY(E) + i, 8);
    } else {
        scale((int32_t *)(G + 0x130), AT(G, 0xd8, void *), E_OOX(E) + i, E_OX(E) + i, 8);
        scale((int32_t *)(G + 0x140), AT(G, 0xe0, void *), E_OOY(E) + i, E_OY(E) + i, 8);
    }
}

/* scl_CopyCurrentCharPoints */
void scl_CopyCurrentCharPoints_port(uint8_t *E) {
    memcpy(E_X(E), E_OX(E), (size_t)(uint16_t)(last_point(E) + 1) * 4);
    memcpy(E_Y(E), E_OY(E), (size_t)(uint16_t)(last_point(E) + 1) * 4);
}

/* scl_CopyCurrentPhantomPoints */
void scl_CopyCurrentPhantomPoints_port(uint8_t *E) {
    uint32_t i = (uint16_t)(last_point(E) + 1);
    memcpy(E_X(E) + i, E_OX(E) + i, 32);
    memcpy(E_Y(E) + i, E_OY(E) + i, 32);
}

/* scl_OriginalCharPointsToCurrentFixedFUnits */
void scl_OriginalCharPointsToCurrentFixedFUnits_port(uint8_t *E) {
    to_fixed(E_OOX(E), E_X(E), (uint16_t)(last_point(E) + 1));
    to_fixed(E_OOY(E), E_Y(E), (uint16_t)(last_point(E) + 1));
}

/* scl_OriginalPhantomPointsToCurrentFixedFUnits */
void scl_OriginalPhantomPointsToCurrentFixedFUnits_port(uint8_t *E) {
    uint32_t i = (uint16_t)(last_point(E) + 1);
    to_fixed(E_OOX(E) + i, E_X(E) + i, 8);
    to_fixed(E_OOY(E) + i, E_Y(E) + i, 8);
}

/* scl_ScaleFixedCurrentCharPoints: 26.6 FUnits in x/y to device units, in place */
void scl_ScaleFixedCurrentCharPoints_port(uint8_t *E, uint8_t *G) {
    int sel = G[0x195] != 0;
    scale_from_fixed((int32_t *)(G + (sel ? 0x130 : 0x110)), AT(G, sel ? 0xd8 : 0xc8, void *), E_X(E), E_X(E),
                     (uint16_t)(last_point(E) + 1));
    scale_from_fixed((int32_t *)(G + (sel ? 0x140 : 0x120)), AT(G, sel ? 0xe0 : 0xd0, void *), E_Y(E), E_Y(E),
                     (uint16_t)(last_point(E) + 1));
}

/* scl_ScaleFixedCurrentPhantomPoints (always the 0x130/0x140 scales) */
void scl_ScaleFixedCurrentPhantomPoints_port(uint8_t *E, uint8_t *G) {
    uint32_t i = (uint16_t)(last_point(E) + 1);
    scale_from_fixed((int32_t *)(G + 0x130), AT(G, 0xd8, void *), E_X(E) + i, E_X(E) + i, 8);
    scale_from_fixed((int32_t *)(G + 0x140), AT(G, 0xe0, void *), E_Y(E) + i, E_Y(E) + i, 8);
}

/* scl_ScaleBackCurrentCharPoints */
void scl_ScaleBackCurrentCharPoints_port(uint8_t *E, uint8_t *G) {
    int sel = G[0x195] != 0;
    scale_back((int32_t *)(G + (sel ? 0x130 : 0x110)), E_X(E), E_X(E), (uint16_t)(last_point(E) + 1));
    scale_back((int32_t *)(G + (sel ? 0x140 : 0x120)), E_Y(E), E_Y(E), (uint16_t)(last_point(E) + 1));
}

/* scl_ScaleBackCurrentPhantomPoints */
void scl_ScaleBackCurrentPhantomPoints_port(uint8_t *E, uint8_t *G) {
    int sel = G[0x195] != 0;
    uint32_t i = (uint16_t)(last_point(E) + 1);
    scale_back((int32_t *)(G + (sel ? 0x130 : 0x110)), E_X(E) + i, E_X(E) + i, 8);
    scale_back((int32_t *)(G + (sel ? 0x140 : 0x120)), E_Y(E) + i, E_Y(E) + i, 8);
}

/* scl_CalcOrigPhantomPoints: bbox = {xMin, yMin, xMax, yMax} */
void scl_CalcOrigPhantomPoints_port(uint8_t *E, const int16_t *bbox, uint32_t lsb, uint32_t tsb,
                                            uint32_t aw, uint32_t ah, uint32_t p7) {
    int32_t *oox = E_OOX(E), *ooy = E_OOY(E);
    uint16_t n = last_point(E);
    memset(ooy + (uint16_t)(n + 1), 0, 32);
    memset(oox + (uint16_t)(n + 1), 0, 32);
    int32_t r = (int32_t)((uint32_t)bbox[0] - (uint32_t)(int16_t)lsb);
    oox[(uint16_t)(n + 1)] = r;
    oox[(uint16_t)(n + 2)] = (int32_t)((uint32_t)(uint16_t)aw + (uint32_t)r);
    oox[(uint16_t)(n + 5)] = r;
    oox[(uint16_t)(n + 6)] = bbox[0];
    r = (int32_t)((uint32_t)bbox[3] + (uint32_t)(int16_t)tsb);
    ooy[(uint16_t)(n + 3)] = r;
    ooy[(uint16_t)(n + 4)] = (int32_t)((uint32_t)r - (uint16_t)ah);
    ooy[(uint16_t)(n + 7)] = r;
    ooy[(uint16_t)(n + 8)] = bbox[3];
    for (int i = 0; i < 2; i++) oox[(uint16_t)(i + 3 + n)] = (int16_t)p7;
    for (int i = 0; i < 2; i++) oox[(uint16_t)(i + 7 + n)] = (int16_t)p7;
}

/* round to a pixel, or to a quarter pixel in ClearType x (G+0x1c0 & 5 == 1) */
static inline int32_t round_x(const uint8_t *G, int32_t v) {
    if ((G[0x1c0] & 5) == 1) return (int32_t)(((uint32_t)v + 2) & 0xfffffffcu);
    return (int32_t)(((uint32_t)v + 0x20) & 0xffffffc0u);
}

/* scl_AdjustOldSideBearingPoints */
void scl_AdjustOldSideBearingPoints_port(uint8_t *E, uint8_t *G) {
    int32_t *ox = E_OX(E);
    uint32_t i = (uint16_t)(last_point(E) + 1);
    int32_t v = ox[i], r = round_x(G, v);
    ox[i] = r;
    ox[(uint16_t)(last_point(E) + 2)] += (int32_t)((uint32_t)r - (uint32_t)v);
}

/* scl_RoundCurrentSideBearingPnt: place the advance phantom points at the rounded scaled
 * advance from the rounded side bearing points */
void scl_RoundCurrentSideBearingPnt_port(uint8_t *E, uint8_t *G, uint32_t upem) {
    int64_t u = (int16_t)upem;
    int64_t half = (int32_t)u / 2;
    uint16_t n = last_point(E);
    uint16_t i2 = (uint16_t)(n + 2), i1 = (uint16_t)(n + 1);
    int32_t *oox = E_OOX(E);
    int16_t d = (int16_t)((uint16_t)oox[i2] - (uint16_t)oox[i1]);
    int64_t p = (int64_t)d * AT(G, 0x184, int32_t), q;
    if (u < 0) q = (p - half) / u;
    else { p += half; q = u == 0 ? (int64_t)(p < 0) + 0x7fffffff : p / u; }
    int32_t r = round_x(G, (int32_t)((uint32_t)q + 0x200) >> 10);
    E_X(E)[i2] = (int32_t)((uint32_t)r + (uint32_t)E_X(E)[i1]);

    n = last_point(E);
    uint16_t i3 = (uint16_t)(n + 3), i4 = (uint16_t)(n + 4);
    int32_t *ooy = E_OOY(E);
    d = (int16_t)((uint16_t)ooy[i4] - (uint16_t)ooy[i3]);
    p = (int64_t)d * AT(G, 0x188, int32_t);
    if (u < 0) q = (p - half) / u;
    else { p += half; q = u == 0 ? (int64_t)(p < 0) + 0x7fffffff : p / u; }
    int32_t *y = E_Y(E);
    y[i3] = (int32_t)(((uint32_t)y[i3] + 0x20) & 0xffffffc0u);
    n = last_point(E);
    uint32_t t = (uint32_t)((int32_t)((uint32_t)q + 0x200) >> 10) + (uint32_t)y[(uint16_t)(n + 3)] + 0x20;
    y[(uint16_t)(n + 4)] = (int32_t)(t & 0xffffffc0u);
}

/* scl_ShiftCurrentCharPoints */
void scl_ShiftCurrentCharPoints_port(uint8_t *E, int32_t dx, int32_t dy) {
    if (dx != 0)
        for (uint32_t i = 0; i < (uint16_t)(last_point(E) + 1); i++) E_X(E)[i] = (int32_t)((uint32_t)E_X(E)[i] + (uint32_t)dx);
    if (dy != 0)
        for (uint32_t i = 0; i < (uint16_t)(last_point(E) + 1); i++) E_Y(E)[i] = (int32_t)((uint32_t)E_Y(E)[i] + (uint32_t)dy);
}

/* scl_ScaleDownFromSubPixelOverscale: x, y (overscaled) to ox, oy */
void scl_ScaleDownFromSubPixelOverscale_port(uint8_t *E, uint32_t osx, uint32_t osy) {
    int32_t dx = (uint16_t)osx, dy = (uint16_t)osy;
    if ((uint16_t)(last_point(E) + 9) == 0) return;
    int32_t i = 0;
    do {
        E_OX(E)[i] = (int32_t)((uint32_t)E_X(E)[i] + ((uint32_t)dx >> 1)) / dx;
        E_OY(E)[i] = (int32_t)((uint32_t)E_Y(E)[i] + ((uint32_t)dy >> 1)) / dy;
        i++;
    } while (i < (int32_t)(uint16_t)(last_point(E) + 9));
}

/* scl_AdjustCompatibleMetrics (p2 unused by the original) */
void scl_AdjustCompatibleMetrics_port(uint8_t *E, int32_t p2, int32_t dw) {
    (void)p2;
    uint16_t n = last_point(E);
    E_X(E)[(uint16_t)(n + 2)] = (int32_t)((uint32_t)E_X(E)[(uint16_t)(n + 1)] + (uint32_t)dw);
}

/* ======================================================================================
 * Composites
 * ====================================================================================== */

/* scl_CalcComponentOffset: scaled (and optionally rounded) offset of a component.
 * parity: RotationParity (0, 1, 2) */
void scl_CalcComponentOffset_port(uint8_t *G, uint32_t ax, uint32_t ay, uint32_t round, uint32_t parent_scale,
                                          uint32_t scaled_offset, const int32_t *m, int32_t parity, int32_t *ox,
                                          int32_t *oy) {
    if ((uint8_t)parent_scale) {
        *ox = ((scl_round_fn)AT(G, 0xd8, void *))((int32_t *)(G + 0x130), (int16_t)ax);
        *oy = ((scl_round_fn)AT(G, 0xe0, void *))((int32_t *)(G + 0x140), (int16_t)ay);
    } else {
        *ox = ((scl_round_fn)AT(G, 0xc8, void *))((int32_t *)(G + 0x110), (int16_t)ax);
        *oy = ((scl_round_fn)AT(G, 0xd0, void *))((int32_t *)(G + 0x120), (int16_t)ay);
    }
    if ((uint8_t)scaled_offset) {
        int32_t sx = max32(abs32(m[0]), abs32(m[1])), sy = max32(abs32(m[3]), abs32(m[4]));
        if (sx != 0x10000 || sy != 0x10000) {
            *ox = tt_FixMul(*ox, sx);
            *oy = tt_FixMul(*oy, sy);
        }
    }
    if ((uint8_t)round) {
        uint16_t f = AT(G, 0x1c0, uint16_t);
        uint32_t z = parity == 0, f4 = f & 4;
        /* ClearType: quarter-pixel rounding along the subpixel direction */
        if ((f & 1) && (parity == 2 || z != f4)) *ox = (int32_t)(((uint32_t)*ox + 2) & 0xfffffffcu);
        else *ox = (int32_t)(((uint32_t)*ox + 0x20) & 0xffffffc0u);
        f = AT(G, 0x1c0, uint16_t);
        f4 = f & 4;
        if ((f & 1) && (parity == 2 || z == f4)) *oy = (int32_t)(((uint32_t)*oy + 2) & 0xfffffffcu);
        else *oy = (int32_t)(((uint32_t)*oy + 0x20) & 0xffffffc0u);
    }
    if (!(uint8_t)parent_scale) {
        scale_back((int32_t *)(G + 0x110), ox, ox, 1);
        scale_back((int32_t *)(G + 0x120), oy, oy, 1);
    }
}

/* scl_UpdateParentElement: append the child's contours to the parent */
int32_t scl_UpdateParentElement_port(const uint8_t *C, uint8_t *P, const uint8_t *maxp) {
    int16_t pn = E_NC(P);
    uint16_t cn = (uint16_t)E_NC(C);
    int16_t sum = (int16_t)(cn + pn);
    if (sum < pn || sum < 0) return 0x1403;
    uint16_t lim = AT(maxp, 0xc, uint16_t);
    if (AT(maxp, 8, uint16_t) >= lim) lim = AT(maxp, 8, uint16_t);
    if ((int32_t)sum > (int32_t)lim) return 0x1403;
    if (pn != 0) {
        int16_t *ep = E_EP(P);
        uint16_t off = (uint16_t)(ep[pn - 1] + 1);
        int16_t t = (int16_t)(off + (uint16_t)ep[sum - 1]);
        if (t < (int16_t)off || t < 0) return 0x1401;
        if (pn < sum)
            for (uint32_t k = 0; k < cn; k++) {
                E_SP(P)[pn + k] = (int16_t)(E_SP(P)[pn + k] + off);
                E_EP(P)[pn + k] = (int16_t)(E_EP(P)[pn + k] + off);
            }
    }
    E_NC(P) = sum;
    return 0;
}

/* scl_IncrementChildElement: point the child's arrays past the parent's points */
void scl_IncrementChildElement_port(uint8_t *C, const uint8_t *P) {
    if (E_NC(P) == 0) {
        memcpy(C, P, 0x70);
        return;
    }
    uint64_t off = (uint16_t)(last_point(P) + 1);
    for (int k = 0; k < 6; k++) AT(C, 8 * k, uint8_t *) = AT(P, 8 * k, uint8_t *) + off * 4;
    AT(C, 0x30, uint8_t *) = AT(P, 0x30, uint8_t *) + off;
    AT(C, 0x48, uint8_t *) = AT(P, 0x48, uint8_t *) + off;
    AT(C, 0x58, int64_t) = (int64_t)E_NC(P) + AT(P, 0x58, int64_t);
    AT(C, 0x68, uint8_t *) = AT(P, 0x68, uint8_t *) + off * 12;
    AT(C, 0x38, uint8_t *) = AT(P, 0x38, uint8_t *) + (int64_t)E_NC(P) * 2;
    int16_t nc = E_NC(P);
    uint8_t *ep = AT(P, 0x40, uint8_t *);
    AT(C, 0x50, int16_t) = 0;
    AT(C, 0x40, uint8_t *) = ep + (int64_t)nc * 2;
}

/* ======================================================================================
 * Advances
 * ====================================================================================== */

/* scl_ScaleAdvanceWidth: v = {x, y} 16.16; linear: scaled in the device x scale only */
void scl_ScaleAdvanceWidth_port(uint8_t *G, int32_t *v, uint32_t aw, uint32_t linear, uint32_t upem,
                                        const int32_t *m) {
    int64_t w = (int16_t)aw;
    int32_t u = (int16_t)upem;
    if ((uint8_t)linear) {
        int64_t p = (int64_t)AT(G, 0x18c, int32_t) * w, half = u / 2, q;
        if (u < 0) q = (p - half) / u;
        else { p += half; q = u == 0 ? (int64_t)(p < 0) + 0x7fffffff : p / u; }
        v[0] = (int32_t)q;
        if ((int32_t)q != 0 && AT(G, 0x1b6, int16_t) != 0) v[0] = (int32_t)((uint32_t)q + 0x10000);
        return;
    }
    /* the 0x1b6 (emboldening) extra is added in device space, so the advance goes through it */
    int32_t r = CHKOVF_port(compdiv64(u, w << 16));
    v[0] = r;
    int32_t sx = AT(G, 0x18c, int32_t);
    if (sx == 0x10000 || AT(G, 0x1b6, int16_t) == 0) {
        if (r != 0 && AT(G, 0x1b6, int16_t) != 0) v[0] = (int32_t)((uint32_t)r + 0x10000);
    } else {
        r = tt_FixMul(r, sx);
        v[0] = r;
        if (r != 0 && AT(G, 0x1b6, int16_t) != 0) { r = (int32_t)((uint32_t)r + 0x10000); v[0] = r; }
        v[0] = tt_FixDiv(r, AT(G, 0x18c, int32_t));
    }
    ORIG(void (*)(int32_t *, int32_t *, const int32_t *), RVA_mth_FixXYMul)(v, v + 1, m);
}

/* scl_ScaleAdvanceHeight */
void scl_ScaleAdvanceHeight_port(uint8_t *G, int32_t *v, uint32_t ah, uint32_t linear, uint32_t upem,
                                         const int32_t *m) {
    int64_t h = (int16_t)ah;
    int32_t u = (int16_t)upem;
    if ((uint8_t)linear) {
        int64_t p = (int64_t)AT(G, 0x190, int32_t) * h, half = u / 2, q;
        if (u < 0) q = (p - half) / u;
        else { p += half; q = u == 0 ? (int64_t)(p < 0) + 0x7fffffff : p / u; }
        v[1] = (int32_t)q;
        if ((int32_t)q != 0) v[1] = (int32_t)((uint32_t)q + 0x10000);
        return;
    }
    int32_t r = CHKOVF_port(compdiv64(u, h << 16));
    v[1] = r;
    if (r != 0) v[1] = (int32_t)((uint32_t)r + 0x10000);
    ORIG(void (*)(int32_t *, int32_t *, const int32_t *), RVA_mth_FixXYMul)(v, v + 1, m);
}

/* ======================================================================================
 * Exports for the function swap (reimpl/oracle.py --swap)
 *
 * DWrite.dll is built with whole-program optimization: its callers keep values in volatile
 * registers across calls to these functions when the callee is known not to touch them
 * (scl_InitializeScaling keeps R9 across mth_90degRotationFactorForEmboldening and the
 * original matrix in XMM1/XMM2 across mth_ReduceMatrix and friends; the scaling loops keep
 * RDX/R8-R10 across CHKOVF). So each tt_* export is a thunk that preserves RCX, RDX, R8-R11
 * and XMM0-XMM5, re-pushes up to 16 stack arguments and calls the port with the original
 * Windows x64 signature. Only RAX (the result) changes.
 *
 * Standalone mode: no thunks, the ports are the exports (the renames at the top of the
 * file); SCL_THUNK only checks that each rename is there.
 * ====================================================================================== */
#ifdef IDWR_STANDALONE
#define SCL_THUNK(name, impl) _Static_assert(sizeof(&name) == sizeof(&impl), #name);
#else
#define SCL_THUNK(name, impl) \
    __attribute__((naked)) TT_EXPORT void name(void) { \
        __asm__("pushq %rbp\n\t" "movq %rsp, %rbp\n\t" "subq $0x140, %rsp\n\t" \
                "movq %rcx, 0xa0(%rsp)\n\t" "movq %rdx, 0xa8(%rsp)\n\t" "movq %r8, 0xb0(%rsp)\n\t" \
                "movq %r9, 0xb8(%rsp)\n\t" "movq %r10, 0xc0(%rsp)\n\t" "movq %r11, 0xc8(%rsp)\n\t" \
                "movdqu %xmm0, 0xd0(%rsp)\n\t" "movdqu %xmm1, 0xe0(%rsp)\n\t" "movdqu %xmm2, 0xf0(%rsp)\n\t" \
                "movdqu %xmm3, 0x100(%rsp)\n\t" "movdqu %xmm4, 0x110(%rsp)\n\t" "movdqu %xmm5, 0x120(%rsp)\n\t" \
                "movq 48(%rbp), %rax\n\t" "movq %rax, 32(%rsp)\n\t" \
                "movq 56(%rbp), %rax\n\t" "movq %rax, 40(%rsp)\n\t" \
                "movq 64(%rbp), %rax\n\t" "movq %rax, 48(%rsp)\n\t" \
                "movq 72(%rbp), %rax\n\t" "movq %rax, 56(%rsp)\n\t" \
                "movq 80(%rbp), %rax\n\t" "movq %rax, 64(%rsp)\n\t" \
                "movq 88(%rbp), %rax\n\t" "movq %rax, 72(%rsp)\n\t" \
                "movq 96(%rbp), %rax\n\t" "movq %rax, 80(%rsp)\n\t" \
                "movq 104(%rbp), %rax\n\t" "movq %rax, 88(%rsp)\n\t" \
                "movq 112(%rbp), %rax\n\t" "movq %rax, 96(%rsp)\n\t" \
                "movq 120(%rbp), %rax\n\t" "movq %rax, 104(%rsp)\n\t" \
                "movq 128(%rbp), %rax\n\t" "movq %rax, 112(%rsp)\n\t" \
                "movq 136(%rbp), %rax\n\t" "movq %rax, 120(%rsp)\n\t" \
                "movq 144(%rbp), %rax\n\t" "movq %rax, 128(%rsp)\n\t" \
                "movq 152(%rbp), %rax\n\t" "movq %rax, 136(%rsp)\n\t" \
                "movq 160(%rbp), %rax\n\t" "movq %rax, 144(%rsp)\n\t" \
                "movq 168(%rbp), %rax\n\t" "movq %rax, 152(%rsp)\n\t" \
                "call " #impl "\n\t" \
                "movq 0xa0(%rsp), %rcx\n\t" "movq 0xa8(%rsp), %rdx\n\t" "movq 0xb0(%rsp), %r8\n\t" \
                "movq 0xb8(%rsp), %r9\n\t" "movq 0xc0(%rsp), %r10\n\t" "movq 0xc8(%rsp), %r11\n\t" \
                "movdqu 0xd0(%rsp), %xmm0\n\t" "movdqu 0xe0(%rsp), %xmm1\n\t" "movdqu 0xf0(%rsp), %xmm2\n\t" \
                "movdqu 0x100(%rsp), %xmm3\n\t" "movdqu 0x110(%rsp), %xmm4\n\t" "movdqu 0x120(%rsp), %xmm5\n\t" \
                "movq %rbp, %rsp\n\t" "popq %rbp\n\t" "ret\n\t"); \
    }
#endif

SCL_THUNK(tt_CHKOVF, CHKOVF_port)
SCL_THUNK(tt_mth_CountLowZeros, mth_CountLowZeros_port)
SCL_THUNK(tt_scl_FRound, scl_FRound_port)
SCL_THUNK(tt_scl_SRound, scl_SRound_port)
SCL_THUNK(tt_scl_FixRound, scl_FixRound_port)
SCL_THUNK(tt_scl_Scale, scl_Scale_port)
SCL_THUNK(tt_scl_ScaleFromFixedFUnits, scl_ScaleFromFixedFUnits_port)
SCL_THUNK(tt_scl_ScaleBack, scl_ScaleBack_port)
SCL_THUNK(tt_scl_ConvertToFixedFUnits, scl_ConvertToFixedFUnits_port)
SCL_THUNK(tt_mth_IntelMul, mth_IntelMul_port)
SCL_THUNK(tt_mth_ReduceMatrix, mth_ReduceMatrix_port)
SCL_THUNK(tt_mth_FoldPointSizeResolution, mth_FoldPointSizeResolution_port)
SCL_THUNK(tt_adjustTrans, adjustTrans_port)
SCL_THUNK(tt_mth_PositiveSquare, mth_PositiveSquare_port)
SCL_THUNK(tt_mth_90degRotationFactorForEmboldening, mth_90degRotationFactorForEmboldening_port)
SCL_THUNK(tt_mth_90degClosestRotationFactor, mth_90degClosestRotationFactor_port)
SCL_THUNK(tt_scl_InitializeChildScaling, scl_InitializeChildScaling_port)
SCL_THUNK(tt_scl_InitializeScaling, scl_InitializeScaling_port)
SCL_THUNK(tt_scl_PostTransformGlyph, scl_PostTransformGlyph_port)
SCL_THUNK(tt_scl_ScaleOldCharPoints, scl_ScaleOldCharPoints_port)
SCL_THUNK(tt_scl_ScaleOldPhantomPoints, scl_ScaleOldPhantomPoints_port)
SCL_THUNK(tt_scl_CopyCurrentCharPoints, scl_CopyCurrentCharPoints_port)
SCL_THUNK(tt_scl_CopyCurrentPhantomPoints, scl_CopyCurrentPhantomPoints_port)
SCL_THUNK(tt_scl_OriginalCharPointsToCurrentFixedFUnits, scl_OriginalCharPointsToCurrentFixedFUnits_port)
SCL_THUNK(tt_scl_OriginalPhantomPointsToCurrentFixedFUnits, scl_OriginalPhantomPointsToCurrentFixedFUnits_port)
SCL_THUNK(tt_scl_ScaleFixedCurrentCharPoints, scl_ScaleFixedCurrentCharPoints_port)
SCL_THUNK(tt_scl_ScaleFixedCurrentPhantomPoints, scl_ScaleFixedCurrentPhantomPoints_port)
SCL_THUNK(tt_scl_ScaleBackCurrentCharPoints, scl_ScaleBackCurrentCharPoints_port)
SCL_THUNK(tt_scl_ScaleBackCurrentPhantomPoints, scl_ScaleBackCurrentPhantomPoints_port)
SCL_THUNK(tt_scl_CalcOrigPhantomPoints, scl_CalcOrigPhantomPoints_port)
SCL_THUNK(tt_scl_AdjustOldSideBearingPoints, scl_AdjustOldSideBearingPoints_port)
SCL_THUNK(tt_scl_RoundCurrentSideBearingPnt, scl_RoundCurrentSideBearingPnt_port)
SCL_THUNK(tt_scl_ShiftCurrentCharPoints, scl_ShiftCurrentCharPoints_port)
SCL_THUNK(tt_scl_ScaleDownFromSubPixelOverscale, scl_ScaleDownFromSubPixelOverscale_port)
SCL_THUNK(tt_scl_AdjustCompatibleMetrics, scl_AdjustCompatibleMetrics_port)
SCL_THUNK(tt_scl_CalcComponentOffset, scl_CalcComponentOffset_port)
SCL_THUNK(tt_scl_UpdateParentElement, scl_UpdateParentElement_port)
SCL_THUNK(tt_scl_IncrementChildElement, scl_IncrementChildElement_port)
SCL_THUNK(tt_scl_ScaleAdvanceWidth, scl_ScaleAdvanceWidth_port)
SCL_THUNK(tt_scl_ScaleAdvanceHeight, scl_ScaleAdvanceHeight_port)
SCL_THUNK(tt_scl_ComputeScaling, scl_ComputeScaling_port)
