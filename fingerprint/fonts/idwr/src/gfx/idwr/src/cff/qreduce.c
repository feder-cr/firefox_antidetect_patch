/* The exit of both CFF scan converters, and the QReducer (notes/cff.md, group "qreduce"):
 *
 *   ConvertRuns  0x1800b82e8  run list -> 1-bit rows (CScan's ATMBuildBitMap and ATMQRedCharBitMap)
 *   IFloorModN   0x1800b8664  largest multiple of n at or below x
 *   ICeilModN    0x1800b8694  smallest multiple of n at or above x
 *
 * Written from the observed behaviour and the record layouts, and verified as a black box by
 * swapping it into DWrite.dll (reimpl/cff_check.py), not transcribed.
 *
 * The run list: int16 xmin, xmax, ymin, ymax at +0..+6 (in pixels of the overscaled grid),
 * int32 "has runs" at +8, and at +0x10 a pointer to the rows, from ymin up: per row an int16
 * count, then count pairs [x0, x1) of int16.
 * The bitmap record: +0 bits, +8 capacity (u32), +0xc rowBytes (u32, 0 = not set up yet),
 * +0x10 a qword cleared at set-up, +0x18 rows, +0x1c width, +0x20 x offset, +0x24 y offset,
 * +0x28 cleared. The first call sizes the bitmap to the run box rounded out to multiples of n,
 * width plus `extra` pixels in rows of a multiple of 4 bytes, allocates it with mem[0] if the
 * buffer is too small and clears it. Every call then ORs its rows in, top row first in memory;
 * the first pixel of a byte is its least significant bit. Out-of-range runs are clipped and
 * reported with 0xfe070134 after drawing; records too big for 16-bit coordinates fail with a
 * 0xf907xxxx code, which makes AdobeInternalGetBitmap retry with the QReducer.
 *
 * Negative control: -DCFF_CONTROL_CONVERTRUNS drops the last pixel of runs that end on a byte
 * boundary plus one. */
#include "cff_port.h"

#ifdef IDWR_STANDALONE
#define cff_ConvertRuns_impl cff_ConvertRuns
#define cff_IFloorModN_impl cff_IFloorModN
#define cff_ICeilModN_impl cff_ICeilModN
#endif

/* .rdata 0x18020a340 and 0x18020a338: the bits of a byte from pixel i on, and before pixel i */
static const uint8_t left_bits[8] = {0xff, 0xfe, 0xfc, 0xf8, 0xf0, 0xe0, 0xc0, 0x80};
static const uint8_t right_bits[8] = {0x00, 0x01, 0x03, 0x07, 0x0f, 0x1f, 0x3f, 0x7f};

static int32_t floor_mod_n(int32_t x, uint16_t n) {
    if (x >= 0)
        return (int32_t)((uint32_t)x - (uint32_t)x % n);
    int32_t t = (int32_t)((uint32_t)n - (uint32_t)x - 1);
    return t % (int32_t)n - t;
}

static int32_t ceil_mod_n(int32_t x, uint16_t n) {
    if (x >= 0)
        x = (int32_t)((uint32_t)x - 1 + n);
    return x - x % (int32_t)n;
}

CFF_API int32_t cff_IFloorModN_impl(int32_t x, uint16_t n) { return floor_mod_n(x, n); }
CFF_EXPORT(cff_IFloorModN, cff_IFloorModN_impl);

CFF_API int32_t cff_ICeilModN_impl(int32_t x, uint16_t n) { return ceil_mod_n(x, n); }
CFF_EXPORT(cff_ICeilModN, cff_ICeilModN_impl);

typedef struct {
    int16_t xmin, xmax, ymin, ymax;
    int32_t has_runs;
    int32_t pad;
    const int16_t *runs;
} RunList;

typedef struct {
    uint8_t *bits;
    uint32_t capacity, row_bytes;
    uint64_t cleared;
    int32_t rows, width, x_off, y_off, zero;
} RunBitmap;

typedef void *(*RunAlloc)(uint32_t size);

/* ORs pixels [x0, x1) into a row, x0 < x1 */
static void set_span(uint8_t *row, int16_t x0, int16_t x1) {
    uint8_t *p = row + (x0 >> 3);
    uint8_t right = right_bits[x1 & 7];
    int16_t bytes = (int16_t)((x1 >> 3) - (x0 >> 3));
    if (bytes == 0) {
        *p |= left_bits[x0 & 7] & right;
        return;
    }
    *p++ |= left_bits[x0 & 7];
    int16_t full = (int16_t)(bytes - 1);
    if (full > 0) {
        memset(p, 0xff, (uint16_t)full);
        p += (uint16_t)full;
    }
    if (right)
        *p |= right;
}

CFF_API uint32_t cff_ConvertRuns_impl(const RunList *rl, RunBitmap *bm, uint16_t n, RunAlloc *mem, int32_t extra) {
    if (bm->row_bytes == 0) {
        bm->cleared = 0;
        int32_t width = ceil_mod_n(rl->xmax, n) - floor_mod_n(rl->xmin, n);
        bm->width = width;
        uint32_t rows = (uint32_t)(ceil_mod_n(rl->ymax, n) - floor_mod_n(rl->ymin, n));
        bm->rows = (int32_t)rows;
        if (rows > 0x7fff)
            return 0xf90700ae;
        bm->x_off = -floor_mod_n(rl->xmin, n);
        if (bm->x_off > 0x7fff)
            return 0xf90700b1;
        bm->y_off = -floor_mod_n(rl->ymin, n);
        if (bm->y_off > 0x7fff)
            return 0xf90700b4;
        bm->zero = 0;
        uint32_t row_bytes = ((uint32_t)(extra + 0x1f + width) >> 3) & 0x1ffffffc;
        bm->row_bytes = row_bytes;
        uint32_t size = row_bytes * rows, capacity = bm->capacity;
        if (capacity < size || capacity > 0x7ffffffe) {
            bm->bits = (uint8_t *)(*mem)(size);
            capacity = size;
            if (!bm->bits)
                return 0xfa0700c1;
        }
        bm->capacity = capacity > size ? capacity : size;
        if (size)
            memset(bm->bits, 0, size);
    }
    uint16_t x_off = (uint16_t)bm->x_off;
    if (bm->rows == 0 || rl->has_runs == 0)
        return 0;
    int16_t y_off = (int16_t)bm->y_off;
    if ((uint32_t)(rl->ymin + 0x8000 + y_off) > 0xffff)
        return 0xf90700da;
    int16_t y = (int16_t)(rl->ymin + y_off);
    int32_t y_end = rl->ymax + y_off;
    int clipped = y < 0;
    if ((uint32_t)(y_end + 0x8000) > 0xffff)
        return 0xf90700e1;
    if (bm->rows < (int16_t)y_end) {
        y_end = (uint16_t)bm->rows;
        clipped = 1;
    }
    const int16_t *r = rl->runs;
    int32_t top = ceil_mod_n((int16_t)y_end, n);
    if (top > 0x7fff)
        return 0xf90700ec;
    int16_t width = (int16_t)bm->width;
    uint8_t *row = bm->bits + (uint32_t)(((int16_t)top - y - 1) * bm->row_bytes);
    for (; y < (int16_t)y_end; y++) {
        for (int16_t k = (int16_t)(*r++ - 1); k >= 0; k--) {
            int16_t x0 = (int16_t)(x_off + r[0]), x1 = (int16_t)(x_off + r[1]);
            r += 2;
            if (x0 < 0) {
                x0 = 0;
                clipped = 1;
            }
            if (x1 > width) {
                x1 = width;
                clipped = 1;
            }
#ifdef CFF_CONTROL_CONVERTRUNS
            if ((x1 & 7) == 1)
                x1--;
#endif
            if (y >= 0 && x0 < x1)
                set_span(row, x0, x1);
        }
        row -= bm->row_bytes;
    }
    return clipped ? 0xfe070134 : 0;
}
CFF_EXPORT8(cff_ConvertRuns, cff_ConvertRuns_impl);

/* ---------------------------------------------------------------------------------------------
 * The QReducer: ATMQRedCharBitMap draws a glyph from its outline with "touch" coverage, the way
 * AdobeInternalGetBitmap wants the big bold glyphs (notes/cff.md, "Which scan converter draws
 * a bitmap"). Written from behaviour observed with entry probes (reimpl/cff_qreduce_probe.py,
 * reimpl/oracle/cff_probe.c: the edge records InsertEdge receives) and synthetic outlines
 * played through the original (reimpl/cff_qreduce_synth.py), not from the original's code.
 *
 * The outline: the interpreter feeds the flattened outline through ATMQResetReducer (start
 * over), ATMQNewPoint (the first point after a reset or a close starts a contour, every other
 * point closes an edge from the previous one; FixFltnCurveTo calls it for the curves) and
 * ATMQRdcClose (the edge back to the first point); PlayOutline (ATMCharOutline) stops on a
 * zero return from any of them. The state record keeps, as FixFltnCurveTo reads: +0x528 the
 * flatness, +0x52c "next point starts a contour", +0x530/+0x534 the last point,
 * +0x538/+0x53c the first point, all 16.16.
 *
 * Each edge becomes a record (BuildEdge) with its ends ordered by y (lower end L, upper end H):
 * - an upper end on an integer y moves down one unit, so the edge stops in the row below; an
 *   edge on an integer y with both ends there (horizontal) is dropped;
 * - the end with the larger x moves left one unit when it is on an integer x; a vertical edge
 *   on an integer x moves left one unit and is marked (it lies on a pixel boundary);
 * - rows a = floor(L.y) to top = floor(H.y), and the pixel of each end;
 * - an edge within one row (a = top) keeps its x extent; one within one pixel column keeps
 *   its column; any other steps in fixed point: dx = H.x - L.x and dy = H.y - L.y (both >> 2
 *   when |dx| or dy reaches 2^29), and an error term that starts as
 *       (1 - frac(L.y)) dx - (1 - frac(L.x)) dy   (dx > 0, rounded per product by fixmul)
 *       (1 - frac(L.y)) |dx| - frac(L.x) dy       (dx < 0)
 *   and grows by |dx| per row; the pixel moves one step toward H while the error is >= 0
 *   (dx > 0) or > 0 (dx < 0), taking dy off each time. This is floor(x) at each row boundary,
 *   except where the rounded products land on the other side of a pixel corner.
 *
 * Each row r of the run box (rows min a to max top, x from the smallest to the largest end or
 * first-row pixel) is the union of
 * - every edge's pixels in r: from its pixel at the row's lower boundary (or its lower end) to
 *   its pixel at the upper boundary (or its upper end); a marked vertical adds none, but leaves
 *   an empty range at the pixel to its right (an empty run when nothing else touches it);
 * - the interior's spans (nonzero winding, +1 for an edge going up) at the row's two ends,
 *   from the pixel of the edge that opens a span to that of the edge that closes it, a marked
 *   vertical opening one pixel to the right. The lower end counts the edges crossing the
 *   boundary and those starting exactly on it; the upper end counts those crossing it and
 *   those whose upper end was exactly on it (at their end pixel);
 * - in a row whose only two edges both lie within it (two one-point contours, say), the
 *   pixels between them, as if they opened and closed a span.
 * Runs that touch merge. The pixels and rows are int16, as in the original's records; outlines
 * 32768 pixels or more across overflow the 16.16 differences and are not modelled. The run list's "has runs" word is the number of int16 words of the
 * rows plus one; the caller's box record (io[2]) gets the run box in 16.16, rounded out to
 * multiples of n.
 *
 * Calls go through the original entry points: the interpreter (procs + 0x18) gets the
 * callbacks at their RVAs, so swapped in together these ports replace the QReducer. The
 * original's internal functions (ATMIniQReducer, ATMQReduce, BuildEdge, InsertEdge, the grow
 * functions) are not needed by this model.
 *
 * Negative control: -DCFF_CONTROL_QREDUCE leaves out the pixels of the sloped edges within
 * each row (the spans at the row ends only). */

#ifdef IDWR_STANDALONE
#define cff_ATMQRedCharBitMap_impl cff_ATMQRedCharBitMap
#define cff_ATMQNewPoint_impl cff_ATMQNewPoint
#define cff_ATMQRdcClose_impl cff_ATMQRdcClose
#define cff_ATMQResetReducer_impl cff_ATMQResetReducer
#define cff_InitEdgeList_impl cff_InitEdgeList
#endif
#include <stdlib.h>
#ifdef CFF_CONTROL_QRED_TRACE
#include <stdio.h>
#endif

#define RVA_ATMQNewPoint 0xb2920
#define RVA_ATMQRdcClose 0xea010
#define RVA_ATMQResetReducer 0xf1aa0
#define RVA_FixFltnCurveTo 0xc7140
#define RVA_FFloorFModN 0xb1c1c
#define RVA_ConvertRuns 0xb82e8
#define RVA_fixmul 0xb3340

#define ST_I32(state, off) (*(int32_t *)((uint8_t *)(state) + (off)))

typedef int32_t (*QFixMul)(int32_t a, int32_t b);

enum { QE_ROW, QE_COLUMN, QE_SLOPE };

/* an edge record (see above) */
typedef struct {
    int32_t dx, dy, err;
    int16_t px;           /* pixel at the next row boundary while stepping */
    int16_t a, top;       /* rows of the lower and the upper end */
    int16_t lo, hi;       /* pixels of the edge in row a */
    int16_t start, end;   /* pixels of the lower and the upper end */
    int8_t dir;           /* +1 when the outline goes up along the edge */
    uint8_t kind;
    uint8_t on_x;         /* a vertical on an integer x, moved one unit left */
    uint8_t start_on_y;   /* lower end on an integer y */
    uint8_t end_on_y;     /* upper end was on an integer y */
} QEdge;

/* the outline of the glyph being reduced (one glyph at a time per thread) */
static _Thread_local struct {
    QEdge *e;
    size_t n, cap;
    int failed;
} q;

static int32_t floor_px(int32_t v) { return v >> 16; }

static void add_edge(int32_t x0, int32_t y0, int32_t x1, int32_t y1) {
    QEdge e;
    memset(&e, 0, sizeof e);
    int32_t xl, yl, xh, yh;
    if (y0 < y1) {
        xl = x0, yl = y0, xh = x1, yh = y1;
        e.dir = 1;
    } else {
        xl = x1, yl = y1, xh = x0, yh = y0;
        e.dir = -1;
    }
    if ((yh & 0xffff) == 0) {
        if (yh == yl)
            return;
        yh--;
        e.end_on_y = 1;
    }
    if (xl == xh) {
        if ((xl & 0xffff) == 0) {
            xl--, xh--;
            e.on_x = 1;
        }
    } else if (xl > xh) {
        if ((xl & 0xffff) == 0)
            xl--;
    } else if ((xh & 0xffff) == 0) {
        xh--;
    }
    e.a = floor_px(yl);
    e.top = floor_px(yh);
    e.start = floor_px(xl);
    e.end = floor_px(xh);
    e.start_on_y = (yl & 0xffff) == 0;
    if (e.a == e.top) {
        e.kind = QE_ROW;
        e.lo = xl < xh ? e.start : e.end;
        e.hi = xl < xh ? e.end : e.start;
    } else if (e.start == e.end) {
        e.kind = QE_COLUMN;
        e.lo = e.hi = e.px = e.start;
    } else {
        QFixMul fixmul = CFF_ORIG(QFixMul, RVA_fixmul);
        int32_t dx = (int32_t)((uint32_t)xh - (uint32_t)xl), dy = yh - yl;
        if (dx >= 0x20000000 || dx <= -0x20000000 || dy >= 0x20000000) {
            dx >>= 2;
            dy >>= 2;
        }
        int32_t fy = 0x10000 - (yl & 0xffff), err;
        int16_t px = e.start;
        if (dx > 0) {
            err = fixmul(-(0x10000 - (xl & 0xffff)), dy) - fixmul(-fy, dx);
            while (err >= 0) {
                px++;
                err -= dy;
            }
            e.lo = e.start;
            e.hi = px;
        } else {
            err = fixmul(-fy, dx) + fixmul(-(xl & 0xffff), dy);
            while (err > 0) {
                px--;
                err -= dy;
            }
            e.lo = px;
            e.hi = e.start;
        }
        e.kind = QE_SLOPE;
        e.dx = dx;
        e.dy = dy;
        e.err = err;
        e.px = px;
    }
    if (q.n == q.cap) {
        size_t cap = q.cap ? q.cap * 2 : 256;
        QEdge *p = (QEdge *)realloc(q.e, cap * sizeof *p);
        if (!p) {
            q.failed = 1;
            return;
        }
        q.e = p;
        q.cap = cap;
    }
    q.e[q.n++] = e;
}

/* the pixel at the next row boundary */
static int32_t step_edge(QEdge *e) {
    if (e->kind != QE_SLOPE)
        return e->px;
    if (e->dx > 0) {
        e->err += e->dx;
        while (e->err >= 0) {
            e->px++;
            e->err -= e->dy;
        }
    } else {
        e->err -= e->dx;
        while (e->err > 0) {
            e->px--;
            e->err -= e->dy;
        }
    }
    return e->px;
}

static void reset_outline(void) {
    q.n = 0;
    q.failed = 0;
}

/* PlayOutline stops on a zero return; the original is void and leaves its (non-null) edge
 * buffer pointer in RAX */
CFF_API int32_t cff_ATMQResetReducer_impl(void *state) {
    reset_outline();
    ST_I32(state, 0x52c) = 1;
    return 1;
}
CFF_EXPORT(cff_ATMQResetReducer, cff_ATMQResetReducer_impl);

CFF_API int32_t cff_ATMQNewPoint_impl(const int32_t *pt, void *state) {
    if (ST_I32(state, 0x52c)) {
        ST_I32(state, 0x538) = pt[0];
        ST_I32(state, 0x53c) = pt[1];
        ST_I32(state, 0x52c) = 0;
    } else {
        add_edge(ST_I32(state, 0x530), ST_I32(state, 0x534), pt[0], pt[1]);
        if (q.failed)
            return 0;
    }
    ST_I32(state, 0x530) = pt[0];
    ST_I32(state, 0x534) = pt[1];
    return 1;
}
CFF_EXPORT(cff_ATMQNewPoint, cff_ATMQNewPoint_impl);

CFF_API int32_t cff_ATMQRdcClose_impl(void *state) {
    if (!ST_I32(state, 0x52c)) {
        add_edge(ST_I32(state, 0x530), ST_I32(state, 0x534), ST_I32(state, 0x538), ST_I32(state, 0x53c));
        if (q.failed)
            return 0;
    }
    ST_I32(state, 0x52c) = 1;
    return 1;
}
CFF_EXPORT(cff_ATMQRdcClose, cff_ATMQRdcClose_impl);

/* InitEdgeList 0x1800e45a4, called by the interpreter on a record of the run buffer: count 1,
 * an empty list whose head (+0x50) points at its sentinel (+0x38) with two 0xd8300001 bounds */
CFF_API void cff_InitEdgeList_impl(int32_t *list) {
    list[0x12] = 0;
    list[0x10] = (int32_t)0xd8300001;
    list[0x11] = (int32_t)0xd8300001;
    *(int32_t **)(list + 0x14) = list + 0xe;
    list[0] = 1;
}
CFF_EXPORT(cff_InitEdgeList, cff_InitEdgeList_impl);

/* growable scratch arrays of the reducer */
typedef struct {
    void *p;
    size_t n, cap, size;
} QVec;

static void *vec_push(QVec *v) {
    if (v->n == v->cap) {
        size_t cap = v->cap ? v->cap * 2 : 64;
        void *p = realloc(v->p, cap * v->size);
        if (!p)
            return NULL;
        v->p = p;
        v->cap = cap;
    }
    return (uint8_t *)v->p + v->size * v->n++;
}

/* an edge at one end of a row */
typedef struct {
    int32_t px;
    int8_t dir;
    uint8_t on_x;
} QCross;

typedef struct {
    int32_t a, b; /* pixels a to b, both included */
} QRange;

static int cross_cmp(const void *p, const void *r) {
    const QCross *x = (const QCross *)p, *y = (const QCross *)r;
    return x->px < y->px ? -1 : x->px > y->px;
}

static int range_cmp(const void *p, const void *r) {
    const QRange *x = (const QRange *)p, *y = (const QRange *)r;
    if (x->a != y->a)
        return x->a < y->a ? -1 : 1;
    return x->b < y->b ? -1 : x->b > y->b;
}

static int edge_cmp(const void *p, const void *r) {
    const QEdge *const *x = (const QEdge *const *)p, *const *y = (const QEdge *const *)r;
    if ((*x)->a != (*y)->a)
        return (*x)->a < (*y)->a ? -1 : 1;
    return *x < *y ? -1 : *x > *y;
}

static int push_cross(QVec *v, int32_t px, const QEdge *e) {
    QCross *c = (QCross *)vec_push(v);
    if (!c)
        return 0;
    *c = (QCross){px, e->dir, e->on_x};
    return 1;
}

/* the pixels of an edge in a row, from a to b in either order; a marked vertical touches none
 * of its column but still leaves an empty range at the pixel to its right */
static int push_range(QVec *v, int32_t a, int32_t b, const QEdge *e) {
#ifdef CFF_CONTROL_QREDUCE
    if (e->kind == QE_SLOPE)
        return 1;
#endif
    QRange *g = (QRange *)vec_push(v);
    if (!g)
        return 0;
    if (e->on_x)
        *g = (QRange){a + 1, a};
    else
        *g = a <= b ? (QRange){a, b} : (QRange){b, a};
    return 1;
}

/* the interior's spans along one row end: nonzero winding over the edges sorted by pixel */
static int end_spans(QVec *cross, QVec *ranges) {
    QCross *c = (QCross *)cross->p;
    qsort(c, cross->n, sizeof *c, cross_cmp);
    int32_t w = 0, start = 0;
    for (size_t i = 0; i < cross->n; i++) {
        int32_t was = w;
        w += c[i].dir;
        if (was == 0 && w != 0) {
            start = c[i].px + c[i].on_x;
        } else if (was != 0 && w == 0) {
            QRange *g = (QRange *)vec_push(ranges);
            if (!g)
                return 0;
            *g = (QRange){start, c[i].px};
        }
    }
    return 1;
}

/* appends one int16 word to the rows */
static int put_word(QVec *out, int32_t v) {
    int16_t *w = (int16_t *)vec_push(out);
    if (!w)
        return 0;
    *w = (int16_t)v;
    return 1;
}

/* the rows of runs of the outline in q, from box[2] up to box[3]; returns the int16 words */
static int16_t *reduce(int16_t box[4], uint32_t *words, uint32_t *status) {
    *words = 0;
    *status = 0;
    box[0] = box[1] = box[2] = box[3] = 0;
    if (!q.n)
        return NULL;
    int32_t xmin = INT32_MAX, xmax = INT32_MIN, ymin = INT32_MAX, ymax = INT32_MIN;
    QEdge **order = (QEdge **)malloc(q.n * sizeof *order);
    if (!order) {
        *status = 0xfa0700c1;
        return NULL;
    }
    for (size_t i = 0; i < q.n; i++) {
        QEdge *e = &q.e[i];
        order[i] = e;
        int32_t l = e->lo < e->end ? e->lo : e->end, h = e->hi > e->end ? e->hi : e->end;
        if (l < xmin) xmin = l;
        if (h > xmax) xmax = h;
        if (e->a < ymin) ymin = e->a;
        if (e->top > ymax) ymax = e->top;
    }
    box[0] = (int16_t)xmin;
    box[1] = (int16_t)(xmax + 1);
    box[2] = (int16_t)ymin;
    box[3] = (int16_t)(ymax + 1);
    qsort(order, q.n, sizeof *order, edge_cmp);
    QVec out = {0, 0, 0, sizeof(int16_t)}, ranges = {0, 0, 0, sizeof(QRange)};
    QVec low = {0, 0, 0, sizeof(QCross)}, up = {0, 0, 0, sizeof(QCross)}, act = {0, 0, 0, sizeof(QEdge *)};
    size_t next = 0;
    int ok = 1;
    for (int32_t y = ymin; ok && y <= ymax; y++) {
        while (ok && next < q.n && order[next]->a == y) {
            QEdge **slot = (QEdge **)vec_push(&act);
            if (slot)
                *slot = order[next++];
            else
                ok = 0;
        }
        ranges.n = low.n = up.n = 0;
        QEdge **ae = (QEdge **)act.p;
        size_t keep = 0;
        for (size_t i = 0; ok && i < act.n; i++) {
            QEdge *e = ae[i];
            if (y == e->a) {
                ok = push_range(&ranges, e->lo, e->hi, e);
                if (ok && e->start_on_y)
                    ok = push_cross(&low, e->start, e);
                if (ok && e->a == e->top) {
                    if (e->end_on_y)
                        ok = push_cross(&up, e->end, e);
                } else if (ok) {
                    ok = push_cross(&up, e->px, e);
                }
            } else {
                int32_t p = e->px;
                ok = push_cross(&low, p, e);
                int32_t r = y < e->top ? step_edge(e) : e->end;
                if (ok)
                    ok = push_range(&ranges, p, r, e);
                if (ok && (y < e->top || e->end_on_y))
                    ok = push_cross(&up, r, e);
            }
            if (e->top > y)
                ae[keep++] = e;
        }
        /* a row with just two edges, both within it (two single points, say): filled between */
        if (ok && act.n == 2 && ae[0]->kind == QE_ROW && ae[1]->kind == QE_ROW) {
            const QEdge *l = ae[0]->lo <= ae[1]->lo ? ae[0] : ae[1], *r = l == ae[0] ? ae[1] : ae[0];
            QRange *g = (QRange *)vec_push(&ranges);
            if (g)
                *g = (QRange){l->lo + l->on_x, r->lo};
            else
                ok = 0;
        }
        act.n = keep;
        ok = ok && end_spans(&low, &ranges) && end_spans(&up, &ranges);
        if (!ok)
            break;
        QRange *rg = (QRange *)ranges.p;
        qsort(rg, ranges.n, sizeof *rg, range_cmp);
        size_t m = 0;
        for (size_t i = 0; i < ranges.n; i++) {
            if (m && rg[i].a <= rg[m - 1].b + 1) {
                if (rg[i].b > rg[m - 1].b)
                    rg[m - 1].b = rg[i].b;
            } else {
                rg[m++] = rg[i];
            }
        }
        ok = put_word(&out, (int32_t)m);
        for (size_t i = 0; ok && i < m; i++)
            ok = put_word(&out, rg[i].a) && put_word(&out, rg[i].b + 1);
    }
    ok = ok && put_word(&out, 0); /* the word the count includes */
    free(order);
    free(ranges.p);
    free(low.p);
    free(up.p);
    free(act.p);
    if (!ok) {
        free(out.p);
        *status = 0xfa0700c1;
        return NULL;
    }
    *words = (uint32_t)out.n;
    return (int16_t *)out.p;
}

typedef int32_t (*QInterpret)(void *state, void *mem, void *charstring, void *io, void *const *procs, uint32_t flags,
                              void *arg6);
typedef int32_t (*QFloorModN)(int32_t v, uint16_t n);

typedef struct {
    int16_t xmin, xmax, ymin, ymax;
    int32_t has_runs;
    int32_t pad;
    const int16_t *runs;
} QRunList;

typedef uint32_t (*QConvertRuns)(const QRunList *rl, void *bitmap, uint16_t n, void *mem, int32_t extra);

CFF_API uint32_t cff_ATMQRedCharBitMap_impl(uint16_t *state, void *mem, void *charstring, void **io, void *bitmap,
                                            void *arg6, uint32_t flags, uint8_t *procs) {
    void *const callbacks[6] = {idwr_orig(RVA_ATMQResetReducer), idwr_orig(RVA_ATMQNewPoint), idwr_orig(RVA_ATMQNewPoint),
                                idwr_orig(RVA_FixFltnCurveTo), idwr_orig(RVA_ATMQRdcClose), NULL};
    reset_outline();
    *(void **)((uint8_t *)state + 0x4b8) = mem; /* as ATMIniQReducer leaves it */
    ST_I32(state, 0x528) = ST_I32(state, 4);
    uint32_t status = (uint32_t)(*(QInterpret *)(procs + 0x18))(state, mem, charstring, io, callbacks, flags & 3, arg6);
#ifdef CFF_CONTROL_QRED_TRACE
    fprintf(stderr, "qred: interp %#x edges %zu\n", status, q.n);
#endif
    if (status == 0) {
        int16_t box[4];
        uint32_t words;
        int16_t *runs = reduce(box, &words, &status);
        if (status == 0) {
            QRunList rl = {box[0], box[1], box[2], box[3], (int32_t)words, 0, runs};
            int32_t *bb = (int32_t *)io[2];
            uint16_t n = state[0];
            bb[0] = (int32_t)((uint32_t)box[0] << 16);
            bb[1] = (int32_t)((uint32_t)box[2] << 16);
            bb[2] = (int32_t)((uint32_t)box[1] << 16);
            bb[3] = (int32_t)((uint32_t)box[3] << 16);
            if (n != 1) {
                QFloorModN fl = CFF_ORIG(QFloorModN, RVA_FFloorFModN);
                bb[0] = fl(bb[0], n);
                bb[1] = fl(bb[1], n);
                bb[2] = fl((int32_t)((uint32_t)n * 0x10000 + (uint32_t)bb[2] - 1), n);
                bb[3] = fl((int32_t)((uint32_t)n * 0x10000 + (uint32_t)bb[3] - 1), n);
            }
            status = CFF_ORIG(QConvertRuns, RVA_ConvertRuns)(&rl, bitmap, n, mem, 0);
#ifdef CFF_CONTROL_QRED_TRACE
            fprintf(stderr, "qred: edges %zu box %d %d %d %d words %u status %#x n %u\n", q.n, box[0], box[1], box[2], box[3], words, status, n);
#endif
        }
        free(runs);
        memset((uint8_t *)state + 0x4b8, 0, 8);
    }
    reset_outline();
    return status;
}
CFF_EXPORT8(cff_ATMQRedCharBitMap, cff_ATMQRedCharBitMap_impl);
