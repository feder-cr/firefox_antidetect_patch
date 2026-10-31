/*
 * idwr: DirectWrite 10.0.26100.9711 text output, reimplemented in portable C.
 *
 * The subset the engine calls (notes/integration.md): font and glyph metrics, the recommended
 * rendering mode, and glyph rasterization to alpha textures. Each function documents the
 * DirectWrite call it reproduces; the checks live in reimpl/idwr_check.py.
 */
#ifndef IDWR_H
#define IDWR_H
#include <stddef.h>
#include <stdint.h>

/* IDWR_STATIC: built into the engine. Nothing is exported, so libxul's symbol table lists no
 * idwr_* name; the engine calls the functions directly. Otherwise the shared library the
 * harnesses load exports the public API.
 *
 * Threads: an idwr_font, and every face, scaler and texture made from it, belongs to one thread
 * at a time. The font carries lazy caches (the CFF metrics rasterizer, cffraster.c) written on
 * first use through calls that take it as const, so the engine keeps one per thread or context
 * (notes/phase4-plan.md, A). The scaler's error frame is already per thread (native.c). */
#if defined(IDWR_STATIC)
#define IDWR_API
#elif defined(_WIN32)
#define IDWR_API __declspec(dllexport)
#else
#define IDWR_API __attribute__((visibility("default")))
#endif

typedef struct idwr_font idwr_font;

/* DWRITE_FONT_FACE_TYPE values */
enum { IDWR_FACE_CFF = 0, IDWR_FACE_TRUETYPE = 1, IDWR_FACE_OPENTYPE_COLLECTION = 2 };

/* DWRITE_RENDERING_MODE values (the DWRITE_RENDERING_MODE1 numbering) */
enum {
    IDWR_MODE_DEFAULT = 0, IDWR_MODE_ALIASED = 1, IDWR_MODE_GDI_CLASSIC = 2, IDWR_MODE_GDI_NATURAL = 3,
    IDWR_MODE_NATURAL = 4, IDWR_MODE_NATURAL_SYMMETRIC = 5, IDWR_MODE_OUTLINE = 6
};

/* DWRITE_MEASURING_MODE values */
enum { IDWR_MEASURING_NATURAL = 0, IDWR_MEASURING_GDI_CLASSIC = 1, IDWR_MEASURING_GDI_NATURAL = 2 };

/* DWRITE_FONT_METRICS1 */
typedef struct {
    uint16_t designUnitsPerEm, ascent, descent;
    int16_t lineGap;
    uint16_t capHeight, xHeight;
    int16_t underlinePosition;
    uint16_t underlineThickness;
    int16_t strikethroughPosition;
    uint16_t strikethroughThickness;
    int16_t glyphBoxLeft, glyphBoxTop, glyphBoxRight, glyphBoxBottom;
    int16_t subscriptPositionX, subscriptPositionY, subscriptSizeX, subscriptSizeY;
    int16_t superscriptPositionX, superscriptPositionY, superscriptSizeX, superscriptSizeY;
    int32_t hasTypographicMetrics;
} idwr_font_metrics;

/* DWRITE_GLYPH_METRICS */
typedef struct {
    int32_t leftSideBearing;
    uint32_t advanceWidth;
    int32_t rightSideBearing, topSideBearing;
    uint32_t advanceHeight;
    int32_t bottomSideBearing, verticalOriginY;
} idwr_glyph_metrics;

/* Opens face `index` of an sfnt file (TrueType, OpenType/CFF, or a collection). The data must
 * stay valid while the font is open. NULL if the file is not a supported sfnt. */
IDWR_API idwr_font *idwr_font_open(const uint8_t *data, size_t size, uint32_t index);
IDWR_API void idwr_font_close(idwr_font *font);

/* IDWriteFontFace::GetType */
IDWR_API int idwr_font_face_type(const idwr_font *font);
/* IDWriteFontFace::TryGetFontTable: the table's bytes, or NULL (and *size 0) */
IDWR_API const uint8_t *idwr_font_table(const idwr_font *font, uint32_t tag, uint32_t *size);
/* IDWriteFontFace::IsSymbolFont */
IDWR_API int idwr_font_is_symbol(const idwr_font *font);
/* IDWriteFontFace::GetGlyphCount */
IDWR_API uint16_t idwr_font_glyph_count(const idwr_font *font);
/* IDWriteFontFace::GetGlyphIndices for one code point (0 if unmapped) */
IDWR_API uint16_t idwr_font_glyph_index(const idwr_font *font, uint32_t codepoint);

/* IDWriteFontFace1::GetMetrics (no font variations) */
IDWR_API void idwr_get_font_metrics(const idwr_font *font, idwr_font_metrics *out);
/* IDWriteFontFace::GetDesignGlyphMetrics, isSideways FALSE */
IDWR_API int idwr_get_design_glyph_metrics(const idwr_font *font, uint16_t glyph, idwr_glyph_metrics *out);

/* IDWriteFontFace::GetRecommendedRenderingMode with the default rendering params (rendering
 * mode DEFAULT). `smoothing` is the system's font smoothing for the GDI measuring modes:
 * 1 none, 2 grayscale, 3 ClearType (DWriteRenderingParams::DetermineCompatibleSmoothingMode).
 * Returns a IDWR_MODE_* value, or -1 where the model needs data it does not reproduce. */
IDWR_API int idwr_recommended_rendering_mode(const idwr_font *font, float em_size, float pixels_per_dip,
                                          int measuring_mode, int smoothing);

/* DWRITE_FONT_AXIS_VALUE, with the tag as IDWR_TAG('w', 'g', 'h', 't') */
typedef struct {
    uint32_t tag;
    float value;
} idwr_axis_value;

/* The normalized coordinates of an instance, in fvar axis order: the first value for each
 * axis tag, clamped to the axis range (defaults for missing axes), normalized and mapped
 * through avar. Returns the axis count (0 without fvar), or -1 for malformed tables or more
 * axes than `capacity`. */
IDWR_API int idwr_normalize_axes(const idwr_font *font, const idwr_axis_value *values, uint32_t count, float *coords,
                               int capacity);
/* gvar deltas for one glyph, added to the `points` coordinates (outline points then the 4
 * phantom points, font units). Returns 0, or -1 with x and y unchanged for malformed data. */
IDWR_API int idwr_apply_gvar(const idwr_font *font, const float *coords, uint16_t axes, uint16_t glyph,
                           const int16_t *end_points, uint16_t contours, int32_t *x, int32_t *y, uint32_t points);
/* IDWriteFontFace::GetDesignGlyphMetrics of a variable instance: the default glyph's metrics
 * plus the HVAR deltas (advance; lsb and rsb when the font maps them) */
IDWR_API int idwr_get_design_glyph_metrics_instance(const idwr_font *font, const float *coords, uint16_t axes,
                                                  uint16_t glyph, idwr_glyph_metrics *out);
/* cvar deltas added to the `count` CVT entries */
IDWR_API int idwr_apply_cvar(const idwr_font *font, const float *coords, uint16_t axes, int32_t *cvt, uint32_t count);

/* DWRITE_GLYPH_OFFSET */
typedef struct {
    float advanceOffset, ascenderOffset;
} idwr_glyph_offset;

/* ID2D1SimplifiedGeometrySink calls: SetFillMode(WINDING), BeginFigure(p[0], p[1], FILLED),
 * AddLines with one point, AddBeziers with one bezier (p[0..5]), EndFigure(CLOSED) */
enum { IDWR_PATH_FILL_WINDING = 0, IDWR_PATH_BEGIN = 1, IDWR_PATH_LINE = 2, IDWR_PATH_BEZIER = 3, IDWR_PATH_END = 4 };
typedef struct {
    int op;
    float p[6];
} idwr_path_cmd;

/* IDWriteFontFace::GetGlyphRunOutline from the glyf table, unhinted, without simulations: the
 * sink calls, in order. `coords` (`axes` of them, from idwr_normalize_axes) select a variable
 * instance; axes = 0 for the default (on a variable font, its default instance: the design
 * metrics the origins read are the instance's, as on DWrite's default face). Returns the call
 * count (which may exceed `capacity`; only `capacity` are stored), or -1. `sideways` non-zero
 * is isSideways TRUE (vertical text): each glyph turned a quarter, its origin at the vertical
 * origin (verticalOriginY of the design glyph metrics: vmtx, or the typo ascender fallback),
 * the pen moving by the given advances or the advance heights, the glyph centred on half its
 * advance width.
 * DWrite hints the outline at ppem = unitsPerEm, so on faces whose glyph programs move points
 * at that size (Arial Black, Georgia, Verdana Bold, ...) this differs by fractions of a font
 * unit: idwr_glyph_run_outline_hinted below is the exact one. This one needs no scaler. */
IDWR_API int idwr_glyph_run_outline(const idwr_font *font, const float *coords, uint16_t axes, float em_size,
                                  const uint16_t *glyphs, const float *advances, const idwr_glyph_offset *offsets,
                                  uint32_t count, int sideways, int right_to_left, idwr_path_cmd *out, int capacity);

/* The outline points of one glyph as TrueTypeRasterizer::Implementation::GetOutline reads them
 * from the scaler after fs__Contour: 26.6 at ppem = unitsPerEm (font units × 64), on-curve
 * where `on` is non-zero, contour end points. */
typedef struct {
    const int32_t *x, *y;
    const uint8_t *on;
    const uint16_t *ends;
    uint32_t contours;
} idwr_glyph_points;
/* Fills `out` for `glyph`; the arrays stay valid until the next call. Returns 0, or -1. */
typedef int (*idwr_points_fn)(void *ctx, uint16_t glyph, idwr_glyph_points *out);

/* idwr_glyph_run_outline with the points of each glyph from `points` (for example
 * idwr_outline_scaler_glyph below) instead of the glyf loader. `simulations` is
 * DWRITE_FONT_SIMULATIONS: bold (1) widens the design advances used for origins
 * (FontFace::GetGlyphAdvances); the outline itself comes from the provider, which applies bold
 * and oblique. Returns the call count, or -1. */
IDWR_API int idwr_glyph_run_outline_points(const idwr_font *font, const float *coords, uint16_t axes, int simulations,
                                         float em_size, const uint16_t *glyphs, const float *advances,
                                         const idwr_glyph_offset *offsets, uint32_t count, int sideways,
                                         int right_to_left, idwr_points_fn points, void *ctx, idwr_path_cmd *out,
                                         int capacity);

/* ---- Glyph outlines from the TrueType scaler (reimpl/truetype/outline_driver.c) ----
 *
 * IDWriteFontFace::GetGlyphRunOutline exactly, on every TrueType face: the points come from
 * the ported scaler set up as GlyphOutlineRasterizationState does (ppem = unitsPerEm, glyph
 * programs run, bold embolden and oblique shear in the scaler, variable instances through
 * gvar and cvar), then the commands, origins and streaming of idwr_glyph_run_outline. Origins
 * without given advances take FontFace::GetGlyphAdvances, which under bold adds
 * (unitsPerEm + 25) / 50 to a non-zero design advance of a glyph with ink.
 *
 * These need the scaler sources (the .c files of reimpl/truetype) in the build. Built with
 * -DIDWR_STANDALONE they run on the ports alone: DWrite.dll is never loaded. */
typedef struct idwr_outline_scaler idwr_outline_scaler;

/* The scaler for one face as GetGlyphRunOutline sets it up: `coords` (`axes` of them, from
 * idwr_normalize_axes; 0 for the default instance, which on a variable font gives the scaler the
 * fvar axes at their defaults) and `simulations` (IDWR_SIM_BOLD, IDWR_SIM_OBLIQUE: those of the
 * font face). Runs the font and pre-programs once. `font` (and its data) must stay open while
 * the scaler is. NULL when the scaler rejects the face. */
IDWR_API idwr_outline_scaler *idwr_outline_scaler_open(const idwr_font *font, const float *coords, uint16_t axes,
                                                    uint32_t simulations);
IDWR_API void idwr_outline_scaler_close(idwr_outline_scaler *s);
/* GetGlyphRunOutline for one run (see idwr_glyph_run_outline for the arguments and the
 * result), sideways runs included: on an oblique face these use a second rasterizer with the
 * sideways shear (rendering flag 8), opened on the first sideways run. */
IDWR_API int idwr_outline_scaler_run(idwr_outline_scaler *s, float em_size, const uint16_t *glyphs,
                                   const float *advances, const idwr_glyph_offset *offsets, uint32_t count,
                                   int sideways, int right_to_left, idwr_path_cmd *out, int capacity);
/* A idwr_points_fn (ctx: a idwr_outline_scaler): one glyph's hinted points, font units × 64 */
IDWR_API int idwr_outline_scaler_glyph(void *s, uint16_t glyph, idwr_glyph_points *out);
/* The fs error of the last open, glyph or run call (0 when none) */
IDWR_API int idwr_outline_scaler_last_error(const idwr_outline_scaler *s);
/* One-shot: idwr_outline_scaler_open, _run, _close. Prefer a scaler kept open for many runs of
 * the same face (the font programs run on every open). */
IDWR_API int idwr_glyph_run_outline_hinted(const idwr_font *font, const float *coords, uint16_t axes,
                                         uint32_t simulations, float em_size, const uint16_t *glyphs,
                                         const float *advances, const idwr_glyph_offset *offsets, uint32_t count,
                                         int sideways, int right_to_left, idwr_path_cmd *out, int capacity);

/* Embedded bitmap strikes, for one glyph drawn alone without a transform, grid fit DEFAULT */
enum { IDWR_STRIKE_NONE = 0, IDWR_STRIKE_ACTIVE = 1, IDWR_STRIKE_GLYPH = 2 };
/* OpenTypeFontFaceBuilder::IsLegacyEastAsian */
IDWR_API int idwr_is_legacy_east_asian(const idwr_font *font);
/* NONE: no strike at this size and mode; ACTIVE: a strike is in use but this glyph comes from
 * its outline; GLYPH: the glyph is drawn from the strike. `mode` is a IDWR_MODE_* value. */
IDWR_API int idwr_strike_state(const idwr_font *font, int mode, float em_size, uint16_t glyph);
typedef struct {
    int32_t bearing_x, bearing_y; /* the bitmap's top-left pixel is at (bearing_x, -bearing_y) */
    uint32_t width, height;
} idwr_strike_glyph;
/* The strike bitmap of a GLYPH: width × height bytes, 255 where a bit is set. `pixels` may be
 * NULL to get the size only. Returns 1, 0 when the glyph is not drawn from a strike, -1 for
 * malformed or unmodelled data (composite image formats). */
IDWR_API int idwr_strike_bitmap(const idwr_font *font, int mode, float em_size, uint16_t glyph, idwr_strike_glyph *out,
                              uint8_t *pixels, size_t capacity);

#define IDWR_TAG(a, b, c, d) ((uint32_t)(a) << 24 | (uint32_t)(b) << 16 | (uint32_t)(c) << 8 | (uint32_t)(d))

/* ---- Glyph run analysis: the run-level part of IDWriteGlyphRunAnalysis (idwr/glyphrun.c) ----
 *
 * GlyphRunAnalysis's constructor, minus rasterization, in two calls:
 *   1. idwr_glyph_run_analyze: the mode attributes, the font matrix and strike ppem, the mode
 *      switch, grid fit, the RenderingFlags and the glyph bitmap cache key, the glyph origins;
 *   2. the caller rasterizes each glyph with `raster` (one GlyphBitmapLayout per glyph);
 *   3. idwr_glyph_run_place, given each glyph's layout header: the whole-pixel origins, the
 *      glyph order (embedded bitmaps last), the bitmap positions and the run rects.
 * Then the two bounds getters. All float arithmetic is single precision as DWrite does it. */

/* DWRITE_MATRIX */
typedef struct {
    float m11, m12, m21, m22, dx, dy;
} idwr_matrix;

/* DWRITE_RENDERING_MODE1_NATURAL_SYMMETRIC_DOWNSAMPLED (grayscale 2x2) */
enum { IDWR_MODE_NATURAL_SYMMETRIC_DOWNSAMPLED = 7 };
/* DWRITE_GRID_FIT_MODE, DWRITE_TEXT_ANTIALIAS_MODE, DWRITE_FONT_SIMULATIONS */
enum { IDWR_GRID_FIT_DEFAULT = 0, IDWR_GRID_FIT_DISABLED = 1, IDWR_GRID_FIT_ENABLED = 2 };
enum { IDWR_AA_CLEARTYPE = 0, IDWR_AA_GRAYSCALE = 1 };
enum { IDWR_SIM_BOLD = 1, IDWR_SIM_OBLIQUE = 2 };
/* errors */
enum { IDWR_E_INVALIDARG = -1, IDWR_E_NEED_METRICS = -2 };

typedef struct {
    float em_size;
    uint32_t count;
    const uint16_t *glyphs;
    const float *advances;            /* NULL: the font's advances */
    const idwr_glyph_offset *offsets;  /* NULL: none */
    int sideways;
    uint32_t bidi_level;              /* odd: right to left */
    const idwr_matrix *transform;      /* NULL: none */
    float pixels_per_dip;             /* 1 for the IDWriteFactory2+ CreateGlyphRunAnalysis */
    float origin_x, origin_y;         /* baseline origin, DIPs */
    int rendering_mode;               /* DWRITE_RENDERING_MODE1 (0 and 6 are E_INVALIDARG) */
    /* 1 for IDWriteFactory3::CreateGlyphRunAnalysis (DWRITE_RENDERING_MODE1); 0 for
     * IDWriteFactory::CreateGlyphRunAnalysis (pixelsPerDip, grid fit DEFAULT, ClearType) and
     * IDWriteFactory2::CreateGlyphRunAnalysis (DWRITE_RENDERING_MODE, grid fit, antialias), where
     * NATURAL under a rotation or skew becomes NATURAL_SYMMETRIC (or the downsampled 2x2 mode at
     * 172.8 dpi and above) */
    int keep_mode;
    int measuring_mode;               /* IDWR_MEASURING_* */
    int grid_fit;                     /* IDWR_GRID_FIT_* */
    int antialias;                    /* IDWR_AA_* */
    uint32_t simulations;             /* IDWR_SIM_*, those of the font face */
    /* Optional per-glyph metrics, used only when the origins need the font's metrics (no
     * advances, right to left, or sideways): IDWriteFontFace::GetGdiCompatibleGlyphMetrics(
     * em_size, pixels_per_dip, transform, measuring == GDI_NATURAL, glyphs, count, out,
     * sideways). Required for the GDI measuring modes (they need hinted advances); for
     * NATURAL, NULL means idwr computes the design metrics (with the simulations). */
    const idwr_glyph_metrics *glyph_metrics;
} idwr_glyph_run;

/* GlyphBitmapRasterizationParameters (0x20 bytes): how every glyph of the run is rasterized,
 * and the key of DWrite's glyph bitmap cache */
typedef struct {
    int32_t matrix[4];       /* 16.16 font matrix, y down: xx, xy, yx, yy */
    int32_t point_size;      /* round half up (em * 0.75) */
    uint16_t overscale_x, overscale_y;
    uint32_t flags;          /* FontRasterizer::RenderingFlags: 1 grid fit, 2 bold, 4 oblique,
                                8 sideways oblique, 0x10/0x20 GDI natural/classic widths,
                                0x40 strikes, 0x80 grayscale; 0x100 odd 2x2 phase */
    uint8_t ppem;            /* strike ppem byte, 0 when unknown */
    uint8_t pad[3];
} idwr_raster_params;

typedef struct {
    uint32_t attributes;     /* rendering mode attributes (notes/rasterizer.md) */
    uint32_t antialias;
    int rendering_mode;      /* after the NATURAL switch (keep_mode 0) */
    float scale;             /* pixelsPerDip, times m11 when a uniform transform was folded */
    float origin_x, origin_y;
    int has_transform;       /* 0 when there is none or it was folded */
    idwr_matrix transform;
    int grid_fit;            /* resolved: IDWR_GRID_FIT_DISABLED or ENABLED */
    idwr_raster_params raster;
    uint8_t spread;          /* thin-font widening of the 1-bit glyph bitmaps (0 or 1) */
    uint8_t thin_family;     /* the family is one of DWrite's thin fonts (Courier New, ...) */
    uint8_t grayscale;       /* grayscale antialiasing, or a glyph forced it (layout flag 4) */
    uint8_t horizontal;      /* every glyph shares the first glyph's baseline */
    uint32_t count;
    uint32_t outline_count;  /* glyphs [0, n) are merged, [n, count) drawn aliased on top */
    uint32_t odd_count;      /* 2x2 runs: glyphs [0, odd) are rasterized again with
                                raster.flags | 0x100 (the odd dither phase) */
    int32_t rect[4];         /* merged glyphs' bounds, overscaled units (left, top, right, bottom) */
    int32_t aliased_rect[4]; /* aliased glyphs' bounds, pixels */
} idwr_glyph_run_analysis;

/* The fields of a GlyphBitmapLayout header that positioning reads */
typedef struct {
    uint32_t flags;          /* header >> 26: 1 aliased overlay (strike), 2 whole-pixel origin,
                                4 forces grayscale */
    int16_t left, top;       /* overscaled units; top = rows above the origin */
    uint16_t width, height;
} idwr_glyph_layout;

/* Step 1. `origins` receives 2 * count floats (ComputeGlyphOrigins: DIPs, transformed).
 * Returns 0, IDWR_E_INVALIDARG (where DWrite fails) or IDWR_E_NEED_METRICS. */
IDWR_API int idwr_glyph_run_analyze(const idwr_font *font, const idwr_glyph_run *run, idwr_glyph_run_analysis *out,
                                  float *origins);
/* Step 3. `layouts` and `origins` in run order (origins as step 1 left them); fills
 * order[count] (the run index of each glyph slot) and positions[2 * count] (each slot's bitmap
 * top-left: overscaled units for [0, n), pixels for [n, count)), and the analysis' counts and
 * rects. Slots [0, odd_count) are then rasterized again with the odd 2x2 phase. */
IDWR_API void idwr_glyph_run_place(idwr_glyph_run_analysis *a, const idwr_glyph_layout *layouts, const float *origins,
                                 uint32_t *order, int32_t *positions);
/* IDWriteGlyphRunAnalysis::GetAlphaTextureBounds (texture type 0 aliased, 1 ClearType 3x1)
 * and GetRunBitmapBounds (raster types 0..5). Return 0 or IDWR_E_INVALIDARG. */
IDWR_API int idwr_glyph_run_alpha_texture_bounds(const idwr_glyph_run_analysis *a, int texture_type, int32_t rect[4]);
IDWR_API int idwr_glyph_run_bitmap_bounds(const idwr_glyph_run_analysis *a, int raster_type, int32_t rect[4]);

/* GetRenderingModeAttributes: the attributes of a DWRITE_RENDERING_MODE2 (0..9) and antialias
 * mode, 0 when out of range. (The run constructor reads another table for the requested mode,
 * without the strike bit on the natural symmetric modes.) */
IDWR_API uint32_t idwr_rendering_mode_attributes(int mode, int antialias);
/* GetTransformMaxScaleFactor: the largest singular value of the 2x2 part (1 for NULL) */
IDWR_API float idwr_transform_max_scale_factor(const idwr_matrix *m);
/* FontFaceLight::GetFontWeight for a face without simulations and without a STAT table (or with STAT and
 * no wght axis): the OS/2 weight (OS/2 or head) resolved with the weight words of the names, as
 * FontDifferentiator computes it (idwr/nameweight.c) */
IDWR_API int idwr_font_weight(const idwr_font *font);
/* OpenTypeFontFaceBuilder::IsThinFontFamily on the face's family name */
IDWR_API int idwr_font_is_thin_family(const idwr_font *font);

/* ---- Glyph run alpha textures: IDWriteGlyphRunAnalysis (idwr/texture.c) ----
 *
 * GetAlphaTextureBounds and CreateAlphaTexture of an IDWriteGlyphRunAnalysis created with
 * `run` (IDWriteFactory3::CreateGlyphRunAnalysis with keep_mode 1, IDWriteFactory's and
 * IDWriteFactory2's with keep_mode 0), rasterized by the ported TrueType scaler and embedded strikes, without
 * DWrite. Available in builds that include the scaler (reimpl/truetype, -DIDWR_STANDALONE).
 * The results are HRESULTs as DWrite returns them (0 = S_OK), or IDWR_E_NEED_METRICS. */
#define IDWR_HR_E_INVALIDARG ((int32_t)0x80070057)
#define IDWR_HR_E_FILEFORMAT ((int32_t)0x88985000)          /* DWRITE_E_FILEFORMAT */
#define IDWR_HR_E_NOT_SUFFICIENT_BUFFER ((int32_t)0x8007007a)
#define IDWR_HR_E_OUTOFMEMORY ((int32_t)0x8007000e)
#define IDWR_HR_E_NOTIMPL ((int32_t)0x80004001)          /* a face without glyf outlines (CFF) */
enum { IDWR_TEXTURE_ALIASED_1x1 = 0, IDWR_TEXTURE_CLEARTYPE_3x1 = 1 };

typedef struct idwr_glyph_run_texture idwr_glyph_run_texture;

/* CreateGlyphRunAnalysis: `font` (a TrueType face, which must stay open; CFF faces give
 * IDWR_HR_E_NOTIMPL) and, for a variable font, the instance's axis values as
 * IDWriteFontResource::CreateFontFace takes them (none: the default instance; ignored without
 * fvar). The run's simulations are the face's. Runs that need font advances (none given,
 * right to left, sideways) take them from the default instance's metrics. */
IDWR_API int32_t idwr_glyph_run_texture_create(const idwr_font *font, const idwr_axis_value *axis_values,
                                             uint32_t axis_count, const idwr_glyph_run *run,
                                             idwr_glyph_run_texture **out);
/* IDWriteGlyphRunAnalysis::GetAlphaTextureBounds */
IDWR_API int32_t idwr_glyph_run_texture_bounds(const idwr_glyph_run_texture *t, int texture_type, int32_t rect[4]);
/* IDWriteGlyphRunAnalysis::CreateAlphaTexture: `size` bytes at `buffer`, 1 (aliased) or 3
 * (ClearType) bytes per pixel of `rect` */
IDWR_API int32_t idwr_glyph_run_texture_render(idwr_glyph_run_texture *t, int texture_type, const int32_t rect[4],
                                             uint8_t *buffer, uint32_t size);
/* the run parameters (idwr_glyph_run_analyze, idwr_glyph_run_place) */
IDWR_API const idwr_glyph_run_analysis *idwr_glyph_run_texture_analysis(const idwr_glyph_run_texture *t);
/* slot `slot` in DWrite's order: its GlyphBitmapLayout (16-byte header + data, the format in
 * reimpl/truetype/glyphrun_layout.c) and bitmap position. 0, or -1 out of range. */
IDWR_API int idwr_glyph_run_texture_slot(const idwr_glyph_run_texture *t, uint32_t slot, const uint8_t **layout,
                                       int32_t position[2]);
IDWR_API void idwr_glyph_run_texture_destroy(idwr_glyph_run_texture *t);

/* ---- Font faces: simulations, variable instances and their metrics (idwr/face.c) ----
 *
 * A idwr_face is what IDWriteFontResource::CreateFontFace(simulations, axis values) returns (or
 * IDWriteFactory::CreateFontFace(simulations) for a face without fvar, whose axis values are
 * ignored): the engine's metrics calls take one. `font` (and its data) must outlive the face.
 * Results that are HRESULTs are 0 (S_OK) or IDWR_HR_*. Glyph ids past the glyph count are
 * glyph 0, as in DWrite. The face is not thread safe (the GDI calls cache a scaler in it). */
typedef struct idwr_face idwr_face;

/* `simulations`: IDWR_SIM_BOLD | IDWR_SIM_OBLIQUE. NULL for malformed fvar/avar or bad flags. */
IDWR_API idwr_face *idwr_face_create(const idwr_font *font, uint32_t simulations, const idwr_axis_value *values,
                                  uint32_t count);
IDWR_API void idwr_face_destroy(idwr_face *face);
IDWR_API uint32_t idwr_face_simulations(const idwr_face *face);
/* IDWriteFontFace3::GetWeight (FontFaceLight::GetFontWeight 0x180024a54): 700 with the bold simulation,
 * else the weight ReadFontFace stored (idwr_face_weight of the axis values; -1: not known) */
IDWR_API int idwr_face_get_weight(const idwr_face *face);
/* IDWriteFontFace1::GetMetrics (IDWriteFontFace::GetMetrics is its first 20 bytes): the MVAR
 * deltas of the instance; the oblique simulation shears the glyph box and the sub/superscript
 * x positions */
IDWR_API void idwr_face_get_metrics(const idwr_face *face, idwr_font_metrics *out);
/* IDWriteFontFace::GetDesignGlyphMetrics: the instance's design metrics with the bold and
 * oblique adjustments; `sideways` matters only under the oblique simulation */
IDWR_API int32_t idwr_face_get_design_glyph_metrics(const idwr_face *face, const uint16_t *glyphs, uint32_t count,
                                                  idwr_glyph_metrics *out, int sideways);
/* IDWriteFontFace1::GetDesignGlyphAdvances (advance heights when sideways) */
IDWR_API int32_t idwr_face_get_design_glyph_advances(const idwr_face *face, uint32_t count, const uint16_t *glyphs,
                                                   int32_t *advances, int sideways);
/* IDWriteFontFace1::GetGdiCompatibleMetrics (and IDWriteFontFace's, its first 20 bytes):
 * every field rounded to whole pixels of em_size * pixels_per_dip * the transform's scale,
 * ascent and descent from VDMX when the face has them at that ppem. `transform` may be NULL. */
IDWR_API int32_t idwr_face_get_gdi_compatible_metrics(const idwr_face *face, float em_size, float pixels_per_dip,
                                                    const idwr_matrix *transform, idwr_font_metrics *out);
/* IDWriteFontFace::GetRecommendedRenderingMode with the default rendering params (see
 * idwr_recommended_rendering_mode), on the instance: its gasp ranges take the MVAR gsp deltas */
IDWR_API int idwr_face_recommended_rendering_mode(const idwr_face *face, float em_size, float pixels_per_dip,
                                                int measuring_mode, int smoothing);
/* IDWriteFontFace1::IsMonospacedFont (Skia's fixed-pitch flag) */
IDWR_API int idwr_font_is_monospaced(const idwr_font *font);
/* idwr_recommended_rendering_mode for normalized coordinates (idwr_normalize_axes) */
IDWR_API int idwr_recommended_rendering_mode_instance(const idwr_font *font, const float *coords, uint16_t axes,
                                                    float em_size, float pixels_per_dip, int measuring_mode,
                                                    int smoothing);

/* IDWriteFontFace::GetGdiCompatibleGlyphMetrics and IDWriteFontFace1::GetGdiCompatibleGlyphAdvances
 * (idwr/gdimetrics.c): the hinted metrics of the TrueType scaler at the GDI-compatible ppem
 * (em_size * pixels_per_dip, rounded, times the transform's scale), hdmx widths, the full-width
 * rule of monospaced CJK faces; the design metrics where DWrite uses them (a transform without a
 * zero entry, or ppem = unitsPerEm without simulations). Available in builds with the scaler
 * (-DIDWR_STANDALONE); otherwise IDWR_HR_E_NOTIMPL. */
IDWR_API int32_t idwr_face_get_gdi_compatible_glyph_metrics(idwr_face *face, float em_size, float pixels_per_dip,
                                                          const idwr_matrix *transform, int use_gdi_natural,
                                                          const uint16_t *glyphs, uint32_t count,
                                                          idwr_glyph_metrics *out, int sideways);
IDWR_API int32_t idwr_face_get_gdi_compatible_glyph_advances(idwr_face *face, float em_size, float pixels_per_dip,
                                                           const idwr_matrix *transform, int use_gdi_natural,
                                                           int sideways, uint32_t count, const uint16_t *glyphs,
                                                           int32_t *advances);

/* ---- Colour fonts: COLRv0 layers and CPAL palettes (idwr/color.c) ----
 *
 * What Skia's DirectWrite scaler asks of a colour font: IDWriteFontFace2::IsColorFont and the
 * palettes, and IDWriteFactory2::TranslateColorGlyphRun, which splits a glyph run into the
 * COLRv0 layers of its colour glyphs (one run per layer, with the layer's palette colour) after
 * a run of the other glyphs. The COLR/CPAL validation of DWrite's face builder is applied: a font
 * it rejects (CreateFontFace fails with DWRITE_E_FILEFORMAT) gives IDWR_HR_E_FILEFORMAT from
 * idwr_font_check_color_tables and from the calls below, and must be refused as DWrite does. */
#define IDWR_HR_E_NOCOLOR ((int32_t)0x8898500C)          /* DWRITE_E_NOCOLOR */
#define IDWR_HR_E_NOT_VALID_STATE ((int32_t)0x8007139F)  /* GetCurrentRun without a current run */
/* DWRITE_GLYPH_IMAGE_FORMATS (the ones idwr models) */
enum {
    IDWR_IMAGE_NONE = 0, IDWR_IMAGE_TRUETYPE = 1, IDWR_IMAGE_CFF = 2, IDWR_IMAGE_COLR = 4,
    IDWR_IMAGE_COLR_PAINT_TREE = 0x100
};
/* IDWriteFactory2::TranslateColorGlyphRun's formats */
#define IDWR_IMAGE_FACTORY2 (IDWR_IMAGE_TRUETYPE | IDWR_IMAGE_CFF | IDWR_IMAGE_COLR)
/* DWRITE_COLOR_F */
typedef struct {
    float r, g, b, a;
} idwr_color_f;

/* CreateFontFace's verdict on the COLR and CPAL tables: 0, or IDWR_HR_E_FILEFORMAT (ColrBuilder:
 * a short table, records past its end, a layer palette index >= numPaletteEntries other than
 * 0xFFFF, a base glyph without layers or past the layer records, no palette, a palette past the
 * colour records) */
IDWR_API int32_t idwr_font_check_color_tables(const idwr_font *font);
/* IDWriteFontFace2::IsColorFont: COLR and CPAL, or an SVG, sbix or CBDT table */
IDWR_API int idwr_face_is_color_font(idwr_face *face);
/* IDWriteFontFace4::GetGlyphImageFormats(): TRUETYPE (glyf), CFF ('CFF '), COLR | COLR_PAINT_TREE
 * (COLR and CPAL); the bitmap formats are not modelled */
IDWR_API uint32_t idwr_face_get_glyph_image_formats(idwr_face *face);
/* IDWriteFontFace2::GetColorPaletteCount and GetPaletteEntryCount (0 without CPAL) */
IDWR_API uint32_t idwr_face_get_color_palette_count(idwr_face *face);
IDWR_API uint32_t idwr_face_get_palette_entry_count(idwr_face *face);
/* IDWriteFontFace2::GetPaletteEntries: IDWR_HR_E_NOCOLOR for a palette out of range,
 * IDWR_HR_E_INVALIDARG for entries out of range. Each channel is byte / 255 in single precision
 * ((float)byte * 0.003921569f), 255 exactly 1. */
IDWR_API int32_t idwr_face_get_palette_entries(idwr_face *face, uint32_t palette, uint32_t first, uint32_t count,
                                             idwr_color_f *out);

/* DWRITE_COLOR_GLYPH_RUN1 without the font face: the glyph run (the arrays belong to the
 * enumerator and change with each run), the baseline origin, the colour and palette index
 * (0xFFFF: the text colour, colour 0), and the image format and measuring mode */
typedef struct {
    float em_size;
    uint32_t count;
    const uint16_t *glyphs;
    const float *advances;
    const idwr_glyph_offset *offsets;  /* NULL when the run had none */
    int sideways;
    uint32_t bidi_level;
    float baseline_x, baseline_y;
    idwr_color_f color;
    uint16_t palette_index;
    uint32_t image_format;
    int measuring_mode;
} idwr_color_glyph_run;
typedef struct idwr_color_run_enum idwr_color_run_enum;

/* IDWriteFactory2::TranslateColorGlyphRun (desired_formats IDWR_IMAGE_FACTORY2) and
 * IDWriteFactory4's (any formats; COLR_PAINT_TREE is E_INVALIDARG there, and bitmap formats on a
 * font with bitmap tables give IDWR_HR_E_NOTIMPL). From `run`: em_size, count, glyphs, advances
 * (NULL: FontFace::GetGlyphAdvances in `measuring_mode` under `transform`, which for the GDI modes
 * needs the scaler build), offsets, sideways and bidi_level. Returns 0 and an enumerator, or
 * IDWR_HR_E_NOCOLOR (no colour glyph in the run for that palette: a non-colour font, a palette
 * out of range, glyphs without a COLRv0 base record, COLRv1-only glyphs included), or an error.
 * `face` must outlive the enumerator. */
IDWR_API int32_t idwr_translate_color_glyph_run(idwr_face *face, float baseline_x, float baseline_y,
                                              const idwr_glyph_run *run, uint32_t desired_formats,
                                              int measuring_mode, const idwr_matrix *transform,
                                              uint32_t palette_index, idwr_color_run_enum **out);
/* IDWriteColorGlyphRunEnumerator::MoveNext: the run of the glyphs without colour (those with
 * colour replaced by glyph 1) first when there are any, then one run per COLRv0 layer of each
 * colour glyph in run order, skipping layers whose palette entry has alpha 0. Always 0. */
IDWR_API int32_t idwr_color_run_enum_move_next(idwr_color_run_enum *e, int *has_run);
/* IDWriteColorGlyphRunEnumerator(1)::GetCurrentRun: IDWR_HR_E_NOT_VALID_STATE after the end */
IDWR_API int32_t idwr_color_run_enum_current(const idwr_color_run_enum *e, const idwr_color_glyph_run **run);
IDWR_API void idwr_color_run_enum_destroy(idwr_color_run_enum *e);

/* Font fallback (DirectWrite's system fallback over a font set given as data) */
#include "idwr_fallback.h"

#endif
