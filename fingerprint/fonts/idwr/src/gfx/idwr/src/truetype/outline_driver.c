/*
 * The outline driver: IDWriteFontFace::GetGlyphRunOutline's points from the TrueType scaler,
 * set up the way DWrite.dll 10.0.26100.9711 sets it up, and the public idwr API over it.
 *
 *   GlyphOutlineRasterizationState::GlyphOutlineRasterizationState 0x180090b74
 *       CreateRasterizer for the face (TrueTypeRasterizer::Implementation, Initialize), then
 *       NewTransform(0xc0000, {upem << 16, 0, 0, upem << 16}, 1, 1, flags, ppem 0)
 *   MakeRasterizerFlagsForOutlineRendering 0x18008f3ac
 *       flags = (bold ? 2 : 0) | (oblique ? 4 + 8 * sideways : 0)
 *   GlyphOutlineRasterizationState::RasterizeGlyph 0x1800901e0  NewGlyph, GetOutline
 *   TrueTypeRasterizer::Implementation::GetOutline 0x1800ccb70
 *       fs_NewGlyph with input +0x90 = 0xffff, +0x92 = glyph, +0x94 = 0, +0x95 = 1;
 *       input +0x90 (byte) = 0, +0x80 (trace) = NULL; fs__Contour(in, info, 1), and on an
 *       error fs__Contour(in, info, 0); the points are info +0x78 / +0x80 (x / y), +0x98
 *       (on-curve, any non-zero byte), +0x90 (contour end points), +0x72 (contour count)
 *
 * What NewTransform makes of that (TrueTypeRasterizer::Implementation::NewTransform
 * 0x1800091bc): the point size at input +0x90 is 0xc0000 << 16, which wraps to 0 in 32 bits;
 * the matrix is the unitsPerEm diagonal at 96 dpi (input +0x94/+0x96 = 0x60); no overscale,
 * so the scan control word is 0; bold sets the embolden amounts +0xc2/+0xc4 to 0x14; the
 * RenderingFlags have no grid-fit bit (bit 0), so input +0xd6 = 1. The scaler then works at
 * ppem = unitsPerEm and the glyph programs still run, which is why some faces come back with
 * fractional font units. Oblique adds FixMul(m, 0x5700) to the second column (or subtracts
 * from the first column when sideways).
 *
 * The variation callbacks are the idwr ports (idwr_apply_gvar, idwr_apply_cvar), with the 2.14
 * coordinates ComputeNormalizedAxisCoordinates 0x18007b8e8 stores at Implementation + 0x2b8:
 * (fixed16 + 2) >> 2.
 *
 * Every fs_* call goes through idwr_orig: built with -DIDWR_STANDALONE it is the port and
 * DWrite.dll is never loaded; in oracle mode it is DWrite.dll's entry, which the oracle's
 * function swap may redirect to the port (reimpl/oracle/outline_oracle.c). Each call into the
 * scaler arms a catch frame for its SafeInt traps (native.h), which then return their
 * IDWR_ERR_SAFEINT_* code as an fs error. Portable C: no platform headers.
 */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "tt_state.h"
#include "orig.h"
#include "native.h"
#include "outline_driver.h"
#include "../idwr/idwr.h"
#include "../idwr/idwr_internal.h"
#if defined(IDWR_STANDALONE) && defined(IDWR_CFF)
#include "../idwr/cffraster.h" /* CFF faces: CffRasterizer (idwr/cffraster.c) */
#endif

/* RVAs, DWrite.dll 10.0.26100.9711 (analysis/publics/DWrite-10.0.26100.9711.tsv) */
#define RVA_fs_NewSfnt            0x9168c
#define RVA_fs__NewTransformation 0x09804
#define RVA_fs_NewGlyph           0x6c388
#define RVA_fs__Contour           0x0c8f0

/* the types of the ports' definitions (reimpl/abi_check.py) */
typedef int32_t (*fs2_fn)(uint8_t *input, uint8_t *info);
typedef int32_t (*newtrans_fn)(uint8_t *input, uint8_t *info, uint64_t mode);
typedef int (*contour_fn)(uint8_t *input, uint8_t *info, char hint);

/* Sizes from TrueTypeRasterizer::Implementation::Implementation (operator new 0xe0 / 0x100) */
#define INPUT_SIZE 0xe0
#define INFO_SIZE  0x100
#define BASE(in, i)  (*(void **)((uint8_t *)(in) + 8 + 8 * (i)))
#define SIZE(inf, i) (*(int32_t *)((uint8_t *)(inf) + 4 * (i)))
#define MAX_AXES 64

struct tt_outline {
    const uint8_t *font;
    size_t font_len;
    uint32_t face_offset;        /* Implementation + 0x248: the face's table directory */
    uint8_t *input, *info;
    uint8_t skip;                /* Implementation + 0x240: fs__NewTransformation returned 0x1901 */
    uint16_t upem;
    int perturb;
    const idwr_font *dfont;       /* the idwr view of the face, for gvar and cvar */
    uint16_t axes;
    float coords[MAX_AXES];      /* Implementation + 0x2a0 */
    int16_t coords214[MAX_AXES]; /* Implementation + 0x2b8 */
    /* GetOutline's result, valid until the next glyph */
    uint16_t *ends;
    uint32_t cap_contours;
};

/* GetSfntFragment(clientID, offset, length, &context): offset 0 is the face's table directory */
static const void *get_fragment(void *client, int offset, int length, void **ctx) {
    tt_outline *o = (tt_outline *)client;
    size_t off = offset == 0 ? o->face_offset : (size_t)(uint32_t)offset;
    *ctx = NULL;
    if (off > o->font_len || (size_t)(uint32_t)length > o->font_len - off) return NULL;
    return o->font + off;
}
static void release_fragment(void *ctx) { (void)ctx; }

/* TrueTypeRasterizer::Implementation::ApplyOutlineVariation 0x8230: nothing without
 * coordinates, otherwise GlyphOutlineVariationInterpolator::ApplyVariation (idwr_apply_gvar) */
static void apply_outline_variation(void *c, uint16_t g, int16_t *e, uint16_t n, int32_t *x, int32_t *y, uint32_t m) {
    tt_outline *o = (tt_outline *)c;
    if (!o || !o->axes || !o->dfont) return;
    idwr_apply_gvar(o->dfont, o->coords, o->axes, g, e, n, x, y, m);
}
/* ApplyCVTVariation 0x76a0 */
static void apply_cvt_variation(void *c, int32_t *cvt, uint32_t n) {
    tt_outline *o = (tt_outline *)c;
    if (!o || !o->axes || !o->dfont) return;
    idwr_apply_cvar(o->dfont, o->coords, o->axes, cvt, n);
}
/* GetVariationAxisCoordinates 0x1800d6ab0: the 2.14 vector when the count matches, else zeros */
static void get_variation_axis_coordinates(void *c, uint16_t *coords, uint16_t n) {
    tt_outline *o = (tt_outline *)c;
    if (!o) return;
    for (uint16_t i = 0; i < n; i++) coords[i] = n == o->axes ? (uint16_t)o->coords214[i] : 0;
}

static int32_t fix_mul(int32_t a, int32_t b) { /* DWRITE_FixMul 0x180067b70 */
    int64_t p = (int64_t)a * b;
    int64_t r = (p + (p >> 63) + 0x8000) >> 16;
    if (r > INT32_MAX) return INT32_MAX;
    if (r < INT32_MIN) return INT32_MIN;
    return (int32_t)r;
}

/* Runs `call` with a catch frame for the scaler's SafeInt traps */
#define GUARDED(err, call)                                    \
    do {                                                      \
        idwr_catch c_;                                         \
        idwr_catch_push(&c_);                                  \
        if (IDWR_SETJMP(c_.buf)) {                             \
            idwr_catch_pop(&c_);                               \
            (err) = c_.code;                                  \
            break;                                            \
        }                                                     \
        (err) = (call);                                       \
        idwr_catch_pop(&c_);                                   \
    } while (0)

void tt_outline_close(tt_outline *o) {
    if (!o) return;
    if (o->input)
        for (int i = 0; i < 9; i++) free(BASE(o->input, i));
    free(o->input), free(o->info), free(o->ends);
    free(o);
}

/* Initialize 0x1800911bc, then NewTransform 0x1800091bc as GlyphOutlineRasterizationState
 * calls it. Allocations are owned by `o`, so a trap leaves nothing behind once o is closed. */
static int open_impl(tt_outline *o, uint32_t rendering_flags) {
    SIZE(o->info, 0) = 0x4ac;
    for (int i = 0; i < 3; i++) {
        BASE(o->input, i) = SIZE(o->info, i) ? calloc(1, SIZE(o->info, i)) : NULL;
        if (SIZE(o->info, i) && !BASE(o->input, i)) return -1;
    }
    uint8_t *b0 = BASE(o->input, 0);
    *(uint32_t *)(b0 + SIZE(o->info, 0) - 4) = 0x73666e74;
    memset(b0, 0, 0x4a8);
    *(uint64_t *)(b0 + 0x178) = 0x10000; *(uint64_t *)(b0 + 0x188) = 0x10000;
    *(uint32_t *)(b0 + 0x198) = 0x10000;
    *(uint64_t *)(b0 + 0x420) = 0x10000; *(uint64_t *)(b0 + 0x430) = 0x10000;
    *(uint32_t *)(b0 + 0x440) = 0x10000;
    *(void **)(b0 + 0x118) = o->input + 8;
    *(void **)(b0 + 0x120) = o->info;
    uint8_t *in = o->input;
    *(void **)(in + 0x50) = (void *)get_fragment;
    *(void **)(in + 0x58) = (void *)release_fragment;
    *(void **)(in + 0x80) = NULL;
    *(void **)(in + 0x78) = o;
    in[0x8a] = 1; in[0x89] = 1;
    *(uint16_t *)(in + 0x90) = 0xffff; *(uint16_t *)(in + 0x92) = 0xffff;
    *(uint16_t *)(in + 0x94) = o->axes; /* (Implementation + 0x2a8 - + 0x2a0) >> 2 */
    *(void **)(in + 0x60) = (void *)apply_outline_variation;
    *(void **)(in + 0x68) = (void *)apply_cvt_variation;
    *(void **)(in + 0x70) = (void *)get_variation_axis_coordinates;
    int e = ORIG(fs2_fn, RVA_fs_NewSfnt)(in, o->info);
    if (e) return e;
    for (int i = 3; i <= 4; i++) {
        BASE(in, i) = SIZE(o->info, i) ? calloc(1, SIZE(o->info, i)) : NULL;
        if (SIZE(o->info, i) && !BASE(in, i)) return -1;
    }

    /* NewTransform with point size 0xc0000, the upem diagonal, overscale 1 × 1, ppem 0 (so no
     * strike lookup and no hdmx check) */
    uint32_t flags = o->perturb & TT_OUTLINE_PERTURB_FLAGS ? 0 : rendering_flags;
    uint32_t point_size = o->perturb & TT_OUTLINE_PERTURB_POINT_SIZE ? 12 : 0xc0000;
    int32_t m[4] = {(int32_t)o->upem << 16, 0, 0, (int32_t)o->upem << 16}; /* Transform {m11, m12, m21, m22} */
    if (flags & 4) {
        if (!(flags & 8)) {
            m[1] += fix_mul(m[0], 0x5700);
            m[3] += fix_mul(m[2], 0x5700);
        } else {
            m[0] -= fix_mul(m[1], 0x5700);
            m[2] -= fix_mul(m[3], 0x5700);
        }
    }
    *(int32_t *)(in + 0x9c) = m[0]; *(int32_t *)(in + 0xa0) = m[2]; *(int32_t *)(in + 0xa4) = 0;
    *(int32_t *)(in + 0xa8) = m[1]; *(int32_t *)(in + 0xac) = m[3]; *(int32_t *)(in + 0xb0) = 0;
    *(int32_t *)(in + 0xb4) = 0; *(int32_t *)(in + 0xb8) = 0; *(int32_t *)(in + 0xbc) = 0x40000000;
    *(uint16_t *)(in + 0x94) = 0x60; *(uint16_t *)(in + 0x96) = 0x60;
    *(uint32_t *)(in + 0x90) = point_size << 16; /* wraps to 0, as in the original */
    *(uint32_t *)(in + 0x98) = 0x16a0a;
    *(uint16_t *)(in + 0xc0) = 0; in[0xcc] = 0; *(uint32_t *)(in + 200) = 0; in[0xcd] = 0;
    in[0xce] = 1; in[0xcf] = 1;
    *(uint16_t *)(in + 0xd2) = 1; *(uint16_t *)(in + 0xd4) = 1;
    *(uint16_t *)(in + 0xd0) = 0; /* overscale x 1: scan control 0 */
    if (flags & 2) { *(uint16_t *)(in + 0xc2) = 0x14; *(uint16_t *)(in + 0xc4) = 0x14; }
    else { *(uint16_t *)(in + 0xc2) = 0; *(uint16_t *)(in + 0xc4) = 0; }
    in[0xd6] = (uint8_t)(~(flags | (o->perturb & TT_OUTLINE_PERTURB_GRID_FIT ? 1 : 0)) & 1);
    e = ORIG(newtrans_fn, RVA_fs__NewTransformation)(in, o->info, 1);
    if (e == 0x1901) {
        o->skip = 1;
        e = 0;
    } else if (e) {
        e = ORIG(newtrans_fn, RVA_fs__NewTransformation)(in, o->info, 0);
    }
    return e;
}

tt_outline *tt_outline_open(const idwr_font *font, const float *coords, uint16_t axes, uint32_t rendering_flags,
                            int perturb, int *error) {
    if (error) *error = -1;
    if (!font || axes > MAX_AXES || (axes && !coords) || !font->head || font->head_len < 54) return NULL;
    tt_outline *o = calloc(1, sizeof *o);
    if (!o) return NULL;
    o->font = font->data, o->font_len = font->size;
    o->face_offset = (uint32_t)(font->dir - font->data - 12);
    o->dfont = font;
    o->perturb = perturb;
    o->upem = idwr_u16(font->head + 18);
    o->axes = perturb & TT_OUTLINE_PERTURB_AXES ? 0 : axes;
    for (uint16_t i = 0; i < axes; i++) {
        o->coords[i] = coords[i];
        /* the float is Fixed16_16::ToFloat, so × 65536 is exact */
        o->coords214[i] =
            (int16_t)((idwr_cvttss2si(coords[i] * 65536.0f) + (perturb & TT_OUTLINE_PERTURB_COORDS_TRUNC ? 0 : 2)) >> 2);
    }
    o->input = calloc(1, INPUT_SIZE);
    o->info = calloc(1, INFO_SIZE);
    if (!o->input || !o->info) { tt_outline_close(o); return NULL; }
    int e;
    GUARDED(e, open_impl(o, rendering_flags));
    if (error) *error = e;
    if (e) { tt_outline_close(o); return NULL; }
    return o;
}

static void reset_memory_bases(tt_outline *o) {
    for (int i = 5; i <= 8; i++) {
        SIZE(o->info, i) = 0;
        free(BASE(o->input, i));
        BASE(o->input, i) = NULL;
    }
}

static int glyph_impl(tt_outline *o, uint16_t glyph) {
    uint8_t *in = o->input;
    reset_memory_bases(o); /* the bitmap bases are not used by the outline path */
    *(uint16_t *)(in + 0x90) = 0xffff;
    *(uint16_t *)(in + 0x92) = glyph;
    in[0x94] = 0;
    in[0x95] = 1;
    int e = ORIG(fs2_fn, RVA_fs_NewGlyph)(in, o->info);
    if (e) return e;
    in[0x90] = 0;
    *(void **)(in + 0x80) = NULL;
    e = ORIG(contour_fn, RVA_fs__Contour)(in, o->info, o->perturb & TT_OUTLINE_PERTURB_NO_HINT ? 0 : 1);
    if (e) e = ORIG(contour_fn, RVA_fs__Contour)(in, o->info, 0);
    return e;
}

int tt_outline_glyph(tt_outline *o, uint16_t glyph, idwr_glyph_points *out) {
    memset(out, 0, sizeof *out);
    if (o->skip) return 0;
    int e;
    GUARDED(e, glyph_impl(o, glyph));
    if (e) return e;
    uint8_t *info = o->info;
    uint32_t n = *(uint16_t *)(info + 0x72);
    const int16_t *ends = *(const int16_t **)(info + 0x90);
    if (n > o->cap_contours) {
        uint16_t *p = realloc(o->ends, sizeof *p * n);
        if (!p) return -1;
        o->ends = p, o->cap_contours = n;
    }
    /* end points as unsigned, the way GetOutline walks them (sign-extended shorts) */
    for (uint32_t c = 0; c < n; c++) o->ends[c] = (uint16_t)ends[c];
    int32_t *x = *(int32_t **)(info + 0x78);
    if ((o->perturb & TT_OUTLINE_PERTURB_POINT) && n && x) x[0] += 1;
    out->x = x;
    out->y = *(const int32_t **)(info + 0x80);
    out->on = *(const uint8_t **)(info + 0x98);
    out->ends = o->ends;
    out->contours = n;
    return 0;
}

/* ---- the public API (idwr.h) ---- */

struct idwr_outline_scaler {
    tt_outline *tt;
#if defined(IDWR_STANDALONE) && defined(IDWR_CFF)
    idwr_cff_outline *cff; /* a CFF face: GlyphOutlineRasterizationState's CffRasterizer instead */
#endif
    const idwr_font *font;
    float coords[MAX_AXES];
    uint16_t axes;
    uint32_t simulations;
    int perturb;
    int last_error;
    /* sideways runs of an oblique face: a second rasterizer, flags | 8 (the shear of the other
     * matrix column), opened on the first such run. GetGlyphRunOutline keys its outline cache
     * on these flags; without oblique, sideways runs share the upright points. */
    tt_outline *tt_sideways;
    /* The face's outline cache: the glyphs already rasterized, each once, in the order the runs
     * met them (GlyphDataElement::GetExistingGlyphs / AddGlyphs); a later run takes them from here,
     * not from the rasterizer. used: the rasterizers have run a glyph since they were set up. */
    struct run_glyph *run;
    uint32_t nrun, caprun;
    int used, used_sideways;
};

/* A cached glyph: its points, copied out of the scaler. */
typedef struct run_glyph {
    uint16_t glyph;
    uint8_t side;
    uint32_t contours;
    uint16_t *ends;
    int32_t *x, *y;
    uint8_t *on;
} run_glyph;

static int g_perturb; /* idwr_outline_scaler_perturb: negative controls of the checks */

/* not in idwr.h: TT_OUTLINE_PERTURB_* bits for the scalers opened afterwards */
TT_EXPORT void idwr_outline_scaler_perturb(int bits) { g_perturb = bits; }

IDWR_API idwr_outline_scaler *idwr_outline_scaler_open(const idwr_font *font, const float *coords, uint16_t axes,
                                                    uint32_t simulations) {
    if (!font || axes > MAX_AXES || (axes && !coords)) return NULL;
    idwr_outline_scaler *s = calloc(1, sizeof *s);
    if (!s) return NULL;
#if defined(IDWR_STANDALONE) && defined(IDWR_CFF)
    if (idwr_font_is_cff(font)) { /* FontRasterizer::CreateRasterizer: CffRasterizer */
        s->cff = idwr_cff_outline_open(font, coords, axes, simulations, &s->last_error);
        if (!s->cff) { free(s); return NULL; }
        s->font = font;
        s->simulations = simulations;
        return s;
    }
#endif
    /* axes = 0 on a variable font: the default instance. The Implementation constructor
     * (0x180092ae0) normalizes the face's axis values whenever fvar has axes, so the scaler gets
     * the fvar axis count (input + 0x94) and their default coordinates, as for textures; the
     * origins take the instance's design metrics (outline.c, run_outline). */
    float defaults[MAX_AXES];
#ifndef IDWR_NEGATIVE_CONTROL_DEFAULTINSTANCE
    if (!axes) {
        int n = idwr_normalize_axes(font, NULL, 0, defaults, MAX_AXES);
        if (n > 0) coords = defaults, axes = (uint16_t)n;
    }
#else
    (void)defaults;
#endif
    /* MakeRasterizerFlagsForOutlineRendering, not sideways (sideways oblique: tt_sideways) */
    uint32_t flags = (simulations & IDWR_SIM_BOLD ? 2u : 0u) | (simulations & IDWR_SIM_OBLIQUE ? 4u : 0u);
    s->tt = tt_outline_open(font, coords, axes, flags, g_perturb, &s->last_error);
    if (!s->tt) { free(s); return NULL; }
    s->font = font;
    s->axes = axes;
    if (axes) memcpy(s->coords, coords, sizeof(float) * axes);
    s->simulations = simulations;
    s->perturb = g_perturb;
    return s;
}

static void run_clear(idwr_outline_scaler *s) {
    for (uint32_t i = 0; i < s->nrun; i++) {
        free(s->run[i].ends), free(s->run[i].x), free(s->run[i].y), free(s->run[i].on);
    }
    s->nrun = 0;
}

/* The errors GetOutline 0x1800ccb70 turns into FileFormatException: an fs error of fs_NewGlyph or
 * of the unhinted retry of fs__Contour, and the SafeInt traps its __except translates
 * (TranslateRasterizerException 0x18014ccb4). GlyphOutlineRasterizationState::RasterizeGlyph
 * 0x1800901e0 catches it and the glyph is empty: the run goes on. */
static int outline_fileformat(int e) {
#ifdef IDWR_NEGATIVE_CONTROL_OUTLINERUN
    return 0;   /* linux_check.py --texture-control outlinerun: an fs error fails the run */
#endif
    return (e > 0 && e < 0x10000) || e == IDWR_ERR_SAFEINT_OVERFLOW || e == IDWR_ERR_SAFEINT_DIVZERO;
}

/* one glyph, through the outline cache */
static int scaler_points(idwr_outline_scaler *s, tt_outline *tt, int side, uint16_t glyph, idwr_glyph_points *out) {
#ifdef IDWR_NEGATIVE_CONTROL_OUTLINERUN
    (void)side;   /* and no outline cache: every glyph from the rasterizer, kept across runs */
    s->last_error = tt_outline_glyph(tt, glyph, out);
    return s->last_error ? -1 : 0;
#endif
    for (uint32_t i = 0; i < s->nrun; i++) {
        run_glyph *r = &s->run[i];
        if (r->glyph != glyph || r->side != side) continue;
        memset(out, 0, sizeof *out);
        out->x = r->x, out->y = r->y, out->on = r->on, out->ends = r->ends, out->contours = r->contours;
        return 0;
    }
    if (side) s->used_sideways = 1;
    else s->used = 1;
    int e = tt_outline_glyph(tt, glyph, out);
    if (e) {
        s->last_error = e;
        if (!outline_fileformat(e)) return -1;
        memset(out, 0, sizeof *out);
    }
    if (s->nrun == s->caprun) {
        uint32_t cap = s->caprun ? s->caprun * 2 : 16;
        run_glyph *p = realloc(s->run, sizeof *p * cap);
        if (!p) return -1;
        s->run = p, s->caprun = cap;
    }
    run_glyph *r = &s->run[s->nrun];
    memset(r, 0, sizeof *r);
    r->glyph = glyph, r->side = (uint8_t)side, r->contours = out->contours;
    uint32_t n = out->contours ? (uint32_t)out->ends[out->contours - 1] + 1 : 0;
    if (out->contours) {
        r->ends = malloc(sizeof *r->ends * out->contours);
        r->x = malloc(sizeof *r->x * n), r->y = malloc(sizeof *r->y * n), r->on = malloc(n);
        if (!r->ends || !r->x || !r->y || !r->on) {
            free(r->ends), free(r->x), free(r->y), free(r->on);
            return -1;
        }
        memcpy(r->ends, out->ends, sizeof *r->ends * out->contours);
        memcpy(r->x, out->x, sizeof *r->x * n), memcpy(r->y, out->y, sizeof *r->y * n), memcpy(r->on, out->on, n);
        out->x = r->x, out->y = r->y, out->on = r->on, out->ends = r->ends;
    }
    s->nrun++;
    return 0;
}

/* A GetGlyphRunOutline call creates its GlyphOutlineRasterizationState (0x180090b74:
 * CreateRasterizer, NewTransform), so a call starts from the rasterizer as the font and
 * pre-programs leave it: what a glyph program wrote into the twilight zone, the CVT or the
 * storage in an earlier call is gone. Within the call it stays, for the glyphs after it. */
static int run_begin(idwr_outline_scaler *s) {
#ifdef IDWR_NEGATIVE_CONTROL_OUTLINERUN
    return 0;
#endif
    if (s->used) {
        uint32_t flags = (s->simulations & IDWR_SIM_BOLD ? 2u : 0u) | (s->simulations & IDWR_SIM_OBLIQUE ? 4u : 0u);
        tt_outline_close(s->tt);
        s->tt = tt_outline_open(s->font, s->axes ? s->coords : NULL, s->axes, flags, s->perturb, &s->last_error);
        s->used = 0;
        if (!s->tt) return -1;
    }
    if (s->used_sideways && s->tt_sideways) {
        tt_outline_close(s->tt_sideways);   /* opened again by the next sideways oblique run */
        s->tt_sideways = NULL;
    }
    s->used_sideways = 0;
    return 0;
}

IDWR_API void idwr_outline_scaler_close(idwr_outline_scaler *s) {
    if (!s) return;
    run_clear(s);
    free(s->run);
#if defined(IDWR_STANDALONE) && defined(IDWR_CFF)
    idwr_cff_outline_close(s->cff);
#endif
    tt_outline_close(s->tt);
    if (s->tt_sideways) tt_outline_close(s->tt_sideways);
    free(s);
}

IDWR_API int idwr_outline_scaler_glyph(void *ctx, uint16_t glyph, idwr_glyph_points *out) {
    idwr_outline_scaler *s = (idwr_outline_scaler *)ctx;
#if defined(IDWR_STANDALONE) && defined(IDWR_CFF)
    if (s->cff) { /* CFF outlines are cubic commands, not points */
        memset(out, 0, sizeof *out);
        return -1;
    }
#endif
    if (!s->tt) return -1;
    s->last_error = 0;
    return scaler_points(s, s->tt, 0, glyph, out);
}

IDWR_API int idwr_outline_scaler_last_error(const idwr_outline_scaler *s) {
#if defined(IDWR_STANDALONE) && defined(IDWR_CFF)
    if (s && s->cff) return idwr_cff_outline_last_error(s->cff);
#endif
    return s ? s->last_error : 0;
}

/* ---- sideways runs of an oblique face ----
 * MakeRasterizerFlagsForOutlineRendering 0x18008f3ac: (bold ? 2 : 0) | (oblique ? 4 + 8 * sideways : 0).
 * Flag 8 makes NewTransform subtract FixMul(m12, 0x5700) from m11 and FixMul(m22, 0x5700) from
 * m21 instead of adding to the second column (open_impl above). */
static int scaler_glyph_sideways(void *ctx, uint16_t glyph, idwr_glyph_points *out) {
    idwr_outline_scaler *s = (idwr_outline_scaler *)ctx;
    s->last_error = 0;
    return scaler_points(s, s->tt_sideways, 1, glyph, out);
}

static int scaler_run_sideways_oblique(idwr_outline_scaler *s, int sims, float em_size, const uint16_t *glyphs,
                                       const float *advances, const idwr_glyph_offset *offsets, uint32_t count,
                                       int right_to_left, idwr_path_cmd *out, int capacity) {
    if (!s->tt_sideways) {
        uint32_t flags = (s->simulations & IDWR_SIM_BOLD ? 2u : 0u) | 4u |
                         (s->perturb & TT_OUTLINE_PERTURB_SIDEWAYS_SHEAR ? 0u : 8u);
        s->tt_sideways = tt_outline_open(s->font, s->axes ? s->coords : NULL, s->axes, flags, s->perturb,
                                         &s->last_error);
        if (!s->tt_sideways) return -1;
    }
    return idwr_glyph_run_outline_points(s->font, s->coords, s->axes, sims, em_size, glyphs, advances, offsets, count,
                                        1, right_to_left, scaler_glyph_sideways, s, out, capacity);
}

IDWR_API int idwr_outline_scaler_run(idwr_outline_scaler *s, float em_size, const uint16_t *glyphs,
                                   const float *advances, const idwr_glyph_offset *offsets, uint32_t count,
                                   int sideways, int right_to_left, idwr_path_cmd *out, int capacity) {
    if (!s) return -1;
#if defined(IDWR_STANDALONE) && defined(IDWR_CFF)
    if (s->cff)
        return idwr_cff_outline_run(s->cff, em_size, glyphs, advances, offsets, count, sideways, right_to_left, out,
                                   capacity);
#endif
    if (run_begin(s)) return -1;
    int sims = (int)s->simulations;
    if (s->perturb & TT_OUTLINE_PERTURB_BOLD_ADVANCE) sims &= ~IDWR_SIM_BOLD;
    if (sideways && (s->simulations & IDWR_SIM_OBLIQUE))
        return scaler_run_sideways_oblique(s, sims, em_size, glyphs, advances, offsets, count, right_to_left, out,
                                           capacity);
    return idwr_glyph_run_outline_points(s->font, s->coords, s->axes, sims, em_size, glyphs, advances, offsets, count,
                                        sideways, right_to_left, idwr_outline_scaler_glyph, s, out, capacity);
}

IDWR_API int idwr_glyph_run_outline_hinted(const idwr_font *font, const float *coords, uint16_t axes,
                                         uint32_t simulations, float em_size, const uint16_t *glyphs,
                                         const float *advances, const idwr_glyph_offset *offsets, uint32_t count,
                                         int sideways, int right_to_left, idwr_path_cmd *out, int capacity) {
    idwr_outline_scaler *s = idwr_outline_scaler_open(font, coords, axes, simulations);
    if (!s) return -1;
    int n = idwr_outline_scaler_run(s, em_size, glyphs, advances, offsets, count, sideways, right_to_left, out,
                                   capacity);
    idwr_outline_scaler_close(s);
    return n;
}
