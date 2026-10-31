/*
 * The outline driver (outline_driver.c): the TrueType scaler set up the way DWrite.dll's
 * GlyphOutlineRasterizationState sets it up for IDWriteFontFace::GetGlyphRunOutline, and one
 * glyph's points after fs__Contour. Internal: the public API is idwr_outline_scaler_* and
 * idwr_glyph_run_outline_hinted in reimpl/idwr/idwr.h. The fs_* entries are reached through
 * idwr_orig, so the same code runs on the ports alone (-DIDWR_STANDALONE) or, in oracle mode, on
 * DWrite.dll's scaler with the oracle's function swap (reimpl/oracle/outline_oracle.c).
 */
#ifndef TT_OUTLINE_DRIVER_H
#define TT_OUTLINE_DRIVER_H
#include <stdint.h>
#include "../idwr/idwr.h"

typedef struct tt_outline tt_outline;

/* Negative controls: each bit breaks one part of the setup (reimpl/outline_hinted_check.py).
 *   1  point size 12 << 16 instead of the wrapped 0      2  grid-fit bit set (input +0xd6 = 0)
 *   4  rendering (simulation) flags dropped               8  no variation coordinates to the scaler
 *   16 2.14 coordinates truncated instead of rounded      32 fs__Contour flag 0 at once (no hinting)
 *   64 origins without the bold advance (public API only) 128 each glyph's first point moved by
 *                                                             1/64 font unit in x
 *   256 sideways runs of an oblique face without rendering flag 8 (the upright shear; public API) */
enum {
    TT_OUTLINE_PERTURB_POINT_SIZE = 1, TT_OUTLINE_PERTURB_GRID_FIT = 2, TT_OUTLINE_PERTURB_FLAGS = 4,
    TT_OUTLINE_PERTURB_AXES = 8, TT_OUTLINE_PERTURB_COORDS_TRUNC = 16, TT_OUTLINE_PERTURB_NO_HINT = 32,
    TT_OUTLINE_PERTURB_BOLD_ADVANCE = 64, TT_OUTLINE_PERTURB_POINT = 128, TT_OUTLINE_PERTURB_SIDEWAYS_SHEAR = 256
};

/* Implementation::Initialize, then GlyphOutlineRasterizationState's NewTransform, for `font`
 * (which must stay open). `coords` (`axes` of them, from idwr_normalize_axes) select a variable
 * instance. `rendering_flags` as MakeRasterizerFlagsForOutlineRendering: 2 bold, 4 oblique,
 * 8 sideways oblique. NULL on an fs error, stored in *error when `error` is not NULL. */
tt_outline *tt_outline_open(const idwr_font *font, const float *coords, uint16_t axes, uint32_t rendering_flags,
                            int perturb, int *error);
/* NewGlyph + GetOutline: the points after fs__Contour (26.6 at ppem = unitsPerEm, font
 * units × 64). The arrays stay valid until the next call. Returns 0 (0 contours when
 * NewTransform skipped the transformation), or the fs error. */
int tt_outline_glyph(tt_outline *o, uint16_t glyph, idwr_glyph_points *out);
void tt_outline_close(tt_outline *o);

#endif
