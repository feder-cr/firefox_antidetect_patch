/*
 * Colour fonts as DWrite.dll 10.0.26100.9711 exposes them to IDWriteFactory2/4::TranslateColorGlyphRun
 * and IDWriteFontFace2: the COLRv0 layers of a base glyph with their CPAL colours, and the
 * palettes. Ported from:
 *   OpenTypeFontFaceBuilder::ReadFontFace 0x18005e0e0   IsColorFont: COLR and CPAL, or SVG, sbix
 *                                        or CBDT (face flag 0x80)
 *   OpenTypeFontFaceBuilder::ReadColorTables 0x180076aa4  with a CPAL: ColrBuilder; the image
 *                                        formats COLR | COLR_PAINT_TREE (0x104) when COLR is there
 *                                        too (TRUETYPE from glyf, CFF from 'CFF ')
 *   ColrBuilder::ColrBuilder 0x18005a35c, ColrBuilder::WriteRegion 0x18005a790
 *                                        the COLRv0 base and layer records and the CPAL palettes,
 *                                        validated: a violation throws FileFormatException, so
 *                                        CreateFontFace fails with DWRITE_E_FILEFORMAT
 *                                        (idwr_font_check_color_tables); the base glyph map is a
 *                                        FontCmapBuilder (CmapRegionBuilder::Insert 0x180088cb0:
 *                                        a later record for the same glyph replaces an earlier one)
 *   ColorGlyphMap::GetPalette 0x180074a98, DWriteFontFace::GetColorPaletteCount 0x1800ce630,
 *   GetPaletteEntryCount 0x1800f3b50, GetPaletteEntries 0x1800c5970, PaletteEntryToColorF 0x1800c9a5c
 *   DWriteFactory::TranslateColorGlyphRun 0x1800742a0 (IDWriteFactory2: formats TRUETYPE | CFF |
 *                                        COLR), 0x180074310 (IDWriteFactory4: paint level 0),
 *                                        0x180074370 (the checks, IsColorFont, NOCOLOR)
 *   DWriteColorGlyphRunEnumerator::Create 0x180074498, FindColorGlyphs 0x180075528,
 *     InitializeColorGlyphPositions 0x1800741c0, MoveNext 0x1800c9700, AdvanceToBaseRun 0x1800c9af4,
 *     AdvanceToColorRun 0x1800c97bc, GetCurrentRun 0x1800df870
 * Bitmap formats (SVG, PNG, JPEG, TIFF, BGRA: GlyphGraphicData) and the COLRv1 paint tree
 * (IDWriteFactory8) are not ported: a request that would reach them returns IDWR_HR_E_NOTIMPL.
 */
#include <stdlib.h>
#include <string.h>
#include "idwr.h"
#include "idwr_internal.h"
#include "face.h"

#define T(a, b, c, d) IDWR_TAG(a, b, c, d)
#define NOCOLOR IDWR_HR_E_NOCOLOR

static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

/* TableEntry::Exists: a table record with a non-zero offset and length */
static const uint8_t *table(const idwr_font *font, uint32_t tag, uint32_t *len) {
    const uint8_t *p = idwr_font_table(font, tag, len);
    return p && *len && p != font->data ? p : NULL;
}

/* the ColorGlyphRegion of a face (ColrBuilder's output) */
typedef struct {
    int32_t status;         /* 0, or IDWR_HR_E_FILEFORMAT: DWrite refuses the face */
    int built;              /* a CPAL: ColrBuilder ran */
    int is_color;           /* IsColorFont */
    uint32_t formats;       /* GetGlyphImageFormats, the bits idwr models */
    int bitmaps;            /* SVG, sbix or CBDT: GlyphGraphicData (not ported) */
    uint16_t num_entries;   /* numPaletteEntries */
    uint16_t num_palettes;
    uint32_t num_records;   /* numColorRecords */
    const uint8_t *records; /* the CPAL colour records, B G R A */
    uint16_t *palettes;     /* the first record of each palette */
    uint16_t num_base, num_layers;
    uint16_t *base;         /* firstLayerIndex, numLayers per base record */
    uint16_t *layers;       /* glyph id, palette index per layer record */
    uint16_t *map;          /* glyph id -> base record + 1 (0: none), 65536 entries */
} color_tables;

static void tables_free(color_tables *c) {
    if (!c) return;
    free(c->palettes), free(c->base), free(c->layers), free(c->map);
    free(c);
}

static int32_t parse(const idwr_font *font, color_tables *c) {
    uint32_t colr_len = 0, cpal_len = 0, n;
    const uint8_t *colr = table(font, T('C', 'O', 'L', 'R'), &colr_len);
    const uint8_t *cpal = table(font, T('C', 'P', 'A', 'L'), &cpal_len);
    if (table(font, T('g', 'l', 'y', 'f'), &n)) c->formats |= IDWR_IMAGE_TRUETYPE;
    if (table(font, T('C', 'F', 'F', ' '), &n)) c->formats |= IDWR_IMAGE_CFF;
    c->bitmaps = table(font, T('S', 'V', 'G', ' '), &n) || table(font, T('s', 'b', 'i', 'x'), &n) ||
                 table(font, T('C', 'B', 'D', 'T'), &n);
    c->is_color = (colr && cpal) || c->bitmaps;
    if (!cpal) return 0;
    c->built = 1;
    /* ColrBuilder::ColrBuilder: COLR (when present) */
    if (colr) {
        if (colr_len < 14) return IDWR_HR_E_FILEFORMAT;
        uint16_t nb = idwr_u16(colr + 2), nl = idwr_u16(colr + 12);
        uint32_t bo = be32(colr + 4), lo = be32(colr + 8);
        if (bo > colr_len || (colr_len - bo) / 6 < nb) return IDWR_HR_E_FILEFORMAT;
        if (lo > colr_len || (colr_len - lo) / 4 < nl) return IDWR_HR_E_FILEFORMAT;
        c->num_base = nb, c->num_layers = nl;
        c->base = malloc(4 * (size_t)nb + 4);
        c->layers = malloc(4 * (size_t)nl + 4);
        if (!c->base || !c->layers) return IDWR_HR_E_OUTOFMEMORY;
        if (nb) {
            c->map = calloc(65536, sizeof *c->map);
            if (!c->map) return IDWR_HR_E_OUTOFMEMORY;
            for (uint32_t k = 0; k < nb; k++) c->map[idwr_u16(colr + bo + 6 * k)] = (uint16_t)(k + 1);
        }
        for (uint32_t k = 0; k < nb; k++) {
            c->base[2 * k] = idwr_u16(colr + bo + 6 * k + 2);
            c->base[2 * k + 1] = idwr_u16(colr + bo + 6 * k + 4);
        }
        for (uint32_t k = 0; k < nl; k++) {
            c->layers[2 * k] = idwr_u16(colr + lo + 4 * k);
            c->layers[2 * k + 1] = idwr_u16(colr + lo + 4 * k + 2);
        }
    }
    /* CPAL */
    if (cpal_len < 12) return IDWR_HR_E_FILEFORMAT;
    uint16_t ne = idwr_u16(cpal + 2);
    uint32_t cro;
    c->num_entries = ne;
    if ((uint32_t)ne * 4 + 4 == cpal_len) {  /* the short form: the records follow numPaletteEntries */
        c->num_records = ne, cro = 4, c->num_palettes = 1;
        c->palettes = calloc(1, sizeof *c->palettes);
        if (!c->palettes) return IDWR_HR_E_OUTOFMEMORY;
    } else {
        uint16_t np = idwr_u16(cpal + 4);
        if (np == 0) return IDWR_HR_E_FILEFORMAT;
        c->num_records = idwr_u16(cpal + 6), cro = be32(cpal + 8);
        if (c->num_records < ne) return IDWR_HR_E_FILEFORMAT;
        if (12 + 2 * (uint32_t)np > cpal_len) return IDWR_HR_E_FILEFORMAT;  /* CheckedPtr::ReadArrayAt */
        c->num_palettes = np;
        c->palettes = malloc(2 * (size_t)np);
        if (!c->palettes) return IDWR_HR_E_OUTOFMEMORY;
        for (uint32_t k = 0; k < np; k++) {
            uint16_t i = idwr_u16(cpal + 12 + 2 * k);
            if ((int32_t)(c->num_records - ne) < (int32_t)i) return IDWR_HR_E_FILEFORMAT;
            c->palettes[k] = i;
        }
    }
    /* ColrBuilder::WriteRegion */
    if (cro > cpal_len || (cpal_len - cro) / 4 < c->num_records) return IDWR_HR_E_FILEFORMAT;
    c->records = cpal + cro;
    for (uint32_t k = 0; k < c->num_layers; k++) {
        uint16_t p = c->layers[2 * k + 1];
        if (ne <= p && p != 0xffff) return IDWR_HR_E_FILEFORMAT;
    }
    for (uint32_t k = 0; k < c->num_base; k++)
        if (c->base[2 * k + 1] == 0 || c->num_layers < (uint32_t)c->base[2 * k] + c->base[2 * k + 1])
            return IDWR_HR_E_FILEFORMAT;
    if (colr) c->formats |= IDWR_IMAGE_COLR | IDWR_IMAGE_COLR_PAINT_TREE;
    return 0;
}

static color_tables *open_tables(const idwr_font *font) {
    color_tables *c = calloc(1, sizeof *c);
    if (!c) return NULL;
    c->status = parse(font, c);
    if (c->status) c->built = 0, c->num_palettes = 0, c->num_entries = 0, c->num_base = 0, c->num_layers = 0;
    return c;
}

static color_tables *face_tables(idwr_face *f) {
    if (!f->color) f->color = open_tables(f->font);
    return f->color;
}

void idwr_face_color_release(idwr_face *f) {
    tables_free(f->color);
    f->color = NULL;
}

int32_t idwr_font_check_color_tables(const idwr_font *font) {
    if (!font) return IDWR_HR_E_INVALIDARG;
    color_tables *c = open_tables(font);
    if (!c) return IDWR_HR_E_OUTOFMEMORY;
    int32_t hr = c->status;
    tables_free(c);
    return hr;
}

int idwr_face_is_color_font(idwr_face *f) {
    color_tables *c = face_tables(f);
    return c && !c->status && c->is_color;
}

uint32_t idwr_face_get_glyph_image_formats(idwr_face *f) {
    color_tables *c = face_tables(f);
    return c && !c->status ? c->formats : 0;
}

uint32_t idwr_face_get_color_palette_count(idwr_face *f) {
    color_tables *c = face_tables(f);
    return c ? c->num_palettes : 0;
}

uint32_t idwr_face_get_palette_entry_count(idwr_face *f) {
    color_tables *c = face_tables(f);
    return c ? c->num_entries : 0;
}

/* ColorGlyphMap::GetPalette: the palette's first colour record, NULL when out of range */
static const uint8_t *palette(const color_tables *c, uint32_t index) {
    return index < c->num_palettes ? c->records + 4 * (size_t)c->palettes[index] : NULL;
}

/* PaletteEntryToColorF: each byte / 255 as (float)byte * 0x3b808081, 255 exactly 1 */
#ifndef IDWR_NEGATIVE_CONTROL_COLORDIV
#define CHANNEL(v) ((v) == 0xff ? 1.0f : (float)(v) * 0.003921569f)
#else /* negative control (reimpl/colr_check.py --control colordiv): a division, not DWrite's product */
#define CHANNEL(v) ((float)(v) / 255.0f)
#endif
static idwr_color_f color_of(const uint8_t *rec) {
    idwr_color_f o;
    o.b = CHANNEL(rec[0]);
    o.g = CHANNEL(rec[1]);
    o.r = CHANNEL(rec[2]);
    o.a = CHANNEL(rec[3]);
    return o;
}

int32_t idwr_face_get_palette_entries(idwr_face *f, uint32_t palette_index, uint32_t first, uint32_t count,
                                      idwr_color_f *out) {
    color_tables *c = face_tables(f);
    if (!c) return IDWR_HR_E_OUTOFMEMORY;
    const uint8_t *p = palette(c, palette_index);
    if (!p) return NOCOLOR;
    if (c->num_entries < first || c->num_entries - first < count) return IDWR_HR_E_INVALIDARG;
    for (uint32_t k = 0; k < count; k++) out[k] = color_of(p + 4 * (size_t)(first + k));
    return 0;
}

/* ---- the enumerator ---- */

typedef struct {
    int kind;                 /* 0 done, 1 COLRv0 layers */
    uint32_t remaining;       /* layer records left */
    const uint16_t *layer;    /* the next layer record */
    uint32_t index;           /* the glyph's index in the run */
    float x;                  /* the sum of the advances before it */
    float advance;
    idwr_glyph_offset offset;
} color_glyph;

struct idwr_color_run_enum {
    const color_tables *tables;
    const uint8_t *pal;       /* the palette's first record */
    uint32_t glyph_count;
    uint16_t *glyphs;         /* the run's glyphs, then each run's (this+0x20) */
    uint16_t *original;       /* this+0x28 */
    float *advances;          /* this+0x30 */
    idwr_glyph_offset *offsets; /* this+0x38, NULL without offsets */
    uint32_t color_count, current;
    color_glyph *cg;
    float origin_x;
    int state;                /* 0 start, 1 after the base run, 2 in the layers, 3 done */
    uint32_t formats;         /* the formats asked for and available (this+0xbc) */
    int measuring;
    idwr_color_glyph_run run;
};

static void base_run(idwr_color_run_enum *e) {
    for (uint32_t k = 0; k < e->color_count; k++) e->glyphs[e->cg[k].index] = 1;
    if (e->glyph_count == e->color_count) e->run.image_format = 0;
    e->run.palette_index = 0xffff;
    memset(&e->run.color, 0, sizeof e->run.color);
    e->state = 1;
}

static void color_run(idwr_color_run_enum *e) {
    for (;;) {
        if (e->current >= e->color_count) {
            e->run.count = 0;
            e->state = 3;
            return;
        }
        color_glyph *g = &e->cg[e->current];
        if (g->kind != 1) {
            e->current++;
            continue;
        }
        uint16_t glyph = g->layer[0], pal = g->layer[1];
        g->layer += 2;
        if (--g->remaining == 0) g->kind = 0;
#ifndef IDWR_NEGATIVE_CONTROL_TRANSPARENT /* reimpl/colr_check.py --control transparent */
        if (pal < e->tables->num_entries && e->pal[4 * (size_t)pal + 3] == 0) continue;  /* transparent */
#endif
        e->glyphs[0] = glyph;
        e->run.image_format = (e->formats & 3) | IDWR_IMAGE_COLR;
        e->run.measuring_mode = e->measuring;
        e->run.baseline_x = (e->run.bidi_level & 1) ? e->origin_x - g->x : e->origin_x + g->x;
        if (e->offsets) e->offsets[0] = g->offset;
        e->run.count = 1;
        if (pal < e->tables->num_entries) {
            e->run.palette_index = pal;
            e->run.color = color_of(e->pal + 4 * (size_t)pal);
        } else {
            e->run.palette_index = 0xffff;
            memset(&e->run.color, 0, sizeof e->run.color);
        }
        e->state = 2;
        return;
    }
}

int32_t idwr_translate_color_glyph_run(idwr_face *face, float baseline_x, float baseline_y,
                                       const idwr_glyph_run *run, uint32_t desired_formats, int measuring_mode,
                                       const idwr_matrix *transform, uint32_t palette_index,
                                       idwr_color_run_enum **out) {
    if (!out) return IDWR_HR_E_INVALIDARG;
    *out = NULL;
    if (!face || !run || (desired_formats & ~0x1ffu)) return IDWR_HR_E_INVALIDARG;
    color_tables *c = face_tables(face);
    if (!c) return IDWR_HR_E_OUTOFMEMORY;
    if (c->status) return c->status;
    if (!c->is_color) return NOCOLOR;
    /* Create */
    const uint8_t *pal = palette(c, palette_index);
    uint32_t formats = desired_formats & c->formats;
    if (!pal) formats &= ~(uint32_t)(IDWR_IMAGE_COLR | IDWR_IMAGE_COLR_PAINT_TREE);
    if (formats & IDWR_IMAGE_COLR_PAINT_TREE) return IDWR_HR_E_INVALIDARG;  /* paint feature level 0 */
    if ((desired_formats & 0xf8) && c->bitmaps) return IDWR_HR_E_NOTIMPL;
    if (!(formats & 0x1fc)) return NOCOLOR;
    uint32_t n = run->count, colors = 0;
    if (formats & IDWR_IMAGE_COLR)  /* FindColorGlyphs */
        for (uint32_t i = 0; i < n; i++)
            if (c->map && c->map[run->glyphs[i]]) colors++;
    if (!colors) return NOCOLOR;
    idwr_color_run_enum *e = calloc(1, sizeof *e);
    if (!e) return IDWR_HR_E_OUTOFMEMORY;
    e->glyphs = malloc(2 * (size_t)n);
    e->original = malloc(2 * (size_t)n);
    e->advances = malloc(4 * (size_t)n);
    e->cg = calloc(colors, sizeof *e->cg);
    int32_t *design = run->advances ? NULL : malloc(4 * (size_t)n);
    if (run->offsets) e->offsets = malloc(sizeof *e->offsets * (size_t)n);
    if (!e->glyphs || !e->original || !e->advances || !e->cg || (!run->advances && !design) ||
        (run->offsets && !e->offsets)) {
        free(design);
        idwr_color_run_enum_destroy(e);
        return IDWR_HR_E_OUTOFMEMORY;
    }
    e->tables = c, e->pal = pal, e->glyph_count = n, e->color_count = colors;
    for (uint32_t i = 0, k = 0; i < n; i++) {
        uint16_t r = c->map ? c->map[run->glyphs[i]] : 0;
        if (!r) continue;
        color_glyph *g = &e->cg[k++];
        g->kind = 1;
        g->layer = c->layers + 2 * (size_t)c->base[2 * (r - 1)];
        g->remaining = c->base[2 * (r - 1) + 1];
        g->index = i;
    }
    memcpy(e->glyphs, run->glyphs, 2 * (size_t)n);
    memcpy(e->original, run->glyphs, 2 * (size_t)n);
    if (run->offsets) memcpy(e->offsets, run->offsets, sizeof *e->offsets * (size_t)n);
    e->formats = formats;
    e->measuring = measuring_mode;
    if (run->advances) {
        memcpy(e->advances, run->advances, 4 * (size_t)n);
    } else {
        /* FontFace::GetGlyphAdvances(em, 1, transform, measuring, sideways), then design units to
         * DIPs as (float)advance * (em / (float)unitsPerEm) */
        int32_t hr = measuring_mode == IDWR_MEASURING_NATURAL
                         ? idwr_face_get_design_glyph_advances(face, n, run->glyphs, design, run->sideways)
                         : idwr_face_get_gdi_compatible_glyph_advances(face, run->em_size, 1.0f, transform,
                                                                       measuring_mode == IDWR_MEASURING_GDI_NATURAL,
                                                                       run->sideways, n, run->glyphs, design);
        if (hr) {
            free(design);
            idwr_color_run_enum_destroy(e);
            return hr;
        }
        float scale = run->em_size / (float)face->metrics.designUnitsPerEm;
        for (uint32_t i = 0; i < n; i++) e->advances[i] = (float)design[i] * scale;
        free(design);
    }
    /* InitializeColorGlyphPositions */
    float x = 0.0f;
    uint32_t at = 0;
    for (uint32_t k = 0; k < colors; k++) {
        color_glyph *g = &e->cg[k];
        for (; at < g->index; at++) x += e->advances[at];
        g->x = x;
        g->advance = e->advances[g->index];
        if (e->offsets) g->offset = e->offsets[g->index];
    }
    idwr_color_glyph_run *r = &e->run;
    r->em_size = run->em_size;
    r->count = n;
    r->glyphs = e->glyphs;
    r->advances = e->advances;
    r->offsets = e->offsets;
    r->sideways = run->sideways;
    r->bidi_level = run->bidi_level;
    r->baseline_x = baseline_x, r->baseline_y = baseline_y;
    e->origin_x = baseline_x;
    r->image_format = formats & 3;
    r->measuring_mode = measuring_mode;
    *out = e;
    return 0;
}

int32_t idwr_color_run_enum_move_next(idwr_color_run_enum *e, int *has_run) {
    if (!e || !has_run) return IDWR_HR_E_INVALIDARG;
    *has_run = 0;
    if (e->state == 0 && e->color_count < e->glyph_count)
        base_run(e);
    else if (e->state <= 2)
        color_run(e);
    else
        e->run.count = 0;
    *has_run = e->run.count != 0;
    return 0;
}

int32_t idwr_color_run_enum_current(const idwr_color_run_enum *e, const idwr_color_glyph_run **run) {
    if (!e || !run) return IDWR_HR_E_INVALIDARG;
    if (e->run.count) {
        *run = &e->run;
        return 0;
    }
    *run = NULL;
    return IDWR_HR_E_NOT_VALID_STATE;
}

void idwr_color_run_enum_destroy(idwr_color_run_enum *e) {
    if (!e) return;
    free(e->glyphs), free(e->original), free(e->advances), free(e->offsets), free(e->cg);
    free(e);
}
