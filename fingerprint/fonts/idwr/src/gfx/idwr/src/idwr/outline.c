/*
 * Glyph outlines: IDWriteFontFace::GetGlyphRunOutline for TrueType faces, as DWrite.dll
 * 10.0.26100.9711 builds them:
 *   GlyphOutlineRasterizationState      runs the TrueType scaler at ppem = unitsPerEm, so its
 *                                       points are font units × 64; the glyph programs run
 *                                       (fs__Contour with hinting on), so on some faces the
 *                                       points are fractional. idwr_glyph_run_outline reads
 *                                       glyf unhinted; idwr_glyph_run_outline_points takes the
 *                                       points from a provider (the scaler:
 *                                       reimpl/oracle/outline_oracle.c)
 *   FontFace::GetGlyphAdvances 0x18004a8d0  design advances, widened under bold
 *   TrueTypeRasterizer::Implementation::GetOutline 0x1800ccb70  points to commands in 16.16
 *                                       font units: move, line, quadratic
 *   ComputeGlyphOrigins 0x180049844     glyph origins from advances and offsets
 *   StreamGlyphOutlines<OutlineTransformWithOffset<0>> 0x18008f430  scaling by
 *                                       em / 65536 / unitsPerEm, y down, quadratics as cubics;
 *                                       the cubic (4) and close (5) commands of
 *                                       CffRasterizer::GetOutline (idwr/cffraster.c, which hands
 *                                       its commands to idwr_glyph_run_outline_commands)
 *   sideways runs (isSideways TRUE): ComputeGlyphOrigins' vertical branch over
 *   FontFace::GetDesignGlyphMetrics 0x1800525a4, and StreamGlyphOutlines<OutlineTransformWithOffset<1>>
 *   0x180151518 (the glyph turned a quarter): run_outline_sideways below
 */
#include <stdlib.h>
#include <string.h>
#include "idwr.h"
#include "idwr_internal.h"
#if defined(IDWR_STANDALONE) && defined(IDWR_CFF)
#include "cffraster.h"
#endif
#ifdef IDWR_STANDALONE
#include "ttraster.h"
#include "../truetype/native.h"
#endif

static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

/* ---- glyf ---- */

typedef struct {
    int32_t *x, *y; /* 26.6 at ppem = unitsPerEm, i.e. font units × 64 */
    uint8_t *on;
    uint16_t *ends;
    uint32_t points, contours, cap_points, cap_contours;
    const float *coords; /* variable instance: normalized coordinates, else NULL */
    uint16_t axes;
    /* metrics: the next glyph loaded records its phantom point deltas (pp1.x, pp1.y, ...,
     * pp4.y); a USE_MY_METRICS component takes over from its composite */
    int capture;
    uint16_t metrics_glyph;
    int32_t metrics_dx, pd[8];
} outline;

static int grow(outline *o, uint32_t points, uint32_t contours) {
    if (o->points + points > o->cap_points) {
        uint32_t cap = (o->points + points) * 2;
        int32_t *x = realloc(o->x, cap * sizeof *x), *y = x ? realloc(o->y, cap * sizeof *y) : NULL;
        if (x) o->x = x;
        if (y) o->y = y;
        uint8_t *on = y ? realloc(o->on, cap) : NULL;
        if (!on) return -1;
        o->on = on, o->cap_points = cap;
    }
    if (o->contours + contours > o->cap_contours) {
        uint32_t cap = (o->contours + contours) * 2;
        uint16_t *e = realloc(o->ends, cap * sizeof *e);
        if (!e) return -1;
        o->ends = e, o->cap_contours = cap;
    }
    return 0;
}

static const uint8_t *glyph_data(const idwr_font *font, uint16_t glyph, uint32_t *len) {
    uint32_t loca_len, glyf_len;
    const uint8_t *loca = idwr_font_table(font, IDWR_TAG('l', 'o', 'c', 'a'), &loca_len);
    const uint8_t *glyf = idwr_font_table(font, IDWR_TAG('g', 'l', 'y', 'f'), &glyf_len);
    if (!loca || !glyf || !font->head || font->head_len < 54) return NULL;
    uint32_t start, end;
    if (idwr_i16(font->head + 50)) {
        if (4 * (uint32_t)glyph + 8 > loca_len) return NULL;
        start = be32(loca + 4 * glyph), end = be32(loca + 4 * glyph + 4);
    } else {
        if (2 * (uint32_t)glyph + 4 > loca_len) return NULL;
        start = 2u * idwr_u16(loca + 2 * glyph), end = 2u * idwr_u16(loca + 2 * glyph + 2);
    }
    if (end < start || end > glyf_len) return NULL;
    *len = end - start;
    return glyf + start;
}

static int load_glyph(const idwr_font *font, uint16_t glyph, outline *o, int depth);

/* gvar for `n` points (and 4 phantom points, which do not move the outline) in font units */
static int vary(const idwr_font *font, const outline *o, uint16_t glyph, const int16_t *ends, uint16_t contours,
                int32_t *x, int32_t *y, uint32_t n, int32_t *pd) {
    int32_t *vx = calloc(n + 4, sizeof *vx), *vy = calloc(n + 4, sizeof *vy);
    if (!vx || !vy) { free(vx), free(vy); return -1; }
    if (n) memcpy(vx, x, n * sizeof *vx), memcpy(vy, y, n * sizeof *vy);
    if (idwr_apply_gvar(font, o->coords, o->axes, glyph, ends, contours, vx, vy, n + 4) == 0) {
        if (n) memcpy(x, vx, n * sizeof *vx), memcpy(y, vy, n * sizeof *vy);
        /* the phantom points start at 0 and only take explicit deltas, so they hold the deltas */
        if (pd)
            for (int i = 0; i < 4; i++) pd[2 * i] = vx[n + i], pd[2 * i + 1] = vy[n + i];
    }
    free(vx), free(vy);
    return 0;
}

/* starts recording the phantom deltas of `glyph` if the caller asked for them */
static int32_t *capture(outline *o, uint16_t glyph) {
    if (!o->capture) return NULL;
    o->capture = 0;
    o->metrics_glyph = glyph, o->metrics_dx = 0;
    memset(o->pd, 0, sizeof o->pd);
    return o->pd;
}

static int load_simple(const idwr_font *font, uint16_t glyph, const uint8_t *g, uint32_t len, int16_t contours,
                       outline *o) {
    int32_t *pd = capture(o, glyph);
    uint32_t at = 10;
    if (at + 2u * contours + 2 > len) return -1;
    uint32_t first = o->points;
    uint32_t n = contours ? (uint32_t)idwr_u16(g + at + 2 * (contours - 1)) + 1 : 0;
    if (grow(o, n, contours)) return -1;
    for (int16_t c = 0; c < contours; c++) o->ends[o->contours + c] = (uint16_t)(first + idwr_u16(g + at + 2 * c));
    at += 2u * contours;
    at += 2 + idwr_u16(g + at);
    uint8_t *flags = o->on + first;
    for (uint32_t i = 0; i < n;) {
        if (at >= len) return -1;
        uint8_t f = g[at++], repeat = 0;
        if (f & 8) {
            if (at >= len) return -1;
            repeat = g[at++];
        }
        for (uint32_t r = 0; r <= repeat && i < n; r++) flags[i++] = f;
    }
    int32_t v = 0;
    for (uint32_t i = 0; i < n; i++) {
        uint8_t f = flags[i];
        if (f & 2) {
            if (at >= len) return -1;
            v += f & 0x10 ? g[at] : -(int32_t)g[at];
            at++;
        } else if (!(f & 0x10)) {
            if (at + 2 > len) return -1;
            v += idwr_i16(g + at);
            at += 2;
        }
        o->x[first + i] = v;
    }
    v = 0;
    for (uint32_t i = 0; i < n; i++) {
        uint8_t f = flags[i];
        if (f & 4) {
            if (at >= len) return -1;
            v += f & 0x20 ? g[at] : -(int32_t)g[at];
            at++;
        } else if (!(f & 0x20)) {
            if (at + 2 > len) return -1;
            v += idwr_i16(g + at);
            at += 2;
        }
        o->y[first + i] = v;
    }
    if (o->axes && (n || pd)) {
        int16_t *ends = malloc(sizeof *ends * (contours ? contours : 1));
        if (!ends) return -1;
        for (int16_t c = 0; c < contours; c++) ends[c] = (int16_t)idwr_u16(g + 10 + 2 * c);
        int err = vary(font, o, glyph, ends, (uint16_t)contours, o->x + first, o->y + first, n, pd);
        free(ends);
        if (err) return -1;
    }
    for (uint32_t i = 0; i < n; i++) flags[i] &= 1, o->x[first + i] *= 64, o->y[first + i] *= 64;
    o->points += n;
    o->contours += contours;
    return 0;
}

/* 2.14 times 26.6, rounded */
static int32_t mul214(int32_t v, int32_t s) { return (int32_t)(((int64_t)v * s + 0x2000) >> 14); }

typedef struct {
    uint16_t flags, glyph;
    int32_t a1, a2, xx, xy, yx, yy;
    int scaled;
} component;

static int load_composite(const idwr_font *font, uint16_t glyph, const uint8_t *g, uint32_t len, outline *o,
                          int depth) {
    int32_t *pd = capture(o, glyph);
    component *comps = NULL;
    uint32_t count = 0, cap = 0, at = 10;
    uint16_t flags;
    int err = -1;
    do {
        if (at + 4 > len) goto done;
        if (count == cap) {
            cap = cap ? cap * 2 : 8;
            component *c = realloc(comps, cap * sizeof *c);
            if (!c) goto done;
            comps = c;
        }
        component *c = &comps[count++];
        flags = c->flags = idwr_u16(g + at);
        c->glyph = idwr_u16(g + at + 2);
        at += 4;
        if (flags & 1) {
            if (at + 4 > len) goto done;
            c->a1 = flags & 2 ? idwr_i16(g + at) : idwr_u16(g + at);
            c->a2 = flags & 2 ? idwr_i16(g + at + 2) : idwr_u16(g + at + 2);
            at += 4;
        } else {
            if (at + 2 > len) goto done;
            c->a1 = flags & 2 ? (int8_t)g[at] : g[at];
            c->a2 = flags & 2 ? (int8_t)g[at + 1] : g[at + 1];
            at += 2;
        }
        c->xx = 0x4000, c->xy = 0, c->yx = 0, c->yy = 0x4000, c->scaled = 0;
        if (flags & 8) {
            if (at + 2 > len) goto done;
            c->xx = c->yy = idwr_i16(g + at);
            at += 2, c->scaled = 1;
        } else if (flags & 0x40) {
            if (at + 4 > len) goto done;
            c->xx = idwr_i16(g + at), c->yy = idwr_i16(g + at + 2);
            at += 4, c->scaled = 1;
        } else if (flags & 0x80) {
            if (at + 8 > len) goto done;
            c->xx = idwr_i16(g + at), c->xy = idwr_i16(g + at + 2), c->yx = idwr_i16(g + at + 4), c->yy = idwr_i16(g + at + 6);
            at += 8, c->scaled = 1;
        }
    } while (flags & 0x20);
    if (o->axes) {
        /* one point per component, its offset; point-matched components keep their numbers */
        int32_t *x = malloc(sizeof *x * count), *y = malloc(sizeof *y * count);
        if (!x || !y) { free(x), free(y); goto done; }
        for (uint32_t k = 0; k < count; k++)
            x[k] = comps[k].flags & 2 ? comps[k].a1 : 0, y[k] = comps[k].flags & 2 ? comps[k].a2 : 0;
        int e = vary(font, o, glyph, NULL, 0, x, y, count, pd);
        for (uint32_t k = 0; k < count; k++)
            if (comps[k].flags & 2) comps[k].a1 = x[k], comps[k].a2 = y[k];
        free(x), free(y);
        if (e) goto done;
    }
    uint32_t first = o->points;
    for (uint32_t k = 0; k < count; k++) {
        const component *c = &comps[k];
        uint32_t base = o->points;
        o->capture = pd && (c->flags & 0x200);
        if (load_glyph(font, c->glyph, o, depth + 1)) goto done;
        /* the component's own phantom points: the (varied) component offset does not move them */
        o->capture = 0;
        if (c->scaled)
            for (uint32_t i = base; i < o->points; i++) {
                int32_t x = o->x[i], y = o->y[i];
                o->x[i] = mul214(x, c->xx) + mul214(y, c->yx);
                o->y[i] = mul214(x, c->xy) + mul214(y, c->yy);
            }
        int32_t dx, dy;
        if (c->flags & 2) {
            dx = c->a1 * 64, dy = c->a2 * 64;
        } else {
            /* point matching: point a1 of the glyph so far onto point a2 of the component */
            uint32_t pp = first + (uint32_t)c->a1, cp = base + (uint32_t)c->a2;
            if (pp >= base || cp >= o->points) goto done;
            dx = o->x[pp] - o->x[cp], dy = o->y[pp] - o->y[cp];
        }
        for (uint32_t i = base; i < o->points; i++) o->x[i] += dx, o->y[i] += dy;
    }
    err = 0;
done:
    free(comps);
    return err;
}

static int load_glyph(const idwr_font *font, uint16_t glyph, outline *o, int depth) {
    if (depth > 16) return -1;
    uint32_t len;
    const uint8_t *g = glyph_data(font, glyph, &len);
    if (!g) return -1;
    if (len == 0) {
        int32_t *pd = capture(o, glyph);
        if (!pd || !o->axes) return 0;
        if (depth == 0) return vary(font, o, glyph, NULL, 0, NULL, NULL, 0, pd);
        /* An empty component: the scaler gives it one point at (0, 0) (sfac_ReadOutlineData
         * with a parent: E_NC = 1, one contour ending at point 0) and passes the variation
         * callback 1 + 4 points, so gvar data written for the 4 phantom points does not fit
         * and no delta applies (Recursive: a USE_MY_METRICS space keeps its default advance).
         * The point is not added to the outline: it has no area and no composite here
         * point-matches it. */
#ifndef IDWR_NEGATIVE_CONTROL_EMPTYCOMPONENT
        int32_t x = 0, y = 0;
        int16_t end = 0;
        return vary(font, o, glyph, &end, 1, &x, &y, 1, pd);
#else
        return vary(font, o, glyph, NULL, 0, NULL, NULL, 0, pd);
#endif
    }
    if (len < 10) return -1;
    int16_t contours = idwr_i16(g);
    return contours >= 0 ? load_simple(font, glyph, g, len, contours, o)
                         : load_composite(font, glyph, g, len, o, depth);
}

/* ---- commands (GetOutline) ---- */

enum { CMD_MOVE = 2, CMD_LINE = 3, CMD_CUBIC = 4, CMD_CLOSE = 5, CMD_QUAD = 6 };

typedef struct { int32_t *v; uint32_t n, cap; } cmdbuf;

static int put(cmdbuf *b, int32_t v) {
    if (b->n == b->cap) {
        uint32_t cap = b->cap ? b->cap * 2 : 256;
        int32_t *p = realloc(b->v, cap * sizeof *p);
        if (!p) return -1;
        b->v = p, b->cap = cap;
    }
    b->v[b->n++] = v;
    return 0;
}

static int commands(const idwr_glyph_points *o, cmdbuf *b) {
    int err = 0;
    int32_t start = 0;
    for (uint32_t c = 0; c < o->contours; c++) {
        int32_t end = o->ends[c], last = end;
        if (start < end) {
            int32_t fx, fy, i = start;
            if (o->on[start]) {
                fx = o->x[start] << 10, fy = o->y[start] << 10, i = start + 1;
            } else if (o->on[end]) {
                fx = o->x[end] << 10, fy = o->y[end] << 10, last = end - 1;
            } else {
                fx = (o->x[start] + o->x[end]) * 0x200, fy = (o->y[start] + o->y[end]) * 0x200;
            }
            err |= put(b, CMD_MOVE) | put(b, fx) | put(b, fy);
            int pending = 0;
            int32_t px = 0, py = 0;
            for (; i <= last; i++) {
                int32_t x = o->x[i] << 10, y = o->y[i] << 10;
                if (!o->on[i]) {
                    if (pending)
                        err |= put(b, CMD_QUAD) | put(b, px) | put(b, py) | put(b, (px + x) / 2) | put(b, (py + y) / 2);
                    px = x, py = y, pending = 1;
                } else if (pending) {
                    err |= put(b, CMD_QUAD) | put(b, px) | put(b, py) | put(b, x) | put(b, y);
                    pending = 0;
                } else {
                    err |= put(b, CMD_LINE) | put(b, x) | put(b, y);
                }
            }
            if (pending) err |= put(b, CMD_QUAD) | put(b, px) | put(b, py) | put(b, fx) | put(b, fy);
        }
        start = end + 1;
    }
    return err ? -1 : 0;
}

/* ---- streaming (StreamGlyphOutlines) ---- */

typedef struct {
    idwr_path_cmd *out;
    int n, cap;
} sink;

static void emit(sink *s, int op, const float *p, int np) {
    if (s->n < s->cap) {
        s->out[s->n].op = op;
        memset(s->out[s->n].p, 0, sizeof s->out[s->n].p);
        memcpy(s->out[s->n].p, p, sizeof(float) * np);
    }
    s->n++;
}

/* the words of one glyph's commands; -1 for what StreamGlyphOutlines throws
 * InvalidCacheDataException on (a command cut short, one outside a figure, an unknown one) */
static int stream(sink *s, const cmdbuf *b, float scale, float ox, float oy) {
    float nscale = -scale, cx = 0, cy = 0;
    int open = 0;
    for (uint32_t i = 0; i < b->n;) {
        int32_t op = b->v[i++];
        uint32_t left = b->n - i;
        if (op == CMD_CUBIC) {
            if (!open || left < 6) return -1;
            float p[6];
            for (int k = 0; k < 6; k += 2)
                p[k] = idwr_addss(ox, idwr_mulss((float)b->v[i + k], scale)),
                p[k + 1] = idwr_addss(oy, idwr_mulss(nscale, (float)b->v[i + k + 1]));
            emit(s, IDWR_PATH_BEZIER, p, 6);
            cx = p[4], cy = p[5], i += 6;
            continue;
        }
        if (op == CMD_CLOSE) {
            if (!open) return -1;
            emit(s, IDWR_PATH_END, NULL, 0);
            open = 0;
            continue;
        }
        if (op != CMD_MOVE && op != CMD_LINE && op != CMD_QUAD) return -1;
        if (left < (op == CMD_QUAD ? 4u : 2u) || (op != CMD_MOVE && !open)) return -1;
        if (op == CMD_MOVE) {
            if (open) emit(s, IDWR_PATH_END, NULL, 0);
            cx = idwr_addss(ox, idwr_mulss((float)b->v[i], scale)), cy = idwr_addss(oy, idwr_mulss(nscale, (float)b->v[i + 1]));
            float p[2] = {cx, cy};
            emit(s, IDWR_PATH_BEGIN, p, 2);
            open = 1, i += 2;
        } else if (op == CMD_LINE) {
            cx = idwr_addss(ox, idwr_mulss((float)b->v[i], scale)), cy = idwr_addss(oy, idwr_mulss(nscale, (float)b->v[i + 1]));
            float p[2] = {cx, cy};
            emit(s, IDWR_PATH_LINE, p, 2);
            i += 2;
        } else {
            float qx = idwr_addss(ox, idwr_mulss((float)b->v[i], scale)), qy = idwr_addss(oy, idwr_mulss(nscale, (float)b->v[i + 1]));
            float ex = idwr_addss(ox, idwr_mulss((float)b->v[i + 2], scale)), ey = idwr_addss(oy, idwr_mulss(nscale, (float)b->v[i + 3]));
            const float two = 0.666666627f, one = 0.333333343f;
            float p[6] = {idwr_addss(idwr_mulss(qx, two), idwr_mulss(cx, one)), idwr_addss(idwr_mulss(qy, two), idwr_mulss(cy, one)),
                          idwr_addss(idwr_mulss(qx, two), idwr_mulss(ex, one)), idwr_addss(idwr_mulss(qy, two), idwr_mulss(ey, one)), ex, ey};
            emit(s, IDWR_PATH_BEZIER, p, 6);
            cx = ex, cy = ey, i += 4;
        }
    }
    if (open) emit(s, IDWR_PATH_END, NULL, 0);
    return 0;
}

/* FontFace::GetGlyphAdvances 0x18004a8d0, natural measuring, two paths:
 *   - no bold simulation: the face's advance array (FontFace + 0x110), which for a variable
 *     instance holds the HVAR advances (idwr_hvar_advance);
 *   - bold: the advance of the design glyph metrics (::GetDesignGlyphMetrics, the metrics
 *     IDWriteFontFace::GetDesignGlyphMetrics returns: idwr_get_design_glyph_metrics_instance,
 *     for a bahnschrift instance the scaler's phantom points), plus (unitsPerEm + 25) / 50 for
 *     a glyph with an advance and ink in both directions. On bahnschrift instances the two
 *     advances differ by up to 34 units. */
static int design_advance(const idwr_font *font, const float *coords, uint16_t axes, uint16_t glyph, int simulations,
                          uint16_t upem, int32_t *advance) {
    idwr_glyph_metrics m;
    int32_t a;
    if (simulations & 1) {
        if (!idwr_get_design_glyph_metrics_instance(font, coords, axes, glyph, &m)) return 0;
        a = (int32_t)m.advanceWidth;
        if (a != 0 && (int32_t)m.advanceWidth - m.leftSideBearing - m.rightSideBearing > 0 &&
            (int32_t)m.advanceHeight - m.topSideBearing - m.bottomSideBearing > 0)
            a += (upem + 25) / 50;
    } else if (!(axes && idwr_hvar_advance(font, coords, axes, glyph, &a))) {
        if (!idwr_get_design_glyph_metrics_instance(font, coords, axes, glyph, &m)) return 0;
        a = (int32_t)m.advanceWidth;
    }
    *advance = a;
    return 1;
}

/* ---- sideways runs (isSideways TRUE) ----
 *
 * ComputeGlyphRunOutline 0x18008ff98 passes isSideways to three places:
 *   MakeRasterizerFlagsForOutlineRendering 0x18008f3ac  flag 8 with oblique: the scaler shears
 *                                      the other matrix column (the points provider's business:
 *                                      outline_driver.c)
 *   ComputeGlyphOrigins 0x180049844    the sideways branch: FontFace::GetGlyphMetrics with
 *                                      natural measuring is FontFace::GetDesignGlyphMetrics
 *                                      0x1800525a4 (isSideways TRUE), and per glyph
 *                                        x = pen (+ or - the advance) + verticalOriginY,
 *                                        y = advanceWidth * 0.5 + 0
 *                                      in DIPs; the pen moves by the given advance, or by the
 *                                      advance height; right to left the origin is pen - height
 *   StreamGlyphOutlines<OutlineTransformWithOffset<1>> 0x180151518  each point (x, y) of the
 *                                      commands goes to (ox + -(y * s), oy + -(x * s)): the
 *                                      glyph turned a quarter, its top at the pen
 * Glyph offsets apply as for horizontal runs. */

/* Negative controls of the checks (reimpl/outline_hinted_check.py --sideways-perturb), not in
 * idwr.h: 1 the vertical origin left out (x = pen), 2 the half advance width left out (y = 0),
 * 4 the simulations' metric adjustments left out, 8 the points not turned (x, y as upright) */
static int g_sideways_perturb;
IDWR_API void idwr_outline_sideways_perturb(int bits) { g_sideways_perturb = bits; }

/* FontFace::GetDesignGlyphMetrics 0x1800525a4 with isSideways TRUE: the design metrics of the
 * face (of the instance: CreateFontMetricsBuilder with the axis values), then
 * AdjustGlyphMetricsForBoldSimulation 0x18009cad0 and AdjustGlyphMetricsForObliqueSimulation
 * 0x18009c9f4 (the sideways branch). GlyphHasSize 0x18009db2c guards both. */
static int sideways_has_size(const idwr_glyph_metrics *m) {
    int64_t w = (int64_t)m->advanceWidth - m->rightSideBearing - m->leftSideBearing;
    int64_t h = (int64_t)m->advanceHeight - m->bottomSideBearing - m->topSideBearing;
    return w > 0 && h > 0;
}

static int sideways_metrics(const idwr_font *font, const float *coords, uint16_t axes, int simulations, uint16_t upem,
                            uint16_t glyph, idwr_glyph_metrics *m) {
    memset(m, 0, sizeof *m);
    if (!idwr_get_design_glyph_metrics_instance(font, coords, axes, glyph, m)) return 0;
    if (g_sideways_perturb & 4) return 1;
    if ((simulations & 1) && sideways_has_size(m)) {
        int32_t k = (int32_t)((upem + 25u) / 50u);
        if (m->advanceWidth == 0) m->rightSideBearing -= k;
        else m->advanceWidth += (uint32_t)k;
        if (m->advanceHeight == 0) {
            m->topSideBearing -= k;
        } else {
            m->verticalOriginY += k;
            m->advanceHeight += (uint32_t)k;
        }
    }
    if ((simulations & 2) && sideways_has_size(m)) {
        int32_t h = (int32_t)((m->advanceWidth >> 1) * 0x5700u) >> 16;
        m->verticalOriginY -= h;
        m->topSideBearing += (m->leftSideBearing * 0x5700 >> 16) - h;
        m->bottomSideBearing += h - ((int32_t)((m->advanceWidth - (uint32_t)m->rightSideBearing) * 0x5700u) >> 16);
    }
    return 1;
}

/* StreamGlyphOutlines<OutlineTransformWithOffset<1>> 0x180151518 for one glyph */
static void stream_sideways(sink *s, const cmdbuf *b, float scale, float ox, float oy) {
    float cx = 0, cy = 0;
    int open = 0;
    for (uint32_t i = 0; i < b->n;) {
        int32_t op = b->v[i++];
        if (op == CMD_MOVE) {
            if (open) emit(s, IDWR_PATH_END, NULL, 0);
            cx = idwr_addss(ox, -idwr_mulss((float)b->v[i + 1], scale)), cy = idwr_addss(oy, -idwr_mulss((float)b->v[i], scale));
            float p[2] = {cx, cy};
            emit(s, IDWR_PATH_BEGIN, p, 2);
            open = 1, i += 2;
        } else if (op == CMD_LINE) {
            cx = idwr_addss(ox, -idwr_mulss((float)b->v[i + 1], scale)), cy = idwr_addss(oy, -idwr_mulss((float)b->v[i], scale));
            float p[2] = {cx, cy};
            emit(s, IDWR_PATH_LINE, p, 2);
            i += 2;
        } else {
            float qx = idwr_addss(ox, -idwr_mulss((float)b->v[i + 1], scale)), qy = idwr_addss(oy, -idwr_mulss((float)b->v[i], scale));
            float ex = idwr_addss(ox, -idwr_mulss((float)b->v[i + 3], scale)), ey = idwr_addss(oy, -idwr_mulss((float)b->v[i + 2], scale));
            const float two = 0.666666627f, one = 0.333333343f;
            float p[6] = {idwr_addss(idwr_mulss(cx, one), idwr_mulss(qx, two)), idwr_addss(idwr_mulss(cy, one), idwr_mulss(qy, two)),
                          idwr_addss(idwr_mulss(ex, one), idwr_mulss(qx, two)), idwr_addss(idwr_mulss(ey, one), idwr_mulss(qy, two)), ex, ey};
            emit(s, IDWR_PATH_BEZIER, p, 6);
            cx = ex, cy = ey, i += 4;
        }
    }
    if (open) emit(s, IDWR_PATH_END, NULL, 0);
}

static int run_outline_sideways(const idwr_font *font, const float *coords, uint16_t axes, int simulations,
                                float em_size, const uint16_t *glyphs, const float *advances,
                                const idwr_glyph_offset *offsets, uint32_t count, int right_to_left,
                                idwr_points_fn points, void *ctx, idwr_path_cmd *out, int capacity) {
    uint16_t upem = idwr_u16(font->head + 18);
    sink s = {out, 0, capacity};
    if (count == 0) return 0;
    float p0[1] = {0};
    emit(&s, IDWR_PATH_FILL_WINDING, p0, 0);
    /* MetricsConverter, natural measuring: design units × em / (1 · unitsPerEm) */
    float design = em_size / (1.0f * (float)upem), pen = 0;
    float scale = em_size * 1.52587891e-05f / (float)upem;
    cmdbuf b = {0};
    int err = 0;
    for (uint32_t k = 0; k < count && !err; k++) {
        idwr_glyph_metrics m;
        if (!sideways_metrics(font, coords, axes, simulations, upem, glyphs[k], &m)) { err = -1; break; }
        float w = (float)(int32_t)m.advanceWidth * design;
        float h = (float)(int32_t)m.advanceHeight * design;
        float a = advances ? advances[k] : h;
        float ox = pen;
        if (right_to_left) {
            ox = pen - h;
            pen = pen - a;
        } else {
            pen = pen + a;
        }
        if (!(g_sideways_perturb & 1)) ox = (float)m.verticalOriginY * design + ox;
        float oy = g_sideways_perturb & 2 ? 0.0f : w * 0.5f + 0.0f;
        if (offsets) {
            ox = right_to_left ? ox - offsets[k].advanceOffset : offsets[k].advanceOffset + ox;
            oy = oy - offsets[k].ascenderOffset;
        }
        idwr_glyph_points g;
        memset(&g, 0, sizeof g);
        b.n = 0;
        if (points(ctx, glyphs[k], &g) || commands(&g, &b)) { err = -1; break; }
        if (g_sideways_perturb & 8) stream(&s, &b, scale, ox, oy);
        else stream_sideways(&s, &b, scale, ox, oy);
    }
    free(b.v);
    return err ? -1 : s.n;
}

static int run_outline(const idwr_font *font, const float *coords, uint16_t axes, int simulations, float em_size,
                       const uint16_t *glyphs, const float *advances, const idwr_glyph_offset *offsets, uint32_t count,
                       int sideways, int right_to_left, idwr_points_fn points, idwr_commands_fn words, void *ctx,
                       idwr_path_cmd *out, int capacity) {
    if (!font->head || font->head_len < 54 || em_size <= 0 || em_size != em_size || (!glyphs && count)) return -1;
    /* axes = 0 on a variable font is its default instance: DWrite has no face of a variable font
     * other than an instance (CreateFontFace gives the default one), and ::GetDesignGlyphMetrics,
     * which the origins read under bold and sideways, gives that instance's metrics. They are
     * not hmtx when a USE_MY_METRICS component carries another advance (Roboto Italic VF glyph
     * 189: hmtx 738, the instance's 728 from component 181). */
    float defaults[64]; /* the fvar axes the scaler takes (outline_driver.c MAX_AXES) */
#ifndef IDWR_NEGATIVE_CONTROL_DEFAULTINSTANCE
    if (!axes) {
        int n = idwr_normalize_axes(font, NULL, 0, defaults, 64);
        if (n > 0) coords = defaults, axes = (uint16_t)n;
    }
#else
    (void)defaults;
#endif
    if (sideways)
        return run_outline_sideways(font, coords, axes, simulations, em_size, glyphs, advances, offsets, count,
                                    right_to_left, points, ctx, out, capacity);
    uint16_t upem = idwr_u16(font->head + 18);
    sink s = {out, 0, capacity};
    if (count == 0) return 0;
    float p0[1] = {0};
    emit(&s, IDWR_PATH_FILL_WINDING, p0, 0);
    /* ComputeGlyphOrigins, natural measuring: design units × em / unitsPerEm */
    float design = em_size / (1.0f * (float)upem), pen = 0;
    float scale = em_size * 1.52587891e-05f / (float)upem;
    cmdbuf b = {0};
    int err = 0;
    for (uint32_t k = 0; k < count && !err; k++) {
        float ox, oy = 0;
        if (right_to_left || !advances) {
            /* FontFace::GetGlyphAdvances: HVAR for an instance, not the scaler's metrics */
            int32_t a;
            if (!design_advance(font, coords, axes, glyphs[k], simulations, upem, &a)) { err = -1; break; }
            float adv = (float)a * design;
            if (right_to_left) {
                ox = pen - adv;
                pen -= advances ? advances[k] : adv;
            } else {
                ox = pen;
                pen += adv;
            }
        } else {
            ox = pen;
            pen += advances[k];
        }
        if (offsets) {
            ox = right_to_left ? ox - offsets[k].advanceOffset : offsets[k].advanceOffset + ox;
            oy = oy - offsets[k].ascenderOffset;
        }
        b.n = 0;
        if (words) {
            /* GetOutline's commands as they are (CffRasterizer) */
            const int32_t *w = NULL;
            uint32_t n = 0;
            if (words(ctx, glyphs[k], &w, &n)) { err = -1; break; }
            cmdbuf view = {(int32_t *)w, n, n};
            if (stream(&s, &view, scale, ox, oy)) { err = -1; break; }
            continue;
        }
        idwr_glyph_points g;
        memset(&g, 0, sizeof g);
        if (points(ctx, glyphs[k], &g) || commands(&g, &b)) { err = -1; break; }
        stream(&s, &b, scale, ox, oy);
    }
    free(b.v);
    return err ? -1 : s.n;
}

/* the glyf loader as a points provider: unhinted, font units × 64 */
typedef struct {
    const idwr_font *font;
    outline o;
} glyf_points;

static int hmtx_lsb(const idwr_font *font, uint16_t glyph, int32_t *lsb);

static int glyf_points_fn(void *ctx, uint16_t glyph, idwr_glyph_points *out) {
    glyf_points *p = ctx;
    p->o.points = p->o.contours = 0;
    p->o.capture = 1;
    if (load_glyph(p->font, glyph, &p->o, 0)) return -1;
    /* The scaler's outline is relative to the origin phantom point pp1.x = xMin - lsb (of the
     * USE_MY_METRICS component for a composite, plus its gvar delta): a face whose hmtx lsb
     * differs from the glyf xMin (Font Awesome 4.7: lsb 0, xMin -1) is shifted by lsb - xMin */
    int32_t lsb;
    uint32_t len;
    if (hmtx_lsb(p->font, p->o.metrics_glyph, &lsb)) {
        const uint8_t *g = glyph_data(p->font, p->o.metrics_glyph, &len);
        int32_t pp1 = (g && len >= 10 ? idwr_i16(g + 2) : 0) - lsb + p->o.metrics_dx + p->o.pd[0];
#ifdef IDWR_NEGATIVE_CONTROL_GLYFORIGIN
        pp1 = 0;
#endif
        if (pp1)
            for (uint32_t i = 0; i < p->o.points; i++) p->o.x[i] -= pp1 * 64;
    }
    out->x = p->o.x, out->y = p->o.y, out->on = p->o.on, out->ends = p->o.ends, out->contours = p->o.contours;
    return 0;
}

/* Can the scaler's grid fit move a point of `glyph` off its glyf position? DWrite's outline path
 * (GlyphOutlineRasterizationState::RasterizeGlyph 0x1800901e0) and the design metrics of an
 * instance (TrueTypeFontMetricsBuilder: Implementation::GetMetrics 0x18006a3b0, GetBitmap,
 * RasterizeInternal) both call fs__Contour 0x18000c8f0 with the hinting flag 1, at ppem =
 * unitsPerEm. Each glyph record then goes through the grid-fit branch of fsg_SimpleInnerGridFit
 * 0x18000fc0c or fsg_CompositeInnerGridFit 0x1800120ac (unless INSTCTRL bit 0, G + 0x88, set
 * by the pre-program, inhibits it). That branch:
 *   - scales the points by the global scale, 1 here (font units x 64), when the record's
 *     same-transform flag (glyph data + 0xd4) is set, else in the record's own transformed
 *     space (scl_InitializeChildScaling), and scales them back afterwards;
 *   - rounds the side bearing point to the grid and moves the outline with it: a whole font
 *     unit already in the same space, but a fraction of one in a transformed space;
 *   - runs the glyph's program, if its length is non-zero (itrp_Execute for a simple glyph;
 *     itrp_ExecuteGlyphPgm for a composite, whose instructions sfac_ReadCompositeInstructions
 *     0x180002fd8 reads when any component record has WE_HAVE_INSTRUCTIONS 0x100, ORed by
 *     fsg_ExecuteGlyph 0x18000dbdc).
 * fsg_ExecuteGlyph clears a component's same-transform flag unless its matrix (F2Dot14 << 2,
 * 16.16) has zero off-diagonal terms and both diagonal terms of magnitude 0x10000.
 * So the grid fit leaves every point where the glyf loader puts it unless some record of the
 * glyph tree has a program or a component has another matrix: the glyf loader is exact exactly
 * where this returns 0. Malformed data returns 1 (the scaler decides, as DWrite's does). */
#ifdef IDWR_STANDALONE
static int unit_f2dot14(int32_t v) { return v == 0x4000 || v == -0x4000; }

static int grid_fit_moves(const idwr_font *font, uint16_t glyph, int depth) {
    if (depth > 16) return 1;
    uint32_t len;
    const uint8_t *g = glyph_data(font, glyph, &len);
    if (!g || len == 0) return 0;
    if (len < 10) return 1;
    int16_t contours = idwr_i16(g);
    if (contours >= 0) {
        uint32_t at = 10u + 2u * (uint32_t)contours;
        if (at + 2 > len) return 1;
#ifdef IDWR_NEGATIVE_CONTROL_GLYFPROGRAM
        return 0;
#endif
        return idwr_u16(g + at) != 0;
    }
    uint32_t at = 10;
    uint16_t flags, instr = 0;
    int moves = 0;
    do {
        if (at + 4 > len) return 1;
        flags = idwr_u16(g + at);
        instr |= flags & 0x100;
        if (grid_fit_moves(font, idwr_u16(g + at + 2), depth + 1)) moves = 1;
        uint32_t m = at + 4 + (flags & 1 ? 4 : 2), n = flags & 8 ? 2 : flags & 0x40 ? 4 : flags & 0x80 ? 8 : 0;
        if (m + n > len) return 1;
#ifndef IDWR_NEGATIVE_CONTROL_CHILDTRANSFORM
        /* the component's matrix: one scale, x and y scales, or the 2x2 */
        if ((flags & 8 && !unit_f2dot14(idwr_i16(g + m))) ||
            (flags & 0x40 && !(unit_f2dot14(idwr_i16(g + m)) && unit_f2dot14(idwr_i16(g + m + 2)))) ||
            (flags & 0x80 && !(unit_f2dot14(idwr_i16(g + m)) && idwr_i16(g + m + 2) == 0 && idwr_i16(g + m + 4) == 0 &&
                               unit_f2dot14(idwr_i16(g + m + 6)))))
            moves = 1;
#endif
        at = m + n;
    } while (flags & 0x20);
    if (moves) return 1;
    if (!instr) return 0;
    if (at + 2 > len) return 1;
#ifdef IDWR_NEGATIVE_CONTROL_COMPOSITEPROGRAM
    return 0;
#endif
    return idwr_u16(g + at) != 0;
}

/* the points of GetGlyphRunOutline: the scaler (idwr_outline_scaler_glyph, opened on the first
 * glyph that needs it) where the grid fit can move a point, the glyf loader elsewhere */
typedef struct {
    glyf_points glyf;
    const float *coords;
    uint16_t axes;
    idwr_outline_scaler *scaler;
} routed_points;

static int routed_points_fn(void *ctx, uint16_t glyph, idwr_glyph_points *out) {
    routed_points *r = ctx;
#ifndef IDWR_NEGATIVE_CONTROL_GLYFROUTE
    if (grid_fit_moves(r->glyf.font, glyph, 0)) {
        if (!r->scaler) r->scaler = idwr_outline_scaler_open(r->glyf.font, r->coords, r->axes, 0);
        if (!r->scaler) return -1;
        return idwr_outline_scaler_glyph(r->scaler, glyph, out);
    }
#endif
    return glyf_points_fn(&r->glyf, glyph, out);
}
#endif

int idwr_glyph_run_outline(const idwr_font *font, const float *coords, uint16_t axes, float em_size,
                          const uint16_t *glyphs, const float *advances, const idwr_glyph_offset *offsets,
                          uint32_t count, int sideways, int right_to_left, idwr_path_cmd *out, int capacity) {
#ifdef IDWR_STANDALONE
    routed_points r;
    memset(&r, 0, sizeof r);
    r.glyf.font = font;
    r.glyf.o.coords = axes ? coords : NULL;
    r.glyf.o.axes = axes;
    r.coords = coords, r.axes = axes;
    int n = run_outline(font, coords, axes, 0, em_size, glyphs, advances, offsets, count, sideways, right_to_left,
                        routed_points_fn, NULL, &r, out, capacity);
    idwr_outline_scaler_close(r.scaler);
    glyf_points *p = &r.glyf;
#else
    /* no scaler in this build: the glyph programs cannot run, so the glyf points are used
     * unhinted, which equals DWrite only where grid_fit_moves() is 0 (in a build with the scaler) */
    glyf_points q, *p = &q;
    memset(p, 0, sizeof *p);
    p->font = font;
    p->o.coords = axes ? coords : NULL;
    p->o.axes = axes;
    int n = run_outline(font, coords, axes, 0, em_size, glyphs, advances, offsets, count, sideways, right_to_left,
                        glyf_points_fn, NULL, p, out, capacity);
#endif
    free(p->o.x), free(p->o.y), free(p->o.on), free(p->o.ends);
    return n;
}

int idwr_glyph_run_outline_points(const idwr_font *font, const float *coords, uint16_t axes, int simulations,
                                 float em_size, const uint16_t *glyphs, const float *advances,
                                 const idwr_glyph_offset *offsets, uint32_t count, int sideways, int right_to_left,
                                 idwr_points_fn points, void *ctx, idwr_path_cmd *out, int capacity) {
    if (!points) return -1;
    return run_outline(font, coords, axes, simulations, em_size, glyphs, advances, offsets, count, sideways,
                       right_to_left, points, NULL, ctx, out, capacity);
}

int idwr_glyph_run_outline_commands(const idwr_font *font, const float *coords, uint16_t axes, int simulations,
                                   float em_size, const uint16_t *glyphs, const float *advances,
                                   const idwr_glyph_offset *offsets, uint32_t count, int sideways, int right_to_left,
                                   idwr_commands_fn commands, void *ctx, idwr_path_cmd *out, int capacity) {
    if (!commands) return -1;
    return run_outline(font, coords, axes, simulations, em_size, glyphs, advances, offsets, count, sideways,
                       right_to_left, NULL, commands, ctx, out, capacity);
}

/* ---- metrics of a variable instance (TrueTypeRasterizer::Implementation::GetMetrics) ---- */

/* The bitmap rect fs_FindBitMapSize 0x18006ad78 gives the glyph at ppem = unitsPerEm (26.6 =
 * font units × 64), which Implementation::GetMetrics 0x18006a3b0 maps through GetDesignBounds
 * 0x180069fbc (GetBitmap with bounds only: no scan for this axis-aligned matrix):
 *   fsc_RemoveDups 0x18006c5cc   drops a point equal to the next one (moving the contour start
 *                                up), then the start once more when the last point equals the
 *                                first; a contour reduced to its end point is then skipped:
 *   FindExtrema 0x18006d780      the extremes over the other contours, rounded outward
 *                                (left (xmin + 31) >> 6, right (xmax + 32) >> 6, the same in y),
 *                                and a rect of zero width or height widened by one pixel to the
 *                                right or top. No contour left: the rect is empty at the origin.
 * The rect in font units (x0, y0, x1, y1) into r; 0 when it is empty at the origin. */
static int bitmap_rect(const outline *o, int32_t r[4]) {
    int32_t xmin = 0, xmax = 0, ymin = 0, ymax = 0;
    int first = 1;
    for (uint32_t c = 0, s = 0; c < o->contours; s = (uint32_t)o->ends[c++] + 1) {
        uint32_t e = o->ends[c], k = s;
        if (e < s || e >= o->points) continue;
        for (uint32_t i = s; i < e; i++)
            if (o->x[i] == o->x[i + 1] && o->y[i] == o->y[i + 1]) k++;
        if (k != e && o->x[e] == o->x[s] && o->y[e] == o->y[s]) k++;
        if (k == e) continue;
        if (first) xmin = xmax = o->x[s], ymin = ymax = o->y[s], first = 0;
        for (uint32_t i = s; i <= e; i++) {
            if (o->x[i] > xmax) xmax = o->x[i];
            if (o->x[i] < xmin) xmin = o->x[i];
            if (o->y[i] > ymax) ymax = o->y[i];
            if (o->y[i] < ymin) ymin = o->y[i];
        }
    }
    if (first) return 0;
    r[0] = (int32_t)((uint32_t)xmin + 31) >> 6, r[1] = (int32_t)((uint32_t)ymin + 31) >> 6;
    r[2] = (int32_t)((uint32_t)xmax + 32) >> 6, r[3] = (int32_t)((uint32_t)ymax + 32) >> 6;
#ifndef IDWR_NEGATIVE_CONTROL_EXTREMA
    if (r[0] == r[2]) r[2]++;
    if (r[1] == r[3]) r[3]++;
#endif
    return 1;
}

static int hmtx_lsb(const idwr_font *font, uint16_t glyph, int32_t *lsb) {
    uint32_t hhea_len, hmtx_len;
    const uint8_t *hhea = idwr_font_table(font, IDWR_TAG('h', 'h', 'e', 'a'), &hhea_len);
    const uint8_t *hmtx = idwr_font_table(font, IDWR_TAG('h', 'm', 't', 'x'), &hmtx_len);
    if (!hhea || hhea_len < 36 || !hmtx) return 0;
    uint16_t n = idwr_u16(hhea + 34);
    uint32_t at = glyph < n ? 4u * glyph + 2 : 4u * n + 2u * (glyph - n);
    if (!n || at + 2 > hmtx_len) return 0;
    *lsb = idwr_i16(hmtx + at);
    return 1;
}

#ifdef IDWR_STANDALONE
/* TrueTypeFontMetricsBuilder's scaler path run on the ported scaler: its rasterizer
 * (TrueTypeFontMetricsBuilder::TrueTypeFontMetricsBuilder 0x180051d44: CreateRasterizer for the
 * instance) set up by OpenTypeFontMetricsBuilder::SetDesignTransform 0x1800de59c as
 * NewTransform(12, {upem << 16, 0, 0, upem << 16}, 1, 1, flags 0, ppem 0), then per glyph
 * NewGlyph(glyph, 0) and GetMetrics 0x18006a3b0 (vtable + 0x10, + 0x20 in GetGlyphMetrics
 * 0x18008b410), whose {advance, advance height, vertical origin, lsb, rsb, tsb, bsb} become the
 * DesignGlyphMetrics. 0 when the scaler throws. */
static int scaler_metrics(const idwr_font *font, const float *coords, uint16_t axes, uint16_t glyph,
                          idwr_glyph_metrics *out) {
    idwr_tt *t = idwr_tt_open(font, coords, axes);
    if (!t) return 0;
    idwr_catch c;
    idwr_catch_push(&c);
    if (IDWR_SETJMP(c.buf)) {
        idwr_catch_pop(&c);
        idwr_tt_close(t);
        return 0;
    }
    int32_t u = (int32_t)idwr_u16(font->head + 18) << 16, m[4] = {u, 0, 0, u}, r[8];
    idwr_tt_transform(t, 12, m, 1, 1, 0, 0);
    idwr_tt_glyph_metrics(t, glyph, 0, r);
    idwr_catch_pop(&c);
    idwr_tt_close(t);
    out->leftSideBearing = r[3];
    out->advanceWidth = (uint32_t)r[0];
    out->rightSideBearing = r[4];
    out->topSideBearing = r[5];
    out->advanceHeight = (uint32_t)r[1];
    out->bottomSideBearing = r[6];
    out->verticalOriginY = r[2];
    idwr_design_metrics_16(out);
    return 1;
}
#endif

int idwr_get_design_glyph_metrics_instance(const idwr_font *font, const float *coords, uint16_t axes, uint16_t glyph,
                                          idwr_glyph_metrics *out) {
    if (!axes) return idwr_get_design_glyph_metrics(font, glyph, out);
#if defined(IDWR_STANDALONE) && defined(IDWR_CFF)
    /* OpenTypeCffFontMetricsBuilder: the instance's CffRasterizer (hmtx + HVAR, VVAR) */
    if (idwr_font_is_cff(font)) return idwr_cff_design_glyph_metrics(font, coords, axes, glyph, out);
#endif
    if (idwr_hvar_glyph_metrics(font, coords, axes, glyph, out)) {
        idwr_design_metrics_16(out);
        return 1;
    }
#ifdef IDWR_STANDALONE
    /* a glyph the grid fit can move: the scaler itself (below: the same metrics from the glyf points) */
#ifndef IDWR_NEGATIVE_CONTROL_METRICSROUTE
    if (grid_fit_moves(font, glyph, 0)) return scaler_metrics(font, coords, axes, glyph, out);
#else
    (void)scaler_metrics;
#endif
#endif
    /* the scaler's metrics: phantom points and bounding box of the varied outline */
    outline o = {0};
    o.coords = coords, o.axes = axes, o.capture = 1;
    int ok = 0;
    idwr_glyph_metrics m;
    int32_t lsb;
    uint32_t len;
    if (load_glyph(font, glyph, &o, 0) == 0 && idwr_get_design_glyph_metrics(font, o.metrics_glyph, &m) &&
        hmtx_lsb(font, o.metrics_glyph, &lsb)) {
        const uint8_t *g = glyph_data(font, o.metrics_glyph, &len);
        int32_t xmin = g && len >= 10 ? idwr_i16(g + 2) : 0;
        int32_t pp1 = xmin - lsb + o.metrics_dx + o.pd[0], pp2 = xmin - lsb + (int32_t)m.advanceWidth + o.metrics_dx + o.pd[2];
        int32_t pp3 = m.verticalOriginY + o.pd[5], pp4 = m.verticalOriginY - (int32_t)m.advanceHeight + o.pd[7];
        if (!idwr_get_design_glyph_metrics(font, glyph, out)) goto done;
        out->advanceWidth = (uint32_t)(pp2 - pp1);
        out->advanceHeight = (uint32_t)(pp3 - pp4);
        out->verticalOriginY = pp3;
        {
            /* An empty glyph's box: GetDesignBounds starts from an empty box at the glyph
             * origin (pp1, baseline), so lsb 0 and rsb = advance whatever the hmtx lsb (Open
             * Sans' .notdef: no outline, lsb 193) */
#ifndef IDWR_NEGATIVE_CONTROL_EMPTYBOX
            int32_t ex = pp1;
#else
            int32_t ex = 0;
#endif
            int32_t x0 = ex, x1 = ex, y0 = 0, y1 = 0;
            int32_t r[4];
            if (bitmap_rect(&o, r)) x0 = r[0], y0 = r[1], x1 = r[2], y1 = r[3];
            out->leftSideBearing = x0 - pp1;
            out->rightSideBearing = pp2 - x1;
            out->topSideBearing = pp3 - y1;
            out->bottomSideBearing = y0 - pp4;
        }
        idwr_design_metrics_16(out);
        ok = 1;
    }
done:
    free(o.x), free(o.y), free(o.on), free(o.ends);
    return ok;
}
