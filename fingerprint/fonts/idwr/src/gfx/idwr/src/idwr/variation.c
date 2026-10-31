/*
 * Font variations: the normalized coordinates of an instance (fvar, avar) and the gvar and
 * cvar deltas the TrueType scaler applies to the outline and the CVT, as DWrite.dll
 * 10.0.26100.9711 computes them:
 *   ComputeNormalizedAxisCoordinates            0x18007c8d0 (Fixed16_16 arithmetic)
 *   GlyphOutlineVariationInterpolator::ApplyVariation 0x180008580
 *   CVTVariationInterpolator::ApplyVariation    0x18000771c
 *   TupleVariationInterpolator::*               tuple scalars, packed points and deltas, IUP
 *
 * Deltas accumulate in float; explicit deltas add delta * scalar, inferred ones add the
 * interpolated delta * scalar truncated to int16. The sums are rounded half up at the end.
 * Malformed data leaves the outline or CVT unchanged (DWrite catches the exception).
 */
#include <float.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "idwr.h"
#include "idwr_internal.h"

static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

/* Fixed16_16 with saturation: INT32_MAX and INT32_MIN stand for +inf and -inf */
static int fx_finite(int32_t v) { return v != INT32_MAX && v != INT32_MIN; }
static int32_t fx_neg(int32_t v) { return v == INT32_MAX ? INT32_MIN : v == INT32_MIN ? INT32_MAX : -v; }
static int32_t fx_sat(int64_t v) { return v < INT32_MIN ? INT32_MIN : v > INT32_MAX ? INT32_MAX : (int32_t)v; }
static int32_t fx_sub(int32_t a, int32_t b) {
    if (!fx_finite(a)) return a;
    if (!fx_finite(b)) return fx_neg(b);
    return fx_sat((int64_t)a - b);
}
static int32_t fx_add(int32_t a, int32_t b) {
    if (!fx_finite(a)) return a;
    if (!fx_finite(b)) return b;
    return fx_sat((int64_t)a + b);
}
/* Fixed16_16::Divide: n / d rounded half away from zero */
static int32_t fx_div(int64_t n, int32_t d) {
    if (d == 0) return n < 0 ? INT32_MIN : INT32_MAX;
    int n_inf = (uint64_t)(n + 0x7fffffffffffLL) > 0xfffffffefffeULL;
    int sign_differs = (n < 0) != (d < 0);
    if (!fx_finite(d)) return n_inf ? (sign_differs ? INT32_MIN : INT32_MAX) : 0;
    if (n_inf) return sign_differs ? INT32_MIN : INT32_MAX;
    int64_t half = d / 2;
    return (int32_t)((sign_differs ? n - half : n + half) / d);
}
static float fx_float(int32_t v) { return v == INT32_MIN ? -INFINITY : (float)v * 1.5258789e-05f; }

/* ConvertAxisValuesFloatToFixed */
static int32_t float_to_fixed(float f) {
    float s = f * 65536.0f * 65536.0f;
    int64_t t = s >= 9.2233720e18f || s < -9.2233720e18f || s != s ? INT64_MIN : (int64_t)s;
    return fx_sat((t + 0x8000) >> 16);
}

/* The weight OpenTypeFontFaceBuilder::ReadFontFace (0x18005e0e0) stores for a face: with a STAT table
 * (OpenTypeFace + 0x1b4 / + 0x1b8), FontStyleAttributes::GetWeight (0x1800e43dc) over the face's axis
 * values, which are GetFvarAxisValues' (each fvar axis: the first value with its tag,
 * ConvertAxisValuesFloatToFixed, clamped to [min, max]; else the default). With a 'wght' entry:
 * (v + 0x8000) >> 16 in int, below 1 -> 1, above 999 -> 999. Without one: the OS/2 rule (0x180021fc4,
 * idwr_font_os2_weight). -1 for malformed fvar. Without STAT, variable or not: FontDifferentiator's weight,
 * the OS/2 weight resolved with the weight words of the names (idwr_font_weight, nameweight.c). */
int idwr_face_weight(const idwr_font *font, const idwr_axis_value *values, uint32_t count) {
    uint32_t len;
#ifndef IDWR_NEGATIVE_CONTROL_STATWEIGHT
    if (!idwr_font_has_stat(font)) return idwr_font_weight(font);
#endif
    const uint8_t *fvar = idwr_font_table(font, IDWR_TAG('f', 'v', 'a', 'r'), &len);
    if (!fvar) return idwr_font_os2_weight(font);
    if (len < 16) return -1;
    uint16_t offset = idwr_u16(fvar + 4), axes = idwr_u16(fvar + 8), size = idwr_u16(fvar + 10);
    if (size < 20) return -1;
    for (uint16_t i = 0; i < axes; i++) {
        uint32_t at = offset + (uint32_t)i * size;
        if (at + 20 > len) return -1;
        const uint8_t *r = fvar + at;
        if (be32(r) != IDWR_TAG('w', 'g', 'h', 't')) continue;
        int32_t v = (int32_t)be32(r + 8);
        for (uint32_t k = 0; k < count; k++) {
            if (values[k].tag != be32(r)) continue;
            int32_t lo = (int32_t)be32(r + 4), hi = (int32_t)be32(r + 12);
            v = float_to_fixed(values[k].value);
            if (v < lo) v = lo;
            if (hi < v) v = hi;
            break;
        }
#ifdef IDWR_NEGATIVE_CONTROL_VARWEIGHT
        return idwr_font_os2_weight(font);   /* the OS/2 weight even with a wght axis */
#endif
        int32_t w = (int32_t)((uint32_t)v + 0x8000u) >> 16;
        return w < 1 ? 1 : w > 999 ? 999 : w;
    }
    return idwr_font_os2_weight(font);
}

int idwr_normalize_axes(const idwr_font *font, const idwr_axis_value *values, uint32_t count, float *coords,
                       int capacity) {
    uint32_t len, alen;
    const uint8_t *fvar = idwr_font_table(font, IDWR_TAG('f', 'v', 'a', 'r'), &len);
    if (!fvar) return 0;
    if (len < 16) return -1;
    uint16_t offset = idwr_u16(fvar + 4), axes = idwr_u16(fvar + 8), size = idwr_u16(fvar + 10);
    if (size < 20) return -1;
    if (axes > capacity) return -1;
    int32_t *fixed = malloc(sizeof *fixed * (axes ? axes : 1));
    if (!fixed) return -1;
    /* GetFvarAxisValues: the first value with the axis tag, clamped to [min, max]; else the default */
    for (uint16_t i = 0; i < axes; i++) {
        uint32_t at = offset + (uint32_t)i * size;
        if (at + 20 > len) { free(fixed); return -1; }
        const uint8_t *r = fvar + at;
        int32_t v = (int32_t)be32(r + 8);
        for (uint32_t k = 0; k < count; k++) {
            if (values[k].tag != be32(r)) continue;
            int32_t lo = (int32_t)be32(r + 4), hi = (int32_t)be32(r + 12);
            v = float_to_fixed(values[k].value);
            if (v < lo) v = lo;
            if (hi < v) v = hi;
            break;
        }
        int32_t def = (int32_t)be32(r + 8);
        if (v < def) {
            int32_t t = fx_div((int64_t)fx_neg(fx_sub(def, v)) << 16, fx_sub(def, (int32_t)be32(r + 4)));
            fixed[i] = t < -0x10000 ? -0x10000 : t > 0 ? 0 : t;
        } else if (def < v) {
            int32_t t = fx_div((int64_t)fx_sub(v, def) << 16, fx_sub((int32_t)be32(r + 12), def));
            fixed[i] = t < 0 ? 0 : t > 0x10000 ? 0x10000 : t;
        } else {
            fixed[i] = 0;
        }
    }
    const uint8_t *avar = idwr_font_table(font, IDWR_TAG('a', 'v', 'a', 'r'), &alen);
    if (avar && alen) {
        if (alen < 8 || idwr_u16(avar + 6) != axes) { free(fixed); return -1; }
        const uint8_t *p = avar + 8;
        uint32_t left = alen - 8;
        for (uint16_t i = 0; i < axes; i++) {
            if (left < 2) { free(fixed); return -1; }
            uint16_t n = idwr_u16(p);
            p += 2, left -= 2;
            if (n != 0) {
                if (n < 3 || left / 4 < n) { free(fixed); return -1; }
                int32_t prev_from = idwr_i16(p) * 4;
                for (uint16_t j = 1; j < n; j++) {
                    int32_t from = idwr_i16(p + 4 * j) * 4;
                    if (fixed[i] <= from) {
                        int32_t to0 = idwr_i16(p + 4 * j - 2) * 4, to1 = idwr_i16(p + 4 * j + 2) * 4;
                        int32_t t = fx_div((int64_t)fx_sub(fixed[i], prev_from) << 16, fx_sub(from, prev_from));
                        int32_t r = fx_add(fx_sat(((int64_t)fx_sub(to1, to0) * t + 0x8000) >> 16), to0);
                        fixed[i] = r < -0x10000 ? -0x10000 : r > 0x10000 ? 0x10000 : r;
                        break;
                    }
                    prev_from = from;
                }
            }
            p += 4 * (uint32_t)n, left -= 4 * (uint32_t)n;
        }
    }
    for (uint16_t i = 0; i < axes; i++) coords[i] = fx_float(fixed[i]);
    free(fixed);
    return axes;
}

/* ---- tuple variation store ---- */

typedef struct { const uint8_t *p; uint32_t n; } span;

/* cvttss2si: out of range and NaN give INT32_MIN */
static int32_t trunc_i32(float f) { return f >= 2147483648.0f || f < -2147483648.0f || f != f ? INT32_MIN : (int32_t)f; }

/* nearest (ties to even), then a tie rounded down goes up: half up */
static int32_t round_half_up(float f) {
    float r = nearbyintf(f);
    int32_t i = r >= 2147483648.0f || r < -2147483648.0f || r != r ? INT32_MIN : (int32_t)r;
    return (float)i - f == -0.5f ? i + 1 : i;
}

typedef struct { uint16_t *v; uint32_t n, cap; } u16vec;
typedef struct { int16_t *v; uint32_t n, cap; } i16vec;

static int u16_push(u16vec *a, uint16_t x) {
    if (a->n == a->cap) {
        uint32_t cap = a->cap ? a->cap * 2 : 64;
        uint16_t *v = realloc(a->v, cap * sizeof *v);
        if (!v) return -1;
        a->v = v, a->cap = cap;
    }
    a->v[a->n++] = x;
    return 0;
}
static int i16_push(i16vec *a, int16_t x) {
    if (a->n == a->cap) {
        uint32_t cap = a->cap ? a->cap * 2 : 64;
        int16_t *v = realloc(a->v, cap * sizeof *v);
        if (!v) return -1;
        a->v = v, a->cap = cap;
    }
    a->v[a->n++] = x;
    return 0;
}

/* ReadPackedPoints: an empty list means every point */
static int read_points(span s, uint32_t *off, u16vec *out) {
    out->n = 0;
    if (*off >= s.n) return -1;
    uint16_t count;
    if (s.p[*off] & 0x80) {
        if (s.n - *off < 2) return -1;
        count = idwr_u16(s.p + *off) & 0x7fff;
        *off += 2;
    } else {
        count = s.p[(*off)++];
    }
    uint16_t point = 0;
    uint8_t run = 0, words = 0;
    while (out->n < count) {
        if (run == 0) {
            if (*off >= s.n) return -1;
            uint8_t c = s.p[(*off)++];
            run = (c & 0x7f) + 1, words = c >> 7;
        }
        if (words) {
            if (s.n - *off < 2) return -1;
            point += idwr_u16(s.p + *off);
            *off += 2;
        } else {
            if (*off >= s.n) return -1;
            point += s.p[(*off)++];
        }
        if (u16_push(out, point)) return -1;
        run--;
    }
    return 0;
}

/* ReadPackedDeltas: stops as soon as `count` deltas are read, even inside a run */
static int read_deltas(span s, uint32_t *off, uint32_t count, i16vec *out) {
    out->n = 0;
    uint8_t run = 0, zero = 0, words = 0;
    while (out->n < count) {
        if (run == 0) {
            if (*off >= s.n) return -1;
            uint8_t c = s.p[(*off)++];
            zero = c >> 7, words = c >> 6 & 1, run = (c & 0x3f) + 1;
        }
        int16_t d = 0;
        if (!zero) {
            if (words) {
                if (*off > s.n || s.n - *off < 2) return -1;
                d = idwr_i16(s.p + *off);
                *off += 2;
            } else {
                if (*off >= s.n) return -1;
                d = (int8_t)s.p[(*off)++];
            }
        }
        if (i16_push(out, d)) return -1;
        run--;
    }
    return 0;
}

/* ComputeTupleScalar */
static float tuple_scalar(const float *coords, const uint8_t *peak, const uint8_t *inter, uint16_t axes) {
    float scalar = 1.0f;
    for (uint16_t i = 0; i < axes; i++) {
        float c = coords[i], p = (float)idwr_i16(peak + 2 * i) * 6.1035156e-05f, lo = 0, hi = 0;
        if (inter) {
            lo = (float)idwr_i16(inter + 2 * i) * 6.1035156e-05f;
            hi = (float)idwr_i16(inter + 2 * (axes + i)) * 6.1035156e-05f;
        }
        int lo_pos = (lo >= 0 && p >= 0) || lo > 0;
        int hi_pos = (hi >= 0 && p >= 0) || hi > 0;
        if (p == 0 || (inter && !(lo_pos == hi_pos && lo <= p && p <= hi))) continue;
        if (c == 0) return 0;
        float f;
        if (!inter) {
            if ((c < 0 && c < p) || (c > 0 && p < c)) return 0;
            f = c / p;
        } else {
            if (c < lo || hi < c) return 0;
            f = p <= c ? (p < c ? (hi - c) / (hi - p) : 1.0f) : (c - lo) / (p - lo);
        }
        scalar *= f;
    }
    return scalar;
}

/* the delta of point `p` in a tuple's list (binary search), 0 if the point is not listed */
static int16_t listed_delta(const u16vec *pts, const i16vec *d, uint16_t p) {
    uint32_t lo = 0, hi = pts->n;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        if (pts->v[mid] < p) lo = mid + 1;
        else if (pts->v[mid] > p) hi = mid;
        else return d->v[mid];
    }
    return 0;
}

/* AccumulateInterpolatedTupleDeltasInternal: points from `p` up to `ref2` (exclusive, wrapping
 * inside [start, end]) between the touched points ref1 and ref2 */
static void infer_span(float *acc, float scalar, const u16vec *pts, const i16vec *d, const int32_t *coord,
                       uint16_t ref1, uint16_t ref2, uint16_t p, uint16_t start, uint16_t end) {
    float c1 = (float)coord[ref1], c2 = (float)coord[ref2], lo, hi;
    int32_t dlo, dhi;
    if (c2 < c1) lo = c2, dlo = listed_delta(pts, d, ref2), hi = c1, dhi = listed_delta(pts, d, ref1);
    else lo = c1, dlo = listed_delta(pts, d, ref1), hi = c2, dhi = listed_delta(pts, d, ref2);
    float range = hi - lo;
    int32_t dd = dhi - dlo, base = dlo;
    if (fabsf(range) < 1e-08f) {
        lo = FLT_MAX;
        if (dd != 0) base = 0;
    }
    do {
        float c = (float)coord[p], v;
        if (c <= lo) v = (float)base;
        else if (hi <= c) v = (float)dhi;
        else v = ((c - lo) * (float)dd) / range + (float)base;
        acc[p] = (float)(int16_t)trunc_i32(v * scalar) + acc[p];
        if (++p > end) p = start;
    } while (p != ref2);
}

/* AccumulateInterpolatedTupleDeltas: every contour with at least one listed point */
static void infer(float *acc, float scalar, const u16vec *pts, const i16vec *d, const int32_t *coord,
                  const int16_t *end_points, uint16_t contours) {
    uint32_t k = 0;
    uint16_t start = 0;
    for (uint16_t c = 0; c < contours; c++) {
        uint16_t end = (uint16_t)end_points[c];
        while (k < pts->n && pts->v[k] < start) k++;
        if (k >= pts->n) return;
        uint16_t first = pts->v[k];
        if (first <= end) {
            uint16_t walk = first, last = first, next;
            for (;;) {
                while (k < pts->n && pts->v[k] <= walk) {
                    last = pts->v[k++];
                    if (++walk > end) walk = start;
                    if (walk == first) break;
                }
                if (walk == first) break;
                next = k >= pts->n || pts->v[k] > end ? first : pts->v[k];
                infer_span(acc, scalar, pts, d, coord, last, next, walk, start, end);
                walk = last = next;
                if (next == first) break;
            }
        }
        start = end + 1;
    }
}

typedef struct {
    uint32_t points;
    /* gvar only */
    const int16_t *end_points;
    uint16_t contours;
    const int32_t *x, *y;
    float *ax, *ay;
    /* cvar only */
    float *acc;
    u16vec list;
    i16vec dx, dy;
} accumulator;

/* the per-tuple lambdas of GlyphOutlineVariationInterpolator / CVTVariationInterpolator */
static int apply_tuple(accumulator *a, int gvar, span data, uint32_t off, const u16vec *pts, float scalar) {
    uint32_t n = pts->n ? pts->n : a->points;
    if (read_deltas(data, &off, n, &a->dx)) return -1;
    if (gvar && read_deltas(data, &off, n, &a->dy)) return -1;
    if (!gvar) {
        if (pts->n == 0) {
            uint32_t m = a->dx.n < a->points ? a->dx.n : a->points;
            for (uint32_t i = 0; i < m; i++) a->acc[i] = (float)a->dx.v[i] * scalar + a->acc[i];
        } else {
            for (uint32_t i = 0; i < pts->n; i++)
                if (pts->v[i] < a->points) a->acc[pts->v[i]] = (float)a->dx.v[i] * scalar + a->acc[pts->v[i]];
        }
        return 0;
    }
    if (pts->n == 0) {
        uint32_t mx = a->dx.n < a->points ? a->dx.n : a->points, my = a->dy.n < a->points ? a->dy.n : a->points;
        for (uint32_t i = 0; i < mx; i++) a->ax[i] = (float)a->dx.v[i] * scalar + a->ax[i];
        for (uint32_t i = 0; i < my; i++) a->ay[i] = (float)a->dy.v[i] * scalar + a->ay[i];
        return 0;
    }
    if (pts->n < a->points) {
        infer(a->ax, scalar, pts, &a->dx, a->x, a->end_points, a->contours);
        infer(a->ay, scalar, pts, &a->dy, a->y, a->end_points, a->contours);
    }
    for (uint32_t i = 0; i < pts->n; i++)
        if (pts->v[i] < a->points) a->ax[pts->v[i]] = (float)a->dx.v[i] * scalar + a->ax[pts->v[i]];
    for (uint32_t i = 0; i < pts->n; i++)
        if (pts->v[i] < a->points) a->ay[pts->v[i]] = (float)a->dy.v[i] * scalar + a->ay[pts->v[i]];
    return 0;
}

/* AccumulateVariationInternal: walks the tuple variation headers */
static int accumulate(accumulator *a, int gvar, uint16_t axes, uint16_t tuples, const float *coords, span headers,
                      span data, span shared) {
    u16vec shared_pts = {0}, private_pts = {0};
    int err = -1;
    if (tuples & 0x8000) {
        uint32_t off = 0;
        if (read_points(data, &off, &shared_pts)) goto done;
        data.p += off, data.n -= off;
    }
    for (uint16_t t = 0; t < (tuples & 0xfff); t++) {
        if (headers.n < 4) goto done;
        uint16_t size = idwr_u16(headers.p), index = idwr_u16(headers.p + 2);
        uint32_t embedded = index & 0x8000 ? 2u * axes : 0;
        uint32_t header_len = 4 + embedded + (index & 0x4000 ? 4u * axes : 0);
        const uint8_t *peak = NULL, *inter = NULL;
        if (index & 0x8000) {
            if (headers.n - 4 < 2u * axes) goto done;
            peak = headers.p + 4;
        } else {
            uint32_t at = (uint32_t)(index & 0xfff) * axes;
            if (at < shared.n / 2 && at + axes <= shared.n / 2) peak = shared.p + 2 * at;
        }
        if (index & 0x4000) {
            if (headers.n < 4 + embedded || headers.n - 4 - embedded < 4u * axes) goto done;
            inter = headers.p + 4 + embedded;
        }
        if (peak) {
            float scalar = tuple_scalar(coords, peak, inter, axes);
            if (scalar > 0) {
                uint32_t off = 0;
                const u16vec *pts = &shared_pts;
                if (index & 0x2000) {
                    if (read_points(data, &off, &private_pts)) goto done;
                    pts = &private_pts;
                }
                if (apply_tuple(a, gvar, data, off, pts, scalar)) goto done;
            }
        }
        if (headers.n < header_len || data.n < size) goto done;
        headers.p += header_len, headers.n -= header_len;
        data.p += size, data.n -= size;
    }
    err = 0;
done:
    free(shared_pts.v);
    free(private_pts.v);
    return err;
}

int idwr_apply_gvar(const idwr_font *font, const float *coords, uint16_t axes, uint16_t glyph,
                   const int16_t *end_points, uint16_t contours, int32_t *x, int32_t *y, uint32_t points) {
    uint32_t len;
    const uint8_t *gvar = idwr_font_table(font, IDWR_TAG('g', 'v', 'a', 'r'), &len);
    if (!gvar || len == 0 || axes == 0) return 0;
    /* Feature_113402169: end points must increase and stay below the point count */
    uint32_t next = 0;
    for (uint16_t c = 0; c < contours; c++) {
        uint16_t e = (uint16_t)end_points[c];
        if (e < next || points <= e) return -1;
        next = (uint32_t)e + 1;
    }
    /* GetGlyphsByteRange */
    if (len < 20) return -1;
    uint16_t glyphs = idwr_u16(gvar + 12);
    if (glyph >= glyphs) return 0;
    uint32_t start, stop;
    if (gvar[15] & 1) {
        if ((len - 20) / 4 < (uint32_t)glyphs + 1) return -1;
        start = be32(gvar + 20 + 4 * glyph), stop = be32(gvar + 24 + 4 * glyph);
    } else {
        if ((len - 20) / 2 < (uint32_t)glyphs + 1) return -1;
        start = 2u * idwr_u16(gvar + 20 + 2 * glyph), stop = 2u * idwr_u16(gvar + 22 + 2 * glyph);
    }
    uint32_t array = be32(gvar + 16);
    if (stop < start || array > len || stop > len - array) return -1;
    span g = {gvar + array + start, stop - start};
    /* GlyphOutlineVariationInterpolator::ApplyVariation */
    if (idwr_u16(gvar + 4) != axes) return -1;
    if (g.n == 0) return 0;
    if (g.n < 4) return -1;
    uint32_t shared_off = be32(gvar + 8), shared_n = (uint32_t)idwr_u16(gvar + 6) * idwr_u16(gvar + 4);
    if (len < shared_off || (len - shared_off) / 2 < shared_n) return -1;
    uint16_t data_off = idwr_u16(g.p + 2);
    if (data_off > g.n) return -1;
    span headers = {g.p + 4, g.n - 4}, data = {g.p + data_off, g.n - data_off};
    span shared = {gvar + shared_off, 2 * shared_n};

    accumulator a = {.points = points, .end_points = end_points, .contours = contours, .x = x, .y = y};
    a.ax = calloc(points ? points : 1, sizeof(float));
    a.ay = calloc(points ? points : 1, sizeof(float));
    int err = !a.ax || !a.ay ? -1 : accumulate(&a, 1, axes, idwr_u16(g.p), coords, headers, data, shared);
    if (!err)
        for (uint32_t i = 0; i < points; i++) {
            x[i] += round_half_up(a.ax[i]);
            y[i] += round_half_up(a.ay[i]);
        }
    free(a.ax), free(a.ay), free(a.dx.v), free(a.dy.v);
    return err;
}

int idwr_apply_cvar(const idwr_font *font, const float *coords, uint16_t axes, int32_t *cvt, uint32_t count) {
    uint32_t len;
    const uint8_t *cvar = idwr_font_table(font, IDWR_TAG('c', 'v', 'a', 'r'), &len);
    if (!cvar || len == 0 || axes == 0) return 0;
    if (len < 8) return -1;
    uint16_t data_off = idwr_u16(cvar + 6);
    if (data_off < 8 || data_off > len) return -1;
    span headers = {cvar + 8, data_off - 8}, data = {cvar + data_off, len - data_off}, shared = {NULL, 0};
    accumulator a = {.points = count};
    a.acc = calloc(count ? count : 1, sizeof(float));
    int err = !a.acc ? -1 : accumulate(&a, 0, axes, idwr_u16(cvar + 4), coords, headers, data, shared);
    if (!err)
        for (uint32_t i = 0; i < count; i++) cvt[i] += round_half_up(a.acc[i]);
    free(a.acc), free(a.dx.v);
    return err;
}

/* ---- HVAR: ItemVariationInterpolator, HorizontalMetricVariationsTable ---- */

/* ComputeRegionScalar: 0 (inactive) as soon as one axis puts the instance outside the region */
static int region_scalar(const float *coords, const uint8_t *region, uint16_t axes, float *out) {
    *out = 1.0f;
    for (uint16_t i = 0; i < axes; i++) {
        float c = coords[i];
        float s = (float)idwr_i16(region + 6 * i) * 6.1035156e-05f;
        float p = (float)idwr_i16(region + 6 * i + 2) * 6.1035156e-05f;
        float e = (float)idwr_i16(region + 6 * i + 4) * 6.1035156e-05f;
        int s_pos = (s >= 0 && p >= 0) || s > 0, e_pos = (e >= 0 && p >= 0) || e > 0;
        if (!(s <= p && p <= e && s_pos == e_pos && p != 0)) continue;
        if (c == 0 || c < s || e < c) {
            *out = 0;
            return 0;
        }
        float f = p > c ? (c - s) / (p - s) : c > p ? (e - c) / (e - p) : 1.0f;
        *out = f * *out;
    }
    return 1;
}

typedef struct {
    const uint8_t *table;
    uint32_t len, offset, regions_at;
    uint16_t region_axes, regions;
} item_store;

static int store_open(const uint8_t *table, uint32_t len, uint32_t offset, uint16_t axes, item_store *st) {
    if (offset > len || len - offset < 8 || idwr_u16(table + offset) != 1) return -1;
    st->table = table, st->len = len, st->offset = offset;
    st->regions_at = be32(table + offset + 2) + offset;
    if (st->regions_at > len || len - st->regions_at < 4) return -1;
    st->region_axes = idwr_u16(table + st->regions_at), st->regions = idwr_u16(table + st->regions_at + 2);
    if ((len - st->regions_at - 4) / 6 < (uint32_t)st->region_axes * st->regions) return -1;
    if (st->region_axes != axes) return -1; /* SetupRegionScalars */
    return 0;
}

typedef struct {
    uint16_t items, words, regions;
    int longs;
    const uint8_t *indexes;
    uint32_t rows;
} item_data;

/* ItemVariationData `outer` */
static int store_data(const item_store *st, uint16_t outer, item_data *d) {
    const uint8_t *s = st->table + st->offset;
    if (st->len - st->offset < 8 || outer >= idwr_u16(s + 6) || (st->len - st->offset - 8) / 4 <= outer) return -1;
    uint32_t at = be32(s + 8 + 4 * outer) + st->offset;
    if (at > st->len || st->len - at < 6) return -1;
    uint16_t w = idwr_u16(st->table + at + 2);
    d->items = idwr_u16(st->table + at), d->words = w & 0x7fff, d->longs = w >> 15;
    d->regions = idwr_u16(st->table + at + 4);
    if (d->regions < d->words || (st->len - at - 6) / 2 < d->regions) return -1;
    d->indexes = st->table + at + 6;
    d->rows = at + 6 + 2u * d->regions;
    return 0;
}

static int active_region(const item_store *st, const float *coords, uint16_t r, float *scalar) {
    return r < st->regions &&
           region_scalar(coords, st->table + st->regions_at + 4 + 6u * st->region_axes * r, st->region_axes, scalar);
}

/* HorizontalMetricVariationsTable::GetAdvanceDelta: the wide deltas (words, or longs) and the
 * narrow ones (bytes, or words) summed apart, then narrow + wide */
static int16_t advance_delta(const item_store *st, const float *coords, uint16_t outer, uint16_t inner) {
    item_data d;
    if (store_data(st, outer, &d) || inner >= d.items) return 0;
    uint32_t wide = d.longs ? 4 : 2, narrow = d.longs ? 2 : 1;
    uint32_t size = d.words * wide + (uint32_t)(d.regions - d.words) * narrow;
    uint32_t row = d.rows + size * inner;
    if (row > st->len || st->len - row < size) return 0;
    float wsum = 0, nsum = 0, scalar;
    for (uint16_t k = 0; k < d.words; k++) {
        if (!active_region(st, coords, idwr_u16(d.indexes + 2 * k), &scalar)) continue;
        const uint8_t *p = st->table + row + wide * k;
        wsum = wsum + (d.longs ? (float)(int32_t)be32(p) : (float)idwr_i16(p)) * scalar;
    }
    for (uint16_t k = d.words; k < d.regions; k++) {
        if (!active_region(st, coords, idwr_u16(d.indexes + 2 * k), &scalar)) continue;
        const uint8_t *p = st->table + row + wide * d.words + narrow * (k - d.words);
        nsum = nsum + (d.longs ? (float)idwr_i16(p) : (float)(int8_t)p[0]) * scalar;
    }
    return (int16_t)round_half_up(nsum + wsum);
}

/* ItemVariationInterpolator::GetItemDelta (lsb, rsb): one sum in region order, word rows */
static int16_t item_delta(const item_store *st, const float *coords, uint16_t outer, uint16_t inner) {
    item_data d;
    if (store_data(st, outer, &d) || inner >= d.items) return 0;
    uint32_t size = (uint32_t)d.regions + d.words, row = d.rows + size * inner;
    if (row > st->len || st->len - row < size) return 0;
    float sum = 0, scalar;
    for (uint16_t k = 0; k < d.regions; k++) {
        if (!active_region(st, coords, idwr_u16(d.indexes + 2 * k), &scalar)) continue;
        int32_t v = k < d.words ? idwr_i16(st->table + row + 2 * k) : (int8_t)st->table[row + 2 * d.words + (k - d.words)];
        sum = sum + (float)v * scalar;
    }
    return (int16_t)round_half_up(sum);
}

/* ---- MVAR: MetricVariationsTable (constructor 0x18007ae80, GetMetricDelta 0x1800ad7d8) ---- */

/* IsValid: an item store the interpolator accepts, valueRecordSize >= 8, records present */
static int mvar_open(const idwr_font *font, uint16_t axes, const uint8_t **table, uint32_t *len, item_store *st) {
    const uint8_t *m = idwr_font_table(font, IDWR_TAG('M', 'V', 'A', 'R'), len);
    if (!m || *len < 12 || idwr_u16(m) != 1) return 0;
    uint16_t store = idwr_u16(m + 10);
    if (!store || idwr_u16(m + 6) < 8 || idwr_u16(m + 8) == 0 || store_open(m, *len, store, axes, st)) return 0;
    *table = m;
    return 1;
}

int idwr_mvar_valid(const idwr_font *font, uint16_t axes) {
    const uint8_t *m;
    uint32_t len;
    item_store st;
    return axes && mvar_open(font, axes, &m, &len, &st);
}

/* GetMetricDelta: binary search of the value records by tag, then GetItemDelta */
int16_t idwr_mvar_delta(const idwr_font *font, const float *coords, uint16_t axes, uint32_t tag) {
    const uint8_t *m;
    uint32_t len;
    item_store st;
    if (!axes || !mvar_open(font, axes, &m, &len, &st)) return 0;
    uint16_t size = idwr_u16(m + 6), lo = 0, hi = idwr_u16(m + 8);
    while (lo < hi) {
        uint16_t mid = (uint16_t)(((uint32_t)lo + hi) >> 1);
        uint32_t at = (uint32_t)mid * size + 12;
        if (at > len || len - at < 8) return 0;
        uint32_t t = be32(m + at);
        if (t > tag) hi = mid;
        else if (t < tag) lo = (uint16_t)(mid + 1);
        else return item_delta(&st, coords, idwr_u16(m + at + 4), idwr_u16(m + at + 6));
    }
    return 0;
}

/* VariableIndexDecoder, format 0: the last entry repeats; no map is the identity */
static void map_index(const uint8_t *t, uint32_t len, uint32_t map, uint16_t glyph, uint16_t *outer, uint16_t *inner) {
    *outer = 0, *inner = glyph;
    if (!map || map > len || len - map < 4) return;
    uint16_t count = idwr_u16(t + map + 2);
    if (!count) return;
    uint8_t size = (t[map + 1] >> 4 & 3) + 1, bits = (t[map + 1] & 0xf) + 1;
    uint32_t at = map + 4 + (uint32_t)(glyph < count ? glyph : count - 1) * size;
    *outer = 0, *inner = 0;
    if (at > len || len - at < size) return;
    uint32_t v = 0;
    for (uint8_t i = 0; i < size; i++) v = v << 8 | t[at + i];
    *outer = (uint16_t)(v >> bits), *inner = (uint16_t)(v & ((1u << bits) - 1));
}

static int has_map(const uint8_t *t, uint32_t len, uint32_t map) {
    return map && map <= len && len - map >= 4 && idwr_u16(t + map + 2) != 0;
}

/* TrueTypeFontMetricsBuilder::GetGlyphMetrics takes this path when HVAR has an item store,
 * lsb and rsb maps, and the face has no vertical metrics: the default glyph's metrics plus
 * the HVAR deltas. Returns 0 when the scaler's metrics are used instead. */
int idwr_hvar_glyph_metrics(const idwr_font *font, const float *coords, uint16_t axes, uint16_t glyph,
                           idwr_glyph_metrics *out) {
    uint32_t len;
    const uint8_t *hvar = idwr_font_table(font, IDWR_TAG('H', 'V', 'A', 'R'), &len);
    if (!hvar || len < 20 || idwr_u16(hvar) != 1) return 0;
    uint32_t store = be32(hvar + 4), adv_map = be32(hvar + 8), lsb_map = be32(hvar + 12), rsb_map = be32(hvar + 16);
    item_store st;
    if (!store || !has_map(hvar, len, lsb_map) || !has_map(hvar, len, rsb_map) ||
        idwr_font_has_table(font, IDWR_TAG('v', 'm', 't', 'x')) || store_open(hvar, len, store, axes, &st))
        return 0;
    if (!idwr_get_design_glyph_metrics(font, glyph, out)) return 0;
    uint16_t outer, inner;
    map_index(hvar, len, lsb_map, glyph, &outer, &inner);
    out->leftSideBearing += item_delta(&st, coords, outer, inner);
    map_index(hvar, len, rsb_map, glyph, &outer, &inner);
    out->rightSideBearing += item_delta(&st, coords, outer, inner);
    map_index(hvar, len, adv_map, glyph, &outer, &inner);
    out->advanceWidth = (uint32_t)((int32_t)out->advanceWidth + advance_delta(&st, coords, outer, inner));
    return 1;
}

int16_t idwr_metric_variation_delta(const idwr_font *font, uint32_t tag, uint32_t map_field, int map_required,
                                   const float *coords, uint16_t axes, uint16_t glyph) {
    uint32_t len;
    const uint8_t *t = idwr_font_table(font, tag, &len);
    if (!t || len < map_field + 4 || idwr_u16(t) != 1) return 0;
    uint32_t store = be32(t + 4), map = be32(t + map_field);
    item_store st;
    if (!store || store_open(t, len, store, axes, &st)) return 0;
    if (map_required && !has_map(t, len, map)) return 0;
    uint16_t outer, inner;
    map_index(t, len, map, glyph, &outer, &inner);
    return advance_delta(&st, coords, outer, inner);
}

/* FontFace::GetGlyphAdvances of an instance: the default advance plus the HVAR delta, when
 * the face has an HVAR item store; returns 0 otherwise */
int idwr_hvar_advance(const idwr_font *font, const float *coords, uint16_t axes, uint16_t glyph, int32_t *advance) {
    uint32_t len;
    const uint8_t *hvar = idwr_font_table(font, IDWR_TAG('H', 'V', 'A', 'R'), &len);
    if (!hvar || len < 20 || idwr_u16(hvar) != 1) return 0;
    uint32_t store = be32(hvar + 4), adv_map = be32(hvar + 8);
    item_store st;
    idwr_glyph_metrics m;
    if (!store || store_open(hvar, len, store, axes, &st) || !idwr_get_design_glyph_metrics(font, glyph, &m)) return 0;
    uint16_t outer, inner;
    map_index(hvar, len, adv_map, glyph, &outer, &inner);
    /* FontFace::GetGlyphAdvances reads a 16-bit advance (zero-extended) */
#ifndef IDWR_NEGATIVE_CONTROL_WRAP16
    *advance = (uint16_t)((int32_t)m.advanceWidth + advance_delta(&st, coords, outer, inner));
#else
    *advance = (int32_t)m.advanceWidth + advance_delta(&st, coords, outer, inner);
#endif
    return 1;
}
