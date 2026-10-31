/*
 * The TrueType rasterizer driver of the standalone texture path (idwr/ttraster.c): what
 * TrueTypeRasterizer::Implementation does around the ported scaler. Internal to idwr; built
 * only with the scaler (-DIDWR_STANDALONE).
 */
#ifndef IDWR_TTRASTER_H
#define IDWR_TTRASTER_H
#include <stdint.h>
#include "idwr.h"

/* FontRasterizer::GlyphBitmap, as GlyphBitmapRasterizationState stores it (0x20 bytes) */
typedef struct {
    int32_t left, top;       /* top = rows above the origin */
    uint32_t width, height;
    uint32_t flags;          /* GlyphRasterizationFlags: 1 aliased overlay, 2 whole-pixel origin,
                                4 forces the grayscale texture */
    int32_t stride;          /* bytes per row, a multiple of 4 */
    const uint32_t *bits;    /* rows from the top, first pixel in the least significant bit */
} idwr_tt_bitmap;

typedef struct idwr_tt idwr_tt;

/* Implementation::Implementation + Initialize: fs_NewSfnt on the font, with the variation
 * hooks for `axes` normalized coordinates (fvar order; 0 for a non-variable face). The font
 * and the coordinates must outlive the driver. NULL on failure. */
idwr_tt *idwr_tt_open(const idwr_font *font, const float *coords, uint16_t axes);
void idwr_tt_close(idwr_tt *t);
/* Implementation::NewTransform: point size, 16.16 font matrix {xx, xy, yx, yy} (y down),
 * overscale, RenderingFlags and strike ppem byte. Raises (native.h) what the original throws. */
void idwr_tt_transform(idwr_tt *t, uint32_t point_size, const int32_t matrix[4], uint16_t overscale_x,
                      uint16_t overscale_y, uint32_t flags, uint8_t ppem);
/* Implementation::GetBitmap(false, ...) for `glyph` (RasterizeInternal, fs_FindBitMapSize,
 * fs_ContourScan, the retry without strikes); glyphs the active strike contains come from
 * reimpl/idwr/sbit.c. The bits stay valid until the next call. Raises what the original throws. */
void idwr_tt_bitmap_get(idwr_tt *t, uint16_t glyph, idwr_tt_bitmap *out);
/* Implementation::NewGlyph(glyph, full_width) + GetMetrics: FontRasterizer::GlyphMetrics in design
 * units {advance, advance height, vertical origin, lsb, rsb, tsb, bsb, GlyphRasterizationFlags}
 * (the GDI-compatible metrics, hdmx widths included). Raises what the original throws. */
void idwr_tt_glyph_metrics(idwr_tt *t, uint16_t glyph, int full_width, int32_t out[8]);

#endif
