/*
 * The standalone CFF/CFF2 path of idwr (idwr/cffraster.c): DWrite's CffRasterizer, i.e. the ported
 * glue and Adobe ATM rasterizer of the reimpl/cff sources, on a idwr_font. Internal to idwr; built only in
 * standalone mode with the CFF ports (-DIDWR_STANDALONE -DIDWR_CFF and the reimpl/cff sources).
 *
 * Every function raises (truetype/native.h) what the original throws; the callers arm the
 * catch frame.
 */
#ifndef IDWR_CFFRASTER_H
#define IDWR_CFFRASTER_H
#include <stdint.h>
#include "idwr.h"

/* FontRasterizer::CreateRasterizer for a CFF face: a CffRasterizer (the ported constructor) for
 * `font` and the instance of `axes` normalized coordinates (idwr_normalize_axes; 0 for the
 * default instance). The object's first field is its IFontRasterizer vtable: {deleting
 * destructor, NewTransform, NewGlyph, GetBitmap, GetMetrics, GetOutline, GetGlyphCount,
 * GetDesignUnitsPerEm}, the ports. `font` must stay open while it lives; `coords` are only read
 * here. */
uint8_t *idwr_cff_rasterizer_create(const idwr_font *font, const float *coords, uint16_t axes);
/* slot 0, the deleting destructor */
void idwr_cff_rasterizer_destroy(uint8_t *r);

/* IFontFace::GetDesignGlyphMetrics without the simulation adjustments (those are FontFace's,
 * idwr/glyphrun.c): OpenTypeCffFontMetricsBuilder::GetGlyphMetrics (the port) on a rasterizer
 * with SetDesignTransform, the 16-bit DesignGlyphMetrics widened as DWRITE_GLYPH_METRICS.
 * Returns 1, or 0 when the rasterizer fails. Keeps rasterizers per font (idwr_cff_font_release). */
int idwr_cff_design_glyph_metrics(const idwr_font *font, const float *coords, uint16_t axes, uint16_t glyph,
                                 idwr_glyph_metrics *out);
/* frees what idwr_cff_design_glyph_metrics kept for `font` (idwr_font_close) */
void idwr_cff_font_release(idwr_font *font);

/* IDWriteFontFace::GetGlyphRunOutline on a CFF face: GlyphOutlineRasterizationState's
 * rasterizer (NewTransform with the unitsPerEm matrix, the simulations' rendering flags), each
 * glyph's CffRasterizer::GetOutline commands streamed by idwr/outline.c */
typedef struct idwr_cff_outline idwr_cff_outline;
idwr_cff_outline *idwr_cff_outline_open(const idwr_font *font, const float *coords, uint16_t axes,
                                      uint32_t simulations, int *error);
int idwr_cff_outline_run(idwr_cff_outline *o, float em_size, const uint16_t *glyphs, const float *advances,
                        const idwr_glyph_offset *offsets, uint32_t count, int sideways, int right_to_left,
                        idwr_path_cmd *out, int capacity);
int idwr_cff_outline_last_error(const idwr_cff_outline *o);
void idwr_cff_outline_close(idwr_cff_outline *o);

#endif
