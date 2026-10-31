/*
 * The CFF/CFF2 rasterizer of the standalone idwr: DWrite.dll 10.0.26100.9711's CffRasterizer
 * (the ports of the reimpl/cff sources, notes/cff.md) on a idwr_font, for the three requests DWrite makes
 * of it:
 *   glyph bitmaps   FontRasterizer::CreateRasterizer picks CffRasterizer for a face whose
 *                   FontFaceReference kind (+0x15) is 0, i.e. one with a 'CFF ' or 'CFF2' table
 *                   (ResolveFontFaceType); GlyphBitmapRasterizationState drives it through its
 *                   vtable (NewTransform, NewGlyph, GetBitmap): idwr/texture.c
 *   design metrics  OpenTypeCffFontMetricsBuilder::GetGlyphMetrics: the rasterizer with
 *                   OpenTypeFontMetricsBuilder::SetDesignTransform, NewGlyph and GetMetrics
 *   outlines        GlyphOutlineRasterizationState: NewTransform(0xc0000, unitsPerEm matrix,
 *                   1, 1, rendering flags, 0), NewGlyph and GetOutline per glyph, the commands
 *                   streamed by idwr/outline.c (StreamGlyphOutlines)
 *
 * The rasterizer is the ported glue (CffRasterizer, its Implementation), reached through
 * idwr_orig like every other port. What the glue needs from the rest of DWrite (OpenTypeFace,
 * FontFragmentPtr, HorizontalMetrics, VerticalMetrics, the HVAR/VVAR tables,
 * ComputeNormalizedAxisCoordinates, CalculateTypoAscenderDescender) is modelled here on the
 * idwr_font as native stand-ins; truetype/native.c lists them by RVA (the IDWR_CFF block of
 * idwr_native_table). Each stand-in keeps only the fields the glue reads at their DWrite offsets
 * (OpenTypeFace + 0x08 head, + 0x13c/+ 0x140 the CFF table, + 0x154/+ 0x158 CFF2, + 0x1c2 the
 * axis count; FontFragmentPtr + 0 data, + 8 size), the rest is private to its natives. Their
 * behaviour is the decompiled originals' (cited per function): the same table reads, the same
 * exceptions (raised through truetype/native.h's catch frame).
 *
 * The FontFaceReference handed to the constructor is a stand-in too: kind 0 at + 0x15, and at
 * + 0x28 (the FontFileReference, which the Implementation keeps at + 0x00) the source: the
 * idwr_font and the instance's normalized coordinates.
 *
 * Negative controls (-DCFF_CONTROL_<NAME>, reimpl/cff_standalone_check.py --control <name>):
 * CFFRASTER_COORDS (the Fixed2_14 coordinates rounded down), CFFRASTER_VMTX (no vertical
 * metrics). The ports' own CFF_CONTROL_* breaks work the same way.
 */
#if defined(IDWR_STANDALONE) && defined(IDWR_CFF)
#include <stdlib.h>
#include <string.h>
#include "idwr_internal.h"
#include "cffraster.h"
#include "../truetype/orig.h"
#include "../truetype/native.h"

#define RVA_CffRasterizer_Ctor 0xb48d4
#define RVA_OTCffMetricsBuilder_GetGlyphMetrics 0xe2340
#define RVA_SetDesignTransform 0xde59c
#define RVA_operator_new 0x117b04

#define AT8(p, o, T) (*(T *)((uint8_t *)(p) + (o)))
#define HR_FILEFORMAT ((int32_t)0x88985000)
#define HR_E_FAIL ((int32_t)0x80004005)
#define MAX_AXES 64

static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

/* ---------------------------------------------------------------------------------------- */
/* the source: what the FontFaceReference stand-in carries (at + 0x28) */

typedef struct {
    const idwr_font *font;
    const float *coords;
    uint16_t axes;
} cff_src;

typedef struct {
    _Alignas(16) uint8_t b[0x70];
} cff_ref;

static void make_ref(cff_ref *r, const idwr_font *font, const float *coords, uint16_t axes) {
    memset(r, 0, sizeof *r);
    r->b[0x15] = 0; /* DWRITE_FONT_FACE_TYPE_CFF: CffRasterizer */
    cff_src s = {font, coords, axes};
    memcpy(r->b + 0x28, &s, sizeof s);
}

static const cff_src *src_of(const void *file) { return (const cff_src *)file; }

/* the table's bytes and its offset in the file */
static const uint8_t *table(const idwr_font *f, uint32_t tag, uint32_t *len, uint32_t *offset) {
    const uint8_t *t = idwr_font_table(f, tag, len);
    if (offset) *offset = t ? (uint32_t)(t - f->data) : 0;
    return t;
}

/* ---------------------------------------------------------------------------------------- */
/* OpenTypeFace (0x1d0 bytes, from the glue's operator new) */

#define FACE_FONT 0x30   /* private: the idwr_font */
#define FACE_COORDS 0x38 /* private: the instance's normalized coordinates */
#define FACE_AXES 0x40   /* private: their count */

static const idwr_font *face_font(const uint8_t *face) { return AT8(face, FACE_FONT, const idwr_font *); }

/* OpenTypeFace::OpenTypeFace(FontFaceReference const &) 0x180092f4c: the table records the glue
 * reads (+ 0x08 the head table, + 0x13c/+ 0x140 and + 0x154/+ 0x158 the CFF and CFF2 records,
 * + 0x1c2 the fvar axis count) */
void *idwr_cffn_OpenTypeFace_ctor(uint8_t *f, const uint8_t *ref) {
    const cff_src *s = src_of(ref + 0x28);
    memset(f, 0, 0x1d0);
    AT8(f, FACE_FONT, const idwr_font *) = s->font;
    AT8(f, FACE_COORDS, const float *) = s->coords;
    AT8(f, FACE_AXES, uint16_t) = s->axes;
    uint32_t len, off;
    AT8(f, 0x08, const uint8_t *) = table(s->font, IDWR_TAG('h', 'e', 'a', 'd'), &len, NULL);
    if (table(s->font, IDWR_TAG('C', 'F', 'F', ' '), &len, &off)) {
        AT8(f, 0x13c, uint32_t) = off;
        AT8(f, 0x140, uint32_t) = len;
    }
    if (table(s->font, IDWR_TAG('C', 'F', 'F', '2'), &len, &off)) {
        AT8(f, 0x154, uint32_t) = off;
        AT8(f, 0x158, uint32_t) = len;
    }
    const uint8_t *fvar = table(s->font, IDWR_TAG('f', 'v', 'a', 'r'), &len, NULL);
    if (fvar && len >= 16) AT8(f, 0x1c2, uint16_t) = idwr_u16(fvar + 8);
    return f;
}

/* OpenTypeTables::~OpenTypeTables 0x180090990: nothing is owned */
void idwr_cffn_OpenTypeTables_dtor(uint8_t *f) { (void)f; }

/* ---------------------------------------------------------------------------------------- */
/* FontFragmentPtr<unsigned char> (0x28 bytes): + 0 the bytes, + 8 the size, + 0x10 private:
 * the source */

/* FontFragmentPtr::FontFragmentPtr(FontFileReference const &) 0x1800b4e44 */
void *idwr_cffn_Fragment_ctor(uint8_t *fr, const void *file) {
    memset(fr, 0, 0x28);
    AT8(fr, 0x10, const void *) = file;
    return fr;
}

/* FontFragmentPtr::Initialize(offset, size) 0x180057aa8: the file's bytes [offset, offset +
 * size); out of the file is a FileFormatException */
void idwr_cffn_Fragment_Initialize(uint8_t *fr, uint64_t offset, uint64_t size) {
    const idwr_font *f = src_of(AT8(fr, 0x10, const void *))->font;
    if (offset > f->size || size > f->size - offset) idwr_raise(HR_FILEFORMAT);
    AT8(fr, 0, const uint8_t *) = f->data + offset;
    AT8(fr, 8, uint64_t) = size;
}

/* FontFragmentPtr::~FontFragmentPtr 0x180057858 */
void idwr_cffn_Fragment_dtor(uint8_t *fr) { (void)fr; }

/* ---------------------------------------------------------------------------------------- */
/* HorizontalMetrics (Implementation + 0x118, 0x40 bytes): + 0 hmtx, + 0x10 u16 the long
 * metrics count, + 0x14 u32 the count of the side bearings after them */

/* HorizontalMetrics::Initialize(OpenTypeTables const &) 0x1800cbd5c: hmtx and hhea's
 * numberOfHMetrics; more long metrics than hmtx holds is a FileFormatException */
void idwr_cffn_HorizontalMetrics_Initialize(uint8_t *hm, const uint8_t *face) {
    const idwr_font *f = face_font(face);
    uint32_t hlen, len;
    const uint8_t *hhea = table(f, IDWR_TAG('h', 'h', 'e', 'a'), &hlen, NULL);
    const uint8_t *hmtx = table(f, IDWR_TAG('h', 'm', 't', 'x'), &len, NULL);
    if (!hmtx || !len || !hhea || hlen < 0x24) return;
    uint16_t n = idwr_u16(hhea + 0x22);
    AT8(hm, 0x10, uint16_t) = n;
    if (n == 0) return;
    if (n > len / 4) idwr_raise(HR_FILEFORMAT);
    AT8(hm, 0, const uint8_t *) = hmtx;
    AT8(hm, 0x14, uint32_t) = (len - 4u * n) >> 1;
}

/* HorizontalMetrics::GetGlyphAdvance(glyph) 0x180033050: the long metric's advance, the last
 * one's after them; past the side bearings an IndexOutOfRangeException (E_FAIL) */
uint16_t idwr_cffn_HorizontalMetrics_GetGlyphAdvance(const uint8_t *hm, uint32_t glyph) {
    uint16_t n = AT8(hm, 0x10, uint16_t);
    const uint8_t *hmtx = AT8(hm, 0, const uint8_t *);
    if (glyph < n) return idwr_u16(hmtx + 4u * glyph);
    if (AT8(hm, 0x14, uint32_t) <= glyph - n) idwr_raise(HR_E_FAIL);
    return idwr_u16(hmtx + 4u * n - 4);
}

/* HorizontalMetrics::~HorizontalMetrics 0x180090abc (VorgTable's too) */
void idwr_cffn_HorizontalMetrics_dtor(uint8_t *hm) { (void)hm; }

/* ---------------------------------------------------------------------------------------- */
/* VerticalMetrics (Implementation + 0x38, 0x48 bytes): + 0 vmtx, + 8 u32 the long metrics
 * count, + 0x10 the top side bearings after them, + 0x18 u32 their count */

/* VerticalMetrics::Initialize(OpenTypeTables const &) 0x1800c6840: with vmtx and vhea,
 * vhea's numOfLongVerMetrics; 0 of them, or more than vmtx holds, is a FileFormatException */
void idwr_cffn_VerticalMetrics_Initialize(uint8_t *vm, const uint8_t *face) {
    const idwr_font *f = face_font(face);
    uint32_t vlen, len;
    const uint8_t *vmtx = table(f, IDWR_TAG('v', 'm', 't', 'x'), &len, NULL);
    const uint8_t *vhea = table(f, IDWR_TAG('v', 'h', 'e', 'a'), &vlen, NULL);
    if (!vmtx || !len || !vhea) return;
    uint16_t n = vlen >= 0x24 ? idwr_u16(vhea + 0x22) : 0;
    if (n == 0 || 4u * n > len) idwr_raise(HR_FILEFORMAT);
    AT8(vm, 0, const uint8_t *) = vmtx;
    AT8(vm, 8, uint32_t) = n;
    AT8(vm, 0x10, const uint8_t *) = vmtx + 4u * n;
    AT8(vm, 0x18, uint32_t) = (len - 4u * n) >> 1;
}

/* VerticalMetrics::TryGetVerticalMetrics(glyph, DesignGlyphMetrics *) 0x18008bab4: the
 * advance height (+ 6) and top side bearing (+ 8), false without vertical metrics or past them */
uint8_t idwr_cffn_VerticalMetrics_TryGet(const uint8_t *vm, uint32_t glyph, uint8_t *out) {
    uint32_t n = AT8(vm, 8, uint32_t);
    const uint8_t *vmtx = AT8(vm, 0, const uint8_t *);
#ifdef CFF_CONTROL_CFFRASTER_VMTX
    n = 0; /* negative control: no vertical metrics (the typographic fallback instead) */
#endif
    if (n == 0) return 0;
    if (glyph < n) {
        AT8(out, 6, uint16_t) = idwr_u16(vmtx + 4u * glyph);
        AT8(out, 8, uint16_t) = idwr_u16(vmtx + 4u * glyph + 2);
        return 1;
    }
    if (AT8(vm, 0x18, uint32_t) + n <= glyph) return 0;
    AT8(out, 6, uint16_t) = idwr_u16(vmtx + 4u * (n - 1));
    AT8(out, 8, uint16_t) = idwr_u16(AT8(vm, 0x10, const uint8_t *) + 2u * (glyph - n));
    return 1;
}

/* VerticalMetrics::~VerticalMetrics 0x180090974 */
void idwr_cffn_VerticalMetrics_dtor(uint8_t *vm) { (void)vm; }

/* ---------------------------------------------------------------------------------------- */
/* HorizontalMetricVariationsTable (0xa0 bytes) and VerticalMetricVariationsTable (0xb0):
 * + 0 the idwr_font, + 8 the tag, + 0xc 1 with an item variation store, + 0x10 the instance's
 * coordinates (a copy), + 0x18 their count */

static void *var_table_ctor(uint8_t *t, const uint8_t *face, uint32_t tag, uint32_t min_len) {
    const idwr_font *f = face_font(face);
    memset(t, 0, 0x20);
    AT8(t, 0, const idwr_font *) = f;
    AT8(t, 8, uint32_t) = tag;
    uint32_t len;
    const uint8_t *v = table(f, tag, &len, NULL);
    if (!v) return t;
    if (len < min_len) idwr_raise(HR_FILEFORMAT); /* OnOutOfRange */
    if (idwr_u16(v) == 1 && be32(v + 4) != 0) AT8(t, 0xc, uint8_t) = 1;
    return t;
}

/* HorizontalMetricVariationsTable(OpenTypeTables const &) 0x18007b17c: HVAR, version 1, an item
 * variation store */
void *idwr_cffn_HvarTable_ctor(uint8_t *t, const uint8_t *face) {
    return var_table_ctor(t, face, IDWR_TAG('H', 'V', 'A', 'R'), 0x14);
}

/* VerticalMetricVariationsTable(OpenTypeTables const &) 0x18007af5c: VVAR */
void *idwr_cffn_VvarTable_ctor(uint8_t *t, const uint8_t *face) {
    return var_table_ctor(t, face, IDWR_TAG('V', 'V', 'A', 'R'), 0x18);
}

/* ItemVariationsTable::SetInstanceCoordinates(array_ref<float const>) 0x180033b40: the region
 * scalars of the instance, when the table has a store */
void idwr_cffn_ItemVariations_SetInstanceCoordinates(uint8_t *t, const float *const *span) {
    if (!AT8(t, 0xc, uint8_t)) return;
    size_t n = (size_t)(span[1] - span[0]);
    float *c = malloc(sizeof *c * (n ? n : 1));
    if (!c) idwr_raise(IDWR_HR_E_OUTOFMEMORY);
    if (n) memcpy(c, span[0], sizeof *c * n);
    free(AT8(t, 0x10, float *));
    AT8(t, 0x10, float *) = c;
    AT8(t, 0x18, uint16_t) = (uint16_t)n;
}

/* ItemVariationsTable::~ItemVariationsTable 0x180090ad8 */
void idwr_cffn_ItemVariations_dtor(uint8_t *t) {
    free(AT8(t, 0x10, float *));
    AT8(t, 0x10, float *) = NULL;
}

static int16_t var_delta(const uint8_t *t, uint32_t map_field, int map_required, uint16_t glyph) {
    if (!AT8(t, 0xc, uint8_t)) return 0;
    return idwr_metric_variation_delta(AT8(t, 0, const idwr_font *), AT8(t, 8, uint32_t), map_field, map_required,
                                      AT8(t, 0x10, const float *), AT8(t, 0x18, uint16_t), glyph);
}

/* HorizontalMetricVariationsTable::GetAdvanceDelta 0x1800ad290 */
int16_t idwr_cffn_Hvar_GetAdvanceDelta(const uint8_t *t, uint32_t glyph) { return var_delta(t, 8, 0, (uint16_t)glyph); }
/* VerticalMetricVariationsTable::GetAdvanceHeightDelta 0x1800c381c */
int16_t idwr_cffn_Vvar_GetAdvanceHeightDelta(const uint8_t *t, uint32_t glyph) {
    return var_delta(t, 8, 0, (uint16_t)glyph);
}
/* VerticalMetricVariationsTable::GetTsbDelta 0x1801403f0: 0 without a tsb mapping */
int16_t idwr_cffn_Vvar_GetTsbDelta(const uint8_t *t, uint32_t glyph) { return var_delta(t, 0xc, 1, (uint16_t)glyph); }

/* ---------------------------------------------------------------------------------------- */
/* ComputeNormalizedAxisCoordinates and CalculateTypoAscenderDescender */

typedef struct {
    uint8_t *first, *last, *end;
} std_vector;

static void vector_resize(std_vector *v, size_t bytes) {
    free(v->first);
    v->first = malloc(bytes ? bytes : 1);
    if (!v->first) idwr_raise(IDWR_HR_E_OUTOFMEMORY);
    v->last = v->end = v->first + bytes;
}

/* ComputeNormalizedAxisCoordinates(face, values, fixed, Fixed2_14, float) 0x18007b8e8: one
 * coordinate per fvar axis, Fixed16_16 normalized (idwr_normalize_axes made them; axes the
 * instance does not set are 0), then float = Fixed16_16::ToFloat and Fixed2_14 =
 * (Fixed16_16 + 2) >> 2. The values and Fixed16_16 vectors stay empty. */
void idwr_cffn_ComputeNormalizedAxisCoordinates(const uint8_t *face, void *values, void *fixed, std_vector *f214,
                                               std_vector *flt) {
    (void)values, (void)fixed;
    uint16_t n = AT8(face, 0x1c2, uint16_t), axes = AT8(face, FACE_AXES, uint16_t);
    const float *c = AT8(face, FACE_COORDS, const float *);
    vector_resize(flt, 4u * n);
    vector_resize(f214, 2u * n);
    for (uint16_t i = 0; i < n; i++) {
        float v = i < axes && c ? c[i] : 0.0f;
        int32_t fx = (int32_t)(v * 65536.0f); /* exact: v is a Fixed16_16 / 65536 */
        ((float *)flt->first)[i] = v;
#ifdef CFF_CONTROL_CFFRASTER_COORDS
        fx -= 2; /* negative control: Fixed2_14 rounded down instead of to nearest */
#endif
        ((int16_t *)f214->first)[i] = (int16_t)((fx + 2) >> 2);
    }
}

/* CalculateTypoAscenderDescender(face, short *, short *) 0x18008b294 (idwr/metrics.c's model) */
void idwr_cffn_CalculateTypoAscenderDescender(const uint8_t *face, int16_t *asc, int16_t *desc) {
    const idwr_font *f = face_font(face);
    uint32_t len;
    if (!table(f, IDWR_TAG('O', 'S', '/', '2'), &len, NULL) && !table(f, IDWR_TAG('h', 'h', 'e', 'a'), &len, NULL))
        idwr_raise(HR_FILEFORMAT);
    int32_t a, d;
    idwr_typo_ascender_descender(f, &a, &d);
    *asc = (int16_t)a;
    *desc = (int16_t)d;
}

/* OpenTypeFontMetricsBuilder::SetDesignTransform(IFontRasterizer *, unitsPerEm) 0x1800de59c:
 * NewTransform(0xc, {upem << 16, 0, 0, upem << 16}, 1, 1, flags 0, ppem 0) */
void idwr_cffn_SetDesignTransform(uint8_t *r, uint32_t upem) {
    int32_t m[4] = {(int32_t)((upem & 0xffff) << 16), 0, 0, (int32_t)((upem & 0xffff) << 16)};
    typedef void new_transform_t(uint8_t *, uint32_t, const int32_t *, uint32_t, uint32_t, uint32_t, uint32_t);
    ((new_transform_t *)AT8(r, 0, void **)[1])(r, 0xc, m, 1, 1, 0, 0);
}

/* ---------------------------------------------------------------------------------------- */
/* the vtables the glue stores: their slots are the ports at the original slots' RVAs */

static const uint32_t vft_cff_rasterizer_rvas[8] = {0xdf8c0, 0xdc260, 0xe22c0, 0x8c3d0,
                                                    0x8bdc0, 0x8ad00, 0x14cc60, 0x14cc40};
void *idwr_cffn_vft_CffRasterizer[8];   /* CffRasterizer::`vftable' 0x18019a780 */
void *idwr_cffn_vft_IFontRasterizer[8]; /* FontRasterizer::IFontRasterizer::`vftable' 0x180199e60 */

static void vtables(void) {
    if (idwr_cffn_vft_CffRasterizer[0]) return;
    for (int i = 0; i < 8; i++) idwr_cffn_vft_CffRasterizer[i] = idwr_orig(vft_cff_rasterizer_rvas[i]);
    idwr_cffn_vft_IFontRasterizer[0] = idwr_orig(0x14cc00); /* the rest: _purecall */
    for (int i = 1; i < 8; i++) idwr_cffn_vft_IFontRasterizer[i] = idwr_orig(0x10def0);
}

/* ---------------------------------------------------------------------------------------- */
/* CffRasterizer */

uint8_t *idwr_cff_rasterizer_create(const idwr_font *font, const float *coords, uint16_t axes) {
    vtables();
    cff_ref ref;
    make_ref(&ref, font, coords, axes);
    /* FontRasterizer::CreateRasterizer: operator new(0x10), CffRasterizer::CffRasterizer */
    uint8_t *r = ORIG(uint8_t *(*)(uint64_t), RVA_operator_new)(0x10);
    return ORIG(uint8_t *(*)(uint8_t *, uint8_t *), RVA_CffRasterizer_Ctor)(r, ref.b);
}

void idwr_cff_rasterizer_destroy(uint8_t *r) {
    if (r) ((void *(*)(uint8_t *, uint32_t))AT8(r, 0, void **)[0])(r, 1);
}

/* ---------------------------------------------------------------------------------------- */
/* design metrics: OpenTypeCffFontMetricsBuilder::GetGlyphMetrics */

typedef struct {
    uint8_t *r;           /* rasterizer with the design transform */
    uint16_t axes;
    float coords[MAX_AXES];
} metrics_rast;

typedef struct {
    metrics_rast slot[2]; /* the default instance, and the last other one */
} font_cache;

static void metrics_rast_free(metrics_rast *m) {
    if (!m->r) return;
    idwr_catch c;
    idwr_catch_push(&c);
    if (!IDWR_SETJMP(c.buf)) idwr_cff_rasterizer_destroy(m->r);
    idwr_catch_pop(&c);
    m->r = NULL;
}

void idwr_cff_font_release(idwr_font *font) {
    font_cache *fc = (font_cache *)font->cff;
    if (!fc) return;
    metrics_rast_free(&fc->slot[0]);
    metrics_rast_free(&fc->slot[1]);
    free(fc);
    font->cff = NULL;
}

/* the builder's rasterizer for the instance (raises) */
static uint8_t *metrics_rasterizer(const idwr_font *font, const float *coords, uint16_t axes) {
    idwr_font *f = (idwr_font *)font; /* the cache only */
    if (!f->cff) {
        f->cff = calloc(1, sizeof(font_cache));
        if (!f->cff) idwr_raise(IDWR_HR_E_OUTOFMEMORY);
    }
    font_cache *fc = (font_cache *)f->cff;
    int instance = 0;
    for (uint16_t i = 0; i < axes; i++) instance |= coords[i] != 0.0f;
    metrics_rast *m = &fc->slot[instance];
    if (m->r && instance && (m->axes != axes || memcmp(m->coords, coords, sizeof *coords * axes) != 0))
        metrics_rast_free(m);
    if (!m->r) {
        if (axes > MAX_AXES) idwr_raise(IDWR_ERR_UNMODELLED);
        m->axes = axes;
        if (axes) memcpy(m->coords, coords, sizeof *coords * axes);
        uint8_t *r = idwr_cff_rasterizer_create(font, coords, axes);
        /* OpenTypeCffFontMetricsBuilder: unitsPerEm from head as the design transform */
        uint16_t upem = font->head && font->head_len >= 20 ? idwr_u16(font->head + 18) : 0;
        if (!font->head) {
            idwr_cff_rasterizer_destroy(r);
            idwr_raise(HR_FILEFORMAT);
        }
        ORIG(void (*)(uint8_t *, uint32_t), RVA_SetDesignTransform)(r, upem);
        m->r = r;
    }
    return m->r;
}

int idwr_cff_design_glyph_metrics(const idwr_font *font, const float *coords, uint16_t axes, uint16_t glyph,
                                 idwr_glyph_metrics *out) {
    _Alignas(16) uint8_t builder[0x460];
    _Alignas(16) uint8_t dgm[16];
    idwr_catch c;
    idwr_catch_push(&c);
    if (IDWR_SETJMP(c.buf)) {
        idwr_catch_pop(&c);
        return 0;
    }
    memset(builder, 0, sizeof builder);
    memset(dgm, 0, sizeof dgm);
    AT8(builder, 0x418, uint8_t *) = metrics_rasterizer(font, coords, axes);
    ORIG(void *(*)(uint8_t *, uint8_t *, uint32_t, uint32_t), RVA_OTCffMetricsBuilder_GetGlyphMetrics)(builder, dgm,
                                                                                                    glyph, 0);
    idwr_catch_pop(&c);
    /* DesignGlyphMetrics {advanceWidth, lsb, rsb, advanceHeight, tsb, bsb, verticalOriginY} */
    out->advanceWidth = AT8(dgm, 0, uint16_t);
    out->leftSideBearing = AT8(dgm, 2, int16_t);
    out->rightSideBearing = AT8(dgm, 4, int16_t);
    out->advanceHeight = AT8(dgm, 6, uint16_t);
    out->topSideBearing = AT8(dgm, 8, int16_t);
    out->bottomSideBearing = AT8(dgm, 10, int16_t);
    out->verticalOriginY = AT8(dgm, 12, int16_t);
    return 1;
}

/* ---------------------------------------------------------------------------------------- */
/* outlines: GlyphOutlineRasterizationState */

struct idwr_cff_outline {
    const idwr_font *font;
    float coords[MAX_AXES];
    uint16_t axes;
    uint32_t simulations;
    uint8_t *r;
    int last_error;
};

void idwr_cff_outline_close(idwr_cff_outline *o) {
    if (!o) return;
    if (o->r) {
        idwr_catch c;
        idwr_catch_push(&c);
        if (!IDWR_SETJMP(c.buf)) idwr_cff_rasterizer_destroy(o->r);
        idwr_catch_pop(&c);
    }
    free(o);
}

idwr_cff_outline *idwr_cff_outline_open(const idwr_font *font, const float *coords, uint16_t axes,
                                      uint32_t simulations, int *error) {
    if (error) *error = -1;
    if (!font || axes > MAX_AXES || (axes && !coords) || !font->head || font->head_len < 54) return NULL;
    idwr_cff_outline *o = calloc(1, sizeof *o);
    if (!o) return NULL;
    o->font = font;
    o->axes = axes;
    o->simulations = simulations;
    if (axes) memcpy(o->coords, coords, sizeof *coords * axes);
    idwr_catch c;
    idwr_catch_push(&c);
    if (IDWR_SETJMP(c.buf)) {
        idwr_catch_pop(&c);
        if (error) *error = c.code;
        idwr_cff_outline_close(o);
        return NULL;
    }
    o->r = idwr_cff_rasterizer_create(font, o->coords, axes);
    /* GlyphOutlineRasterizationState: NewTransform(0xc0000, {upem << 16, 0, 0, upem << 16},
     * 1, 1, MakeRasterizerFlagsForOutlineRendering, 0) */
    uint32_t upem = idwr_u16(font->head + 18);
    int32_t m[4] = {(int32_t)(upem << 16), 0, 0, (int32_t)(upem << 16)};
    uint32_t flags = (simulations & IDWR_SIM_BOLD ? 2u : 0u) | (simulations & IDWR_SIM_OBLIQUE ? 4u : 0u);
    typedef void new_transform_t(uint8_t *, uint32_t, const int32_t *, uint32_t, uint32_t, uint32_t, uint32_t);
    ((new_transform_t *)AT8(o->r, 0, void **)[1])(o->r, 0xc0000, m, 1, 1, flags, 0);
    idwr_catch_pop(&c);
    if (error) *error = 0;
    return o;
}

int idwr_cff_outline_last_error(const idwr_cff_outline *o) { return o ? o->last_error : 0; }

/* GlyphOutlineRasterizationState::RasterizeGlyph 0x1800901e0: NewGlyph, GetOutline into
 * {count, words}; its catch leaves the outline empty (a FileFormatException of the glyph) */
static int outline_commands(void *ctx, uint16_t glyph, const int32_t **words, uint32_t *count) {
    idwr_cff_outline *o = (idwr_cff_outline *)ctx;
    *words = NULL;
    *count = 0;
    o->last_error = 0;
    struct {
        uint32_t count;
        uint32_t pad;
        const int32_t *words;
    } go = {0, 0, NULL};
    idwr_catch c;
    idwr_catch_push(&c);
    if (IDWR_SETJMP(c.buf)) {
        idwr_catch_pop(&c);
        o->last_error = c.code;
        return c.code == HR_FILEFORMAT ? 0 : -1;
    }
    void **vt = AT8(o->r, 0, void **);
    ((void (*)(uint8_t *, uint32_t, uint32_t))vt[2])(o->r, glyph, 0);
    ((void (*)(uint8_t *, void *))vt[5])(o->r, &go);
    idwr_catch_pop(&c);
    *words = go.words;
    *count = go.count;
    return 0;
}

int idwr_cff_outline_run(idwr_cff_outline *o, float em_size, const uint16_t *glyphs, const float *advances,
                        const idwr_glyph_offset *offsets, uint32_t count, int sideways, int right_to_left,
                        idwr_path_cmd *out, int capacity) {
    if (!o) return -1;
    return idwr_glyph_run_outline_commands(o->font, o->axes ? o->coords : NULL, o->axes, (int)o->simulations, em_size,
                                          glyphs, advances, offsets, count, sideways, right_to_left, outline_commands,
                                          o, out, capacity);
}

#endif /* IDWR_STANDALONE && IDWR_CFF */
