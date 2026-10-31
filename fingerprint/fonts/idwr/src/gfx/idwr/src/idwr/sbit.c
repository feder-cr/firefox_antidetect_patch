/*
 * Embedded bitmap strikes (EBLC/EBDT): when DWrite draws a glyph from a strike, and the strike
 * bitmap itself. The rules are the output model of notes/rasterizer.md "Embedded bitmap
 * strikes" (scripts/bitmap-strikes.py), the formats are the OpenType specification's.
 *   strike state: grid fit enabled, then legacy East Asian face or aliased mode, then a
 *                 BitmapSize with ppemX = ppemY = ppem and bitDepth 1; the glyph is drawn from
 *                 it when it has image data and the vertical overscale is 1
 *   bitmap:       index subtable formats 1-5, image formats 1, 2, 5, 6, 7 (1 bit per pixel) and
 *                 the composites 8 and 9 (GetSbitComponent's assembly, below);
 *                 drawn at (bearingX, -bearingY) from the origin, 255 where a bit is set
 */
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "idwr.h"
#include "idwr_internal.h"

static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

static int32_t round_half_up(float x) {
    int32_t r = idwr_cvtss2si(x);
    return (float)r - x == -0.5f ? r + 1 : r;
}

typedef struct {
    uint8_t format;   /* image format */
    uint32_t offset;  /* in EBDT */
    uint32_t size;
    const uint8_t *metrics; /* EBLC small metrics for formats 2 and 5 of the index, else NULL */
} sbit_entry;

/* the strike (BitmapSize record) for `ppem`, or NULL */
static const uint8_t *find_strike(const uint8_t *eblc, uint32_t len, int32_t ppem) {
    if (!eblc || len < 8) return NULL;
    uint32_t n = be32(eblc + 4);
    for (uint32_t i = 0; i < n && 8 + 48 * (uint64_t)(i + 1) <= len; i++) {
        const uint8_t *b = eblc + 8 + 48 * i;
        if (b[44] == ppem && b[45] == ppem && b[46] == 1) return b;
    }
    return NULL;
}

/* the glyph's image data in a strike: 1 if present */
static int strike_entry(const uint8_t *eblc, uint32_t len, const uint8_t *strike, uint16_t glyph, sbit_entry *e) {
    uint32_t array = be32(strike), count = be32(strike + 8);
    for (uint32_t k = 0; k < count; k++) {
        uint32_t at = array + 8 * k;
        if ((uint64_t)at + 8 > len) return 0;
        uint16_t first = idwr_u16(eblc + at), last = idwr_u16(eblc + at + 2);
        if (glyph < first || glyph > last) continue;
        uint32_t sub = array + be32(eblc + at + 4);
        if ((uint64_t)sub + 8 > len) return 0;
        uint16_t fmt = idwr_u16(eblc + sub);
        uint32_t data = be32(eblc + sub + 4), j = glyph - first;
        e->format = (uint8_t)idwr_u16(eblc + sub + 2);
        e->metrics = NULL;
        if (fmt == 1 || fmt == 3) {
            uint32_t w = fmt == 1 ? 4 : 2, o = sub + 8 + w * j;
            if ((uint64_t)o + 2 * w > len) return 0;
            uint32_t a = fmt == 1 ? be32(eblc + o) : idwr_u16(eblc + o);
            uint32_t b = fmt == 1 ? be32(eblc + o + 4) : idwr_u16(eblc + o + 2);
            if (b <= a) return 0;
            e->offset = data + a, e->size = b - a;
            return 1;
        }
        if (fmt == 2) {
            if ((uint64_t)sub + 20 > len) return 0;
            e->size = be32(eblc + sub + 8);
            e->offset = data + j * e->size;
            e->metrics = eblc + sub + 12;
            return 1;
        }
        if (fmt == 4) {
            if ((uint64_t)sub + 12 > len) return 0;
            uint32_t num = be32(eblc + sub + 8);
            for (uint32_t i = 0; i < num && (uint64_t)sub + 12 + 4 * (i + 2) <= len; i++) {
                const uint8_t *p = eblc + sub + 12 + 4 * i;
                if (idwr_u16(p) != glyph) continue;
                uint16_t a = idwr_u16(p + 2), b = idwr_u16(p + 6);
                if (b <= a) return 0;
                e->offset = data + a, e->size = (uint32_t)(b - a);
                return 1;
            }
            return 0;
        }
        if (fmt == 5) {
            if ((uint64_t)sub + 24 > len) return 0;
            uint32_t size = be32(eblc + sub + 8), num = be32(eblc + sub + 20);
            for (uint32_t i = 0; i < num && (uint64_t)sub + 24 + 2 * (i + 1) <= len; i++) {
                if (idwr_u16(eblc + sub + 24 + 2 * i) != glyph) continue;
                e->size = size, e->offset = data + i * size, e->metrics = eblc + sub + 12;
                return 1;
            }
            return 0;
        }
        return 0;
    }
    return 0;
}

/* union of the glyph ranges of every strike contains `glyph` */
static int in_strike_ranges(const uint8_t *eblc, uint32_t len, uint16_t glyph) {
    uint32_t n = be32(eblc + 4);
    for (uint32_t i = 0; i < n && 8 + 48 * (uint64_t)(i + 1) <= len; i++) {
        const uint8_t *b = eblc + 8 + 48 * i;
        uint32_t array = be32(b), count = be32(b + 8);
        for (uint32_t k = 0; k < count && (uint64_t)array + 8 * (k + 1) <= len; k++)
            if (glyph >= idwr_u16(eblc + array + 8 * k) && glyph <= idwr_u16(eblc + array + 8 * k + 2)) return 1;
    }
    return 0;
}

/* OpenTypeFontFaceBuilder::IsLegacyEastAsian: four groups of six test characters */
static const uint32_t LEGACY_EA_GROUPS[4][6] = {
    {0x3044, 0x3046, 0x3093, 0x3057, 0x306e, 0x304b}, {0xc774, 0xb2e4, 0xb294, 0xc758, 0xc5d0, 0xd558},
    {0x6c49, 0x5b57, 0x4e2d, 0x7684, 0x4e2a, 0x4eec}, {0x6f22, 0x5011, 0x4e09, 0x4e86, 0x5b78, 0x7232}};

int idwr_is_legacy_east_asian(const idwr_font *font) {
    uint32_t len;
    const uint8_t *eblc = idwr_font_table(font, IDWR_TAG('E', 'B', 'L', 'C'), &len);
    if (!eblc || len < 8 || be32(eblc + 4) == 0) return 0;
    if (!idwr_font_has_table(font, IDWR_TAG('g', 'l', 'y', 'f')) && !idwr_font_has_table(font, IDWR_TAG('C', 'F', 'F', ' ')))
        return 1;
    for (int g = 0; g < 4; g++) {
        int all = 1;
        for (int c = 0; c < 6 && all; c++) {
            uint16_t gid = idwr_font_glyph_index(font, LEGACY_EA_GROUPS[g][c]);
            all = gid && in_strike_ranges(eblc, len, gid);
        }
        if (all) return 1;
    }
    return 0;
}

static int gdi_compatible(int mode) {
    return mode == IDWR_MODE_ALIASED || mode == IDWR_MODE_GDI_CLASSIC || mode == IDWR_MODE_GDI_NATURAL;
}

/* the strike DWrite uses at this size and mode (grid fit DEFAULT, no transform), or NULL */
static const uint8_t *active_strike(const idwr_font *font, int mode, float em, const uint8_t **eblc, uint32_t *len) {
    *eblc = idwr_font_table(font, IDWR_TAG('E', 'B', 'L', 'C'), len);
    if (!*eblc) return NULL;
    int32_t ppem = round_half_up(em);
    if (!gdi_compatible(mode)) {
        int set = idwr_gasp_flag(font, 4 /* SYMMETRIC_GRIDFIT */, ppem < 1 ? 1 : ppem);
        if (set == 0) return NULL; /* grid fit disabled by gasp */
    }
    if (!(mode == IDWR_MODE_ALIASED || idwr_is_legacy_east_asian(font))) return NULL;
    int force_int = font->head && font->head_len >= 18 && (idwr_u16(font->head + 16) & 8);
    if (!gdi_compatible(mode) && !force_int) ppem = idwr_cvttss2si(em); /* 16.16 em size, integer part */
    return find_strike(*eblc, *len, ppem);
}

int idwr_strike_state(const idwr_font *font, int mode, float em, uint16_t glyph) {
    const uint8_t *eblc;
    uint32_t len;
    const uint8_t *strike = active_strike(font, mode, em, &eblc, &len);
    if (!strike) return IDWR_STRIKE_NONE;
    sbit_entry e;
    if (mode != IDWR_MODE_NATURAL_SYMMETRIC && strike_entry(eblc, len, strike, glyph, &e)) return IDWR_STRIKE_GLYPH;
    return IDWR_STRIKE_ACTIVE;
}

/* ---- composite glyphs (image formats 8 and 9), as sbit_GetBitmap 0x1800051a0 assembles them ----
 * GetSbitComponent 0x18000558c draws the glyph (sfac_GetSbitBitmap 0x180005a28) into the composite's
 * buffer (row bytes ((width + 31) >> 3) & ~3, as sbit_GetMetrics 0x1800017c4 sets them at rotation 0,
 * times the height) at (x, y), then for each component (sfac_GetSbitComponentInfo 0x1800061cc: glyph,
 * then xOffset and yOffset read as unsigned bytes) finds it in the same strike (sfac_SearchForBitmap;
 * none: 0x1801), reads its metrics, shaves an image format 5 component (sfac_ShaveSbitMetrics 0x180003f70)
 * and recurses at (x + xOffset + shaved left, y + yOffset + shaved top), at most 20 deep (0x140b).
 * sfac_GetSbitBitmap fails with 0x1a00 when the glyph's rows (from y) pass the buffer or its bytes (from
 * x) pass the row; byte-aligned rows (formats 1, 6) are ORed whole bytes (shifted by x & 7, with the
 * byte that carries the shift), bit-aligned ones (2, 5, 7) exactly `width` bits. DWrite gives the glyph
 * of any of these errors an empty bitmap (output: notes/truetype.md); here the caller gets -1. */
typedef struct {
    const idwr_font *font;
    const uint8_t *eblc, *ebdt, *strike;
    uint32_t len, ebdt_len;
    uint8_t *buf;     /* rows of `row_bytes`, most significant bit first */
    uint32_t row_bytes, size;
} sbit_canvas;

typedef struct {
    uint16_t height, width;
    const uint8_t *data;  /* the bits (formats 1, 2, 5, 6, 7) or numComponents (8, 9) */
    uint32_t avail;       /* bytes from `data` to the end of the image data */
} sbit_image;

/* the image's size and data, from EBDT or (formats 2 and 5 of the index) EBLC */
static int sbit_image_of(const sbit_canvas *c, const sbit_entry *e, sbit_image *im) {
    if ((uint64_t)e->offset + e->size > c->ebdt_len) return 0;
    const uint8_t *g = c->ebdt + e->offset;
    uint32_t skip;
    if (e->format == 1 || e->format == 2) skip = 5;
    else if (e->format == 6 || e->format == 7 || e->format == 9) skip = 8;
    else if (e->format == 8) skip = 6;   /* small metrics and a pad byte */
    else if (e->format == 5 && e->metrics) skip = 0;
    else return 0;
    const uint8_t *m = e->format == 5 ? e->metrics : g;
    if (e->format != 5 && e->size < skip) return 0;
    im->height = m[0], im->width = m[1];
    im->data = g + skip, im->avail = e->size - skip;
    return 1;
}

/* sfac_ShaveSbitMetrics for image format 5 up to 128 pixels wide: the blank rows above and the blank
 * columns left of the ink (*top, *left) and the inked box's size; an all-blank glyph keeps its cell */
static void sbit_shave(const sbit_image *im, uint16_t *left, uint16_t *top, uint16_t *w, uint16_t *h) {
    *left = *top = 0, *w = im->width, *h = im->height;
    if (im->width > 0x80 || !im->width || !im->height || !im->avail) return;
    uint32_t x0 = im->width, x1 = 0, y0 = im->height, y1 = 0;
    for (uint32_t y = 0; y < im->height; y++)
        for (uint32_t x = 0; x < im->width; x++) {
            uint32_t pos = y * im->width + x;
            if (pos / 8 < im->avail && (im->data[pos / 8] >> (7 - pos % 8) & 1)) {
                if (x < x0) x0 = x;
                if (x + 1 > x1) x1 = x + 1;
                if (y < y0) y0 = y;
                if (y + 1 > y1) y1 = y + 1;
            }
        }
    if (x0 >= x1) return;
    *left = (uint16_t)x0, *top = (uint16_t)y0, *w = (uint16_t)(x1 - x0), *h = (uint16_t)(y1 - y0);
}

static int sbit_draw(sbit_canvas *c, uint16_t glyph, const sbit_entry *e, uint32_t x, uint32_t y, uint32_t depth) {
    sbit_image im;
    if (!sbit_image_of(c, e, &im)) return 0x1a00;
    uint16_t sl = 0, st = 0, w = im.width, h = im.height;
    if (e->format == 5) sbit_shave(&im, &sl, &st, &w, &h);
    uint32_t X = x + sl, Y = y + st;
    if (X > 0xffff || Y > 0xffff) return 0x1a00;   /* SafeInt: a FileFormatException as well */
    if ((uint64_t)(Y + h) * c->row_bytes > c->size || c->row_bytes < (X + w + 7) / 8) return 0x1a00;
    if (e->format == 8 || e->format == 9) {
        uint32_t n = im.avail >= 2 ? idwr_u16(im.data) : 0;
        if (n && depth + 1 > 20) return 0x140b;
        for (uint32_t k = 0; k < n; k++) {
            if (2 + 4 * (uint64_t)(k + 1) > im.avail) return 0x1a00;
            const uint8_t *r = im.data + 2 + 4 * k;
            uint16_t cg = idwr_u16(r);
            sbit_entry ce;
            if (!strike_entry(c->eblc, c->len, c->strike, cg, &ce)) return 0x1801;
            int err = sbit_draw(c, cg, &ce, X + r[2], Y + r[3], depth + 1);
            if (err) return err;
        }
        (void)glyph;
        return 0;
    }
    uint8_t *row = c->buf + (size_t)Y * c->row_bytes;
    if (e->format == 1 || e->format == 6) {   /* whole bytes, shifted into place */
        uint32_t srb = (im.width + 7u) / 8u, s = X & 7, avail = im.avail;
        const uint8_t *p = im.data;
        for (uint32_t r = 0; r < im.height; r++, row += c->row_bytes) {
            uint32_t n = srb < avail ? srb : avail;
            uint8_t *d = row + (X >> 3), carry = 0;
            for (uint32_t i = 0; i < n; i++) {
                if (d + i >= c->buf + c->size) return 0x1a00;
                d[i] |= (uint8_t)((carry << (8 - s)) | (p[i] >> s));
                carry = s ? p[i] & (uint8_t)((1u << s) - 1) : 0;
            }
            if (s) {
                if (d + n >= c->buf + c->size) return 0x1a00;
                d[n] |= (uint8_t)(carry << (8 - s));
            }
            p += n, avail -= n;
        }
        return 0;
    }
    /* bit-aligned: the rows of the (unshaved) image, the shaved box at (X, Y) */
    for (uint32_t r = 0; r < im.height; r++)
        for (uint32_t q = 0; q < im.width; q++) {
            uint32_t pos = r * im.width + q;
            if (pos / 8 < im.avail && (im.data[pos / 8] >> (7 - pos % 8) & 1)) {
                uint32_t px = x + q, py = y + r;
                c->buf[(size_t)py * c->row_bytes + px / 8] |= (uint8_t)(0x80 >> (px & 7));
            }
        }
    return 0;
}

/* a composite's canvas: the metrics box grown by (grow_x, grow_y), the bold shifts sbit_GetMetrics adds
 * to the strike glyph's width and height (sbit_State + 0x50, + 0x52 = client + 0x410, + 0x412). Its row
 * bytes follow the grown width, so component bits past the glyph's own width stay in the buffer and show
 * where the grown columns are drawn. -2 when the glyph is not a composite. */
int idwr_strike_composite(const idwr_font *font, int mode, float em, uint16_t glyph, uint32_t grow_x, uint32_t grow_y,
                         idwr_strike_glyph *out, uint8_t *pixels, size_t capacity) {
    const uint8_t *eblc;
    uint32_t len, ebdt_len;
    const uint8_t *strike = active_strike(font, mode, em, &eblc, &len);
    sbit_entry e;
    if (!strike || mode == IDWR_MODE_NATURAL_SYMMETRIC || !strike_entry(eblc, len, strike, glyph, &e)) return 0;
    if (e.format != 8 && e.format != 9) return -2;
    const uint8_t *ebdt = idwr_font_table(font, IDWR_TAG('E', 'B', 'D', 'T'), &ebdt_len);
    if (!ebdt || (uint64_t)e.offset + e.size > ebdt_len || e.size < (e.format == 8 ? 5u : 8u)) return -1;
    const uint8_t *g = ebdt + e.offset;
    uint32_t width = g[1] + (g[1] ? grow_x : 0), height = g[0] + (g[0] ? grow_y : 0);
    out->bearing_x = (int8_t)g[2], out->bearing_y = (int8_t)g[3], out->width = width, out->height = height;
    sbit_canvas c = {font, eblc, ebdt, strike, len, ebdt_len, NULL, ((width + 31u) >> 3) & ~3u, 0};
    c.size = c.row_bytes * height;
    c.buf = calloc(c.size ? c.size : 1, 1);
    if (!c.buf) return -1;
#ifndef IDWR_NEGATIVE_CONTROL_COMPOSITE
    int err = sbit_draw(&c, glyph, &e, 0, 0, 1);
#else
    int err = -1;   /* the earlier model: composite formats not decoded */
#endif
    if (err || (pixels && capacity < (size_t)width * height)) {
        free(c.buf);
        return -1;
    }
    if (pixels)
        for (uint32_t y = 0; y < height; y++)
            for (uint32_t x = 0; x < width; x++)
                pixels[y * width + x] = c.buf[y * c.row_bytes + x / 8] >> (7 - x % 8) & 1 ? 255 : 0;
    free(c.buf);
    return 1;
}

int idwr_strike_bitmap(const idwr_font *font, int mode, float em, uint16_t glyph, idwr_strike_glyph *out,
                      uint8_t *pixels, size_t capacity) {
    int rc = idwr_strike_composite(font, mode, em, glyph, 0, 0, out, pixels, capacity);
    if (rc != -2) return rc;
    const uint8_t *eblc;
    uint32_t len, ebdt_len;
    const uint8_t *strike = active_strike(font, mode, em, &eblc, &len);
    sbit_entry e;
    if (!strike || mode == IDWR_MODE_NATURAL_SYMMETRIC || !strike_entry(eblc, len, strike, glyph, &e)) return 0;
    const uint8_t *ebdt = idwr_font_table(font, IDWR_TAG('E', 'B', 'D', 'T'), &ebdt_len);
    if (!ebdt || (uint64_t)e.offset + e.size > ebdt_len) return -1;
    const uint8_t *g = ebdt + e.offset, *bits;
    uint8_t height, width;
    int8_t bx, by;
    int aligned;
    uint32_t avail;
    if (e.format == 1 || e.format == 2) {
        if (e.size < 5) return -1;
        height = g[0], width = g[1], bx = (int8_t)g[2], by = (int8_t)g[3];
        bits = g + 5, avail = e.size - 5, aligned = e.format == 1;
    } else if (e.format == 6 || e.format == 7) {
        if (e.size < 8) return -1;
        height = g[0], width = g[1], bx = (int8_t)g[2], by = (int8_t)g[3];
        bits = g + 8, avail = e.size - 8, aligned = e.format == 6;
    } else if (e.format == 5 && e.metrics) {
        height = e.metrics[0], width = e.metrics[1], bx = (int8_t)e.metrics[2], by = (int8_t)e.metrics[3];
        bits = g, avail = e.size, aligned = 0;
    } else {
        return -1;
    }
    out->bearing_x = bx, out->bearing_y = by, out->width = width, out->height = height;
    if (!pixels) return 1;
    if (capacity < (size_t)width * height) return -1;
    uint32_t stride = (width + 7u) / 8u;
    for (uint32_t y = 0; y < height; y++)
        for (uint32_t x = 0; x < width; x++) {
            uint32_t pos = aligned ? y * stride * 8 + x : y * width + x;
            uint8_t on = pos / 8 < avail && (bits[pos / 8] >> (7 - pos % 8) & 1);
            pixels[y * width + x] = on ? 255 : 0;
        }
    return 1;
}

/* the active strike's entry for `glyph`: its image format and whether its metrics come from
 * the index subtable (formats 2 and 5 of the index); 1, or 0 when the glyph has none */
int idwr_strike_entry_kind(const idwr_font *font, int mode, float em, uint16_t glyph, int *image_format,
                          int *index_metrics) {
    const uint8_t *eblc;
    uint32_t len;
    const uint8_t *strike = active_strike(font, mode, em, &eblc, &len);
    sbit_entry e;
    if (!strike || !strike_entry(eblc, len, strike, glyph, &e)) return 0;
    *image_format = e.format;
    *index_metrics = e.metrics != NULL;
    return 1;
}
