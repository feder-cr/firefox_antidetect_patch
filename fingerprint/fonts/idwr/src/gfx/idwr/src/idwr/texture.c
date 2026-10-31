/*
 * IDWriteGlyphRunAnalysis::GetAlphaTextureBounds and ::CreateAlphaTexture (DWrite.dll
 * 10.0.26100.9711) without DWrite: the glyph run parameters and positions of idwr/glyphrun.c,
 * each glyph rasterized by the ported TrueType scaler (idwr/ttraster.c) or drawn from its
 * embedded strike (idwr/sbit.c), packed by the ported GlyphBitmapRasterizationState and
 * CachedBitmapWriter (truetype/glyphrun_layout.c), and the texture built by the ported
 * DWriteGlyphRunAnalysis::CreateAlphaTexture (truetype/glyphrun_texture.c) on a
 * GlyphRunAnalysis-shaped block. Built with the scaler only (-DIDWR_STANDALONE); the
 * originals those ports still name resolve to native helpers (truetype/native.c), so nothing
 * here calls into DWrite.
 *
 * What GlyphRunAnalysis::GlyphRunAnalysis does, and where it is done here:
 *   1. the run parameters, the font matrix, RenderingFlags, the cache key and the glyph
 *      origins: idwr_glyph_run_analyze;
 *   2. GetGlyphData: one GlyphBitmapRasterizationState (the ported constructor, on a FontFace
 *      stand-in, native.h) whose rasterizer is a idwr_tt; each distinct glyph (ids past the
 *      glyph count become 0) rasterized and packed into a GlyphBitmapLayout;
 *   3. positions, the embedded-bitmap partition and the run rects: idwr_glyph_run_place;
 *   4. GetGlyphBitmaps for the odd 2x2 phase: the slots idwr_glyph_run_place moved first are
 *      rasterized again by a second state with RenderingFlags | 0x100.
 * Exceptions of the original (FileFormatException from the scaler, ArgumentException from the
 * texture code, SafeInt traps) come back as the HRESULT the COM method returns.
 *
 * The rasterizer is the one FontRasterizer::CreateRasterizer picks by the face's kind: the
 * TrueType scaler (idwr_tt) for glyf outlines, CffRasterizer (idwr/cffraster.c: the ported glue
 * and Adobe rasterizer) for a face with a 'CFF ' or 'CFF2' table. The CFF one needs a build with
 * the CFF ports (-DIDWR_CFF, the reimpl/cff sources); without them CFF faces give E_NOTIMPL.
 */
#ifdef IDWR_STANDALONE
#include <stdlib.h>
#include <string.h>
#include "idwr_internal.h"
#include "ttraster.h"
#include "../truetype/orig.h"
#include "../truetype/native.h"
#ifdef IDWR_CFF
#include "cffraster.h"
#endif

#define RVA_State_ctor                   0x53824
#define RVA_State_RasterizeGlyph         0x5225c
#define RVA_State_GetCachedGlyphDataSize 0x19910
#define RVA_State_FillGlyphData          0x19934
#define RVA_DWriteCreateAlphaTexture     0x15dd0

#define AT8(p, o, T) (*(T *)((uint8_t *)(p) + (o)))
#define MAX_AXES 64

struct idwr_glyph_run_texture {
    idwr_glyph_run_analysis a;
    uint32_t count;
    const uint8_t **layouts;   /* per slot (DWrite's order) */
    int32_t *positions;        /* per slot: x, y */
    uint8_t **owned;           /* every layout allocated */
    uint32_t nowned, capowned;
    _Alignas(16) uint8_t block[0xc0];   /* DWriteGlyphRunAnalysis: vtable slot, GlyphRunAnalysis */
};

/* ---- the IFontRasterizer a GlyphBitmapRasterizationState drives ---- */

typedef struct {
    void **vtbl;
    idwr_tt *tt;
    uint16_t glyph;   /* NewGlyph's glyph, rasterized by GetBitmap */
} rasterizer;

typedef struct {
    const idwr_font *font;
    const float *coords;
    uint16_t axes;
    int failed;
} rasterizer_ctx;

#ifdef IDWR_CFF
/* FontRasterizer::CreateRasterizer for a CFF face: CffRasterizer, whose vtable starts with the
 * same four slots */
static void *create_cff_rasterizer(void *p) {
    rasterizer_ctx *c = (rasterizer_ctx *)p;
    return idwr_cff_rasterizer_create(c->font, c->coords, c->axes);
}
#endif

static void r_destroy(rasterizer *r, uint32_t flags) {
    (void)flags;
    idwr_tt_close(r->tt);
    free(r);
}
static void r_new_transform(rasterizer *r, int32_t point_size, uint8_t *params, uint16_t ox, uint16_t oy,
                            uint32_t flags, uint8_t ppem) {
    idwr_tt_transform(r->tt, (uint32_t)point_size, (const int32_t *)params, ox, oy, flags, ppem);
}
static void r_new_glyph(rasterizer *r, uint32_t glyph, uint8_t fullwidth) {
    (void)fullwidth;   /* the full-width flag only matters to the hdmx advance widths */
    r->glyph = (uint16_t)glyph;
}
static void r_get_bitmap(rasterizer *r, uint8_t *bm) { idwr_tt_bitmap_get(r->tt, r->glyph, (idwr_tt_bitmap *)bm); }

static void *rasterizer_vtbl[4] = { (void *)r_destroy, (void *)r_new_transform, (void *)r_new_glyph,
                                    (void *)r_get_bitmap };

/* FontRasterizer::CreateRasterizer for the stand-in face */
static void *create_rasterizer(void *p) {
    rasterizer_ctx *c = (rasterizer_ctx *)p;
    rasterizer *r = calloc(1, sizeof *r);
    if (!r) idwr_raise(IDWR_HR_E_OUTOFMEMORY);
    r->vtbl = rasterizer_vtbl;
    r->tt = idwr_tt_open(c->font, c->coords, c->axes);
    if (!r->tt) {
        free(r);
        idwr_raise(IDWR_HR_E_FILEFORMAT);   /* Initialize: fs_NewSfnt failed */
    }
    return r;
}

/* ---- GlyphBitmapRasterizationState (0x60 bytes): the ported constructor, glyph and layout ---- */

typedef struct {
    _Alignas(16) uint8_t state[0x60];
    int live;
} raster_state;

static void state_open(raster_state *s, uint8_t *face, const idwr_raster_params *params, uint32_t extra_flags) {
    uint32_t p[8];   /* by value: the constructor may change the overscale */
    memcpy(p, params, sizeof p);
    p[6] |= extra_flags;
    memset(s->state, 0, sizeof s->state);
    s->live = 1;
    ORIG(uint8_t *(*)(uint8_t *, void *, uint8_t *, uint8_t *), RVA_State_ctor)(s->state, NULL, face, (uint8_t *)p);
}

static void state_close(raster_state *s) {
    if (!s->live) return;
    s->live = 0;
    free(AT8(s->state, 0x10, void *));   /* the writer's run vector (native allocator) */
    void **r = AT8(s->state, 0, void **);
    if (r) ((void (*)(void *, uint32_t))((void **)r[0])[0])(r, 1);   /* the deleting destructor */
}

static int keep_layout(idwr_glyph_run_texture *t, uint8_t *l) {
    if (t->nowned == t->capowned) {
        uint32_t n = t->capowned ? 2 * t->capowned : 16;
        uint8_t **p = realloc(t->owned, n * sizeof *p);
        if (!p) return 0;
        t->owned = p;
        t->capowned = n;
    }
    t->owned[t->nowned++] = l;
    return 1;
}

/* RasterizeGlyph, then GetCachedGlyphDataSize and FillGlyphData into a new layout */
static const uint8_t *state_layout(idwr_glyph_run_texture *t, raster_state *s, uint16_t glyph) {
    ORIG(void (*)(uint8_t *, uint32_t, uint8_t), RVA_State_RasterizeGlyph)(s->state, glyph, 0);
    uint32_t n = ORIG(uint32_t (*)(uint8_t *), RVA_State_GetCachedGlyphDataSize)(s->state);
    uint8_t *l = calloc(1, n);
    if (!l || !keep_layout(t, l)) {
        free(l);
        idwr_raise(IDWR_HR_E_OUTOFMEMORY);
    }
    ORIG(void (*)(uint8_t *, uint8_t *, uint32_t), RVA_State_FillGlyphData)(s->state, l, 0);
    return l;
}

/* ---- the API ---- */

void idwr_glyph_run_texture_destroy(idwr_glyph_run_texture *t) {
    if (!t) return;
    for (uint32_t i = 0; i < t->nowned; i++) free(t->owned[i]);
    free(t->owned);
    free(t->layouts);
    free(t->positions);
    free(AT8(t->block, 8 + 0x70, void *));   /* the run bitmap vector (native allocator) */
    free(t);
}

typedef struct {
    float coords[MAX_AXES];
    uint8_t face_data[0x68];
    _Alignas(16) uint8_t face[0xa8];
    rasterizer_ctx rctx;
    raster_state s0, s1;
    const uint8_t **by_glyph;
    float *origins;
    idwr_glyph_layout *lays;
    uint32_t *order;
} build;

static int32_t build_run(idwr_glyph_run_texture *t, build *b, const idwr_font *font, const idwr_axis_value *axis_values,
                         uint32_t axis_count, const idwr_glyph_run *run) {
    uint32_t n = run->count;
    /* the instance's gasp ranges (MVAR gsp deltas) decide the grid fit */
    int vaxes = idwr_normalize_axes(font, axis_values, axis_count, b->coords, MAX_AXES);
    int r = idwr_glyph_run_analyze_instance(font, b->coords, (uint16_t)(vaxes > 0 ? vaxes : 0), run, &t->a, b->origins);
    if (r == IDWR_E_INVALIDARG) return IDWR_HR_E_INVALIDARG;
    if (r != 0) return r;
    if (n == 0) {
        idwr_glyph_run_place(&t->a, b->lays, b->origins, b->order, t->positions);
        return 0;
    }
    int axes = idwr_normalize_axes(font, axis_values, axis_count, b->coords, MAX_AXES);
#ifdef IDWR_NEGATIVE_CONTROL_VARIATION
    for (int i = 0; i < axes; i++) b->coords[i] = 0.0f;   /* texture_check.py --control variation */
#endif
    if (axes < 0) return IDWR_HR_E_INVALIDARG;
    uint16_t nglyphs = idwr_font_glyph_count(font);
    if (nglyphs == 0) return IDWR_HR_E_INVALIDARG;   /* ConstrainGlyphIndex throws */

    /* the FontFace stand-in (native.h) */
    int weight = idwr_face_weight(font, axis_values, axis_count);   /* ReadFontFace's (nameweight.c) */
    AT8(b->face_data, 0x58, uint16_t) = (uint16_t)(weight < 0 ? idwr_font_weight(font) : weight);
    AT8(b->face_data, 0x60, uint8_t) = (uint8_t)(idwr_font_is_thin_family(font) ? 4 : 0);
    AT8(b->face_data, 0x62, uint16_t) = nglyphs;
    AT8(b->face, 0, uint8_t *) = b->face_data;
    AT8(b->face, 0x36, uint8_t) = (uint8_t)(run->simulations & 1);
    b->rctx.font = font;
    b->rctx.coords = b->coords;
    b->rctx.axes = (uint16_t)axes;
    AT8(b->face, 0x98, void *) = (void *)create_rasterizer;
#ifdef IDWR_CFF
    if (idwr_font_is_cff(font)) AT8(b->face, 0x98, void *) = (void *)create_cff_rasterizer;
#endif
    AT8(b->face, 0xa0, void *) = &b->rctx;

    /* GetGlyphData: each distinct glyph once, in run order */
    b->by_glyph = calloc(nglyphs, sizeof *b->by_glyph);
    if (!b->by_glyph) return IDWR_HR_E_OUTOFMEMORY;
    state_open(&b->s0, b->face, &t->a.raster, 0);
    const uint8_t **run_layouts = t->layouts;   /* run order first, slot order after placing */
    for (uint32_t i = 0; i < n; i++) {
        uint16_t g = run->glyphs[i] < nglyphs ? run->glyphs[i] : 0;
        if (!b->by_glyph[g]) b->by_glyph[g] = state_layout(t, &b->s0, g);
        run_layouts[i] = b->by_glyph[g];
        uint32_t h = AT8(b->by_glyph[g], 0, uint32_t);
        b->lays[i].flags = h >> 26;
        b->lays[i].left = AT8(b->by_glyph[g], 4, int16_t);
        b->lays[i].top = AT8(b->by_glyph[g], 6, int16_t);
        b->lays[i].width = AT8(b->by_glyph[g], 8, uint16_t);
        b->lays[i].height = AT8(b->by_glyph[g], 10, uint16_t);
    }
    state_close(&b->s0);

    idwr_glyph_run_place(&t->a, b->lays, b->origins, b->order, t->positions);
    const uint8_t **slot = malloc(sizeof *slot * n);
    if (!slot) return IDWR_HR_E_OUTOFMEMORY;
    for (uint32_t i = 0; i < n; i++) slot[i] = run_layouts[b->order[i]];
    memcpy(t->layouts, slot, sizeof *slot * n);
    free(slot);

    /* GetGlyphBitmaps: the odd 2x2 phase */
#ifdef IDWR_NEGATIVE_CONTROL_ODDPHASE
    if (0) {   /* texture_check.py --control oddphase */
#else
    if (t->a.odd_count) {
#endif
        memset(b->by_glyph, 0, sizeof *b->by_glyph * nglyphs);
        state_open(&b->s1, b->face, &t->a.raster, 0x100);
        for (uint32_t i = 0; i < t->a.odd_count; i++) {
            uint16_t g0 = run->glyphs[b->order[i]];
            uint16_t g = g0 < nglyphs ? g0 : 0;
            if (!b->by_glyph[g]) b->by_glyph[g] = state_layout(t, &b->s1, g);
            t->layouts[i] = b->by_glyph[g];
        }
        state_close(&b->s1);
    }
    return 0;
}

int32_t idwr_glyph_run_texture_create(const idwr_font *font, const idwr_axis_value *axis_values, uint32_t axis_count,
                                     const idwr_glyph_run *run, idwr_glyph_run_texture **out) {
    *out = NULL;
    if (!font || !run || !run->glyphs) return IDWR_HR_E_INVALIDARG;
    /* The argument checks of DWriteFactory::CreateGlyphRunAnalysis, before any work: IDWriteFactory's
     * 0x180049080 (pixelsPerDip, mode <= 6), IDWriteFactory2's 0x180049490 (mode <= 6) and IDWriteFactory3's
     * 0x1800480b0 (mode <= 7; keep_mode 1). comiss / jc: !(emSize >= 0), so -0 passes and NaN fails;
     * comiss / jbe: !(pixelsPerDip > 0) (the later overloads pass 1); a NaN baseline origin; a NaN entry of
     * the transform (IsValidTransform 0x18004a104); grid fit, antialias and measuring out of range. */
#ifndef IDWR_NEGATIVE_CONTROL_FACTORYARGS
    if (!(run->em_size >= 0.0f) || !(run->pixels_per_dip > 0.0f) || isnan(run->origin_x) || isnan(run->origin_y))
        return IDWR_HR_E_INVALIDARG;
#endif
    if (run->transform) {
        const idwr_matrix *m = run->transform;
        if (isnan(m->m11) || isnan(m->m12) || isnan(m->m21) || isnan(m->m22) || isnan(m->dx) || isnan(m->dy))
            return IDWR_HR_E_INVALIDARG;
    }
    if ((unsigned)run->rendering_mode > (run->keep_mode ? 7u : 6u) || (unsigned)run->grid_fit > 2u ||
        (unsigned)run->antialias > 1u || (unsigned)run->measuring_mode > 2u)
        return IDWR_HR_E_INVALIDARG;
#ifdef IDWR_CFF
    /* CFF and CFF2 outlines go through CffRasterizer (idwr/cffraster.c) */
    if (!idwr_font_is_cff(font) && !idwr_font_has_table(font, IDWR_TAG('g', 'l', 'y', 'f')))
        return IDWR_HR_E_NOTIMPL;
#else
    /* CFF outlines go through DWrite's CffRasterizer, which this build does not include */
    if (!idwr_font_has_table(font, IDWR_TAG('g', 'l', 'y', 'f')) || idwr_font_is_cff(font))
        return IDWR_HR_E_NOTIMPL;
#endif
    uint32_t n = run->count;
    idwr_glyph_run_texture *t = calloc(1, sizeof *t);
    build *b = calloc(1, sizeof *b);
    if (t && b) {
        t->count = n;
        t->layouts = calloc(n ? n : 1, sizeof *t->layouts);
        t->positions = calloc(2 * (size_t)(n ? n : 1), sizeof *t->positions);
        b->origins = calloc(2 * (size_t)(n ? n : 1), sizeof *b->origins);
        b->lays = calloc(n ? n : 1, sizeof *b->lays);
        b->order = calloc(n ? n : 1, sizeof *b->order);
    }
    if (!t || !b || !t->layouts || !t->positions || !b->origins || !b->lays || !b->order) {
        if (b) free(b->origins), free(b->lays), free(b->order);
        free(b);
        idwr_glyph_run_texture_destroy(t);
        return IDWR_HR_E_OUTOFMEMORY;
    }
    int32_t hr;
    idwr_catch c;
    idwr_catch_push(&c);
    if (IDWR_SETJMP(c.buf)) {
        idwr_catch_pop(&c);
        hr = c.code;
    } else {
        hr = build_run(t, b, font, axis_values, axis_count, run);
        idwr_catch_pop(&c);
    }
    state_close(&b->s0);
    state_close(&b->s1);
    free(b->by_glyph);
    free(b->origins);
    free(b->lays);
    free(b->order);
    free(b);
    if (hr != 0) {
        idwr_glyph_run_texture_destroy(t);
        return hr;
    }
    /* the GlyphRunAnalysis fields CreateAlphaTexture reads */
    uint8_t *g = t->block + 8;
    AT8(g, 0x28, uint32_t) = n;
    AT8(g, 0x30, const uint8_t **) = t->layouts;
    AT8(g, 0x38, int32_t *) = t->positions;
    AT8(g, 0x40, void *) = NULL;
    AT8(g, 0x48, uint32_t) = t->a.outline_count;
    memcpy(g + 0x4c, t->a.rect, 16);
    memcpy(g + 0x5c, t->a.aliased_rect, 16);
    AT8(g, 0x88, uint32_t) = t->a.attributes;
    AT8(g, 0x8c, uint32_t) = t->a.antialias;
    AT8(g, 0x90, uint8_t) = t->a.grayscale;
    AT8(g, 0x91, uint8_t) = t->a.thin_family;
    *out = t;
    return 0;
}

int32_t idwr_glyph_run_texture_bounds(const idwr_glyph_run_texture *t, int texture_type, int32_t rect[4]) {
    if (!t || !rect) return IDWR_HR_E_INVALIDARG;
    return idwr_glyph_run_alpha_texture_bounds(&t->a, texture_type, rect) ? IDWR_HR_E_INVALIDARG : 0;
}

int32_t idwr_glyph_run_texture_render(idwr_glyph_run_texture *t, int texture_type, const int32_t rect[4],
                                     uint8_t *buffer, uint32_t size) {
    if (!t) return IDWR_HR_E_INVALIDARG;
    int32_t r[4] = { 0, 0, 0, 0 };
    if (rect) memcpy(r, rect, sizeof r);
    int32_t hr;
    idwr_catch c;
    idwr_catch_push(&c);
    if (IDWR_SETJMP(c.buf)) {
        idwr_catch_pop(&c);
        return c.code;
    }
    hr = (int32_t)ORIG(long (*)(void *, uint32_t, void *, uint8_t *, uint32_t), RVA_DWriteCreateAlphaTexture)(
        t->block, (uint32_t)texture_type, rect ? (void *)r : NULL, buffer, size);
    idwr_catch_pop(&c);
    return hr;
}

const idwr_glyph_run_analysis *idwr_glyph_run_texture_analysis(const idwr_glyph_run_texture *t) { return &t->a; }

int idwr_glyph_run_texture_slot(const idwr_glyph_run_texture *t, uint32_t slot, const uint8_t **layout,
                               int32_t position[2]) {
    if (!t || slot >= t->count) return -1;
    if (layout) *layout = t->layouts[slot];
    if (position) position[0] = t->positions[2 * slot], position[1] = t->positions[2 * slot + 1];
    return 0;
}

#endif /* IDWR_STANDALONE */
