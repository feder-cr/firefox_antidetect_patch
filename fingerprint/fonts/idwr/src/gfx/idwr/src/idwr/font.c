/*
 * sfnt access for idwr: the table directory of one face, table lookup, the face type, and the
 * cmap lookup used by the metrics model.
 */
#include <stdlib.h>
#include <string.h>
#include "idwr.h"
#include "idwr_internal.h"

static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

idwr_font *idwr_font_open(const uint8_t *data, size_t size, uint32_t index) {
    if (!data || size < 12) return NULL;
    size_t dir = 0;
    int collection = be32(data) == IDWR_TAG('t', 't', 'c', 'f');
    if (collection) {
        if (size < 16 || index >= be32(data + 8) || 12 + 4 * (size_t)index + 4 > size) return NULL;
        dir = be32(data + 12 + 4 * index);
    } else if (index != 0) {
        return NULL;
    }
    if (dir + 12 > size) return NULL;
    uint32_t version = be32(data + dir);
    if (version != 0x00010000 && version != IDWR_TAG('O', 'T', 'T', 'O') && version != IDWR_TAG('t', 'r', 'u', 'e'))
        return NULL;
    uint16_t n = idwr_u16(data + dir + 4);
    if (dir + 12 + 16 * (size_t)n > size) return NULL;
    idwr_font *f = calloc(1, sizeof *f);
    if (!f) return NULL;
    f->data = data;
    f->size = size;
    f->ntables = n;
    f->dir = data + dir + 12;
    f->collection = collection;
    f->head = idwr_font_table(f, IDWR_TAG('h', 'e', 'a', 'd'), &f->head_len);
    return f;
}

#if defined(IDWR_STANDALONE) && defined(IDWR_CFF)
#include "cffraster.h"
#endif

void idwr_font_close(idwr_font *font) {
    if (!font) return;
#if defined(IDWR_STANDALONE) && defined(IDWR_CFF)
    idwr_cff_font_release(font);
#endif
    free(font);
}

int idwr_font_is_cff(const idwr_font *font) {
    return idwr_font_has_table(font, IDWR_TAG('C', 'F', 'F', ' ')) || idwr_font_has_table(font, IDWR_TAG('C', 'F', 'F', '2'));
}

const uint8_t *idwr_font_table(const idwr_font *font, uint32_t tag, uint32_t *size) {
    for (uint16_t i = 0; i < font->ntables; i++) {
        const uint8_t *r = font->dir + 16 * i;
        if (be32(r) != tag) continue;
        uint32_t off = be32(r + 8), len = be32(r + 12);
        if ((size_t)off + len > font->size) break;
        if (size) *size = len;
        return font->data + off;
    }
    if (size) *size = 0;
    return NULL;
}

int idwr_font_has_table(const idwr_font *font, uint32_t tag) {
    uint32_t n;
    return idwr_font_table(font, tag, &n) != NULL && n > 0;
}

int idwr_font_face_type(const idwr_font *font) {
    if (font->collection) return IDWR_FACE_OPENTYPE_COLLECTION;
    /* CFF2 faces are DWRITE_FONT_FACE_TYPE_CFF too (GetType on the CFF2 fonts of the CFF corpus) */
    return idwr_font_is_cff(font) ? IDWR_FACE_CFF : IDWR_FACE_TRUETYPE;
}

uint16_t idwr_font_glyph_count(const idwr_font *font) {
    uint32_t n;
    const uint8_t *maxp = idwr_font_table(font, IDWR_TAG('m', 'a', 'x', 'p'), &n);
    return maxp && n >= 6 ? idwr_u16(maxp + 4) : 0;
}

/* cmap subtable lookups: format 4 and 12 */
static uint16_t cmap4(const uint8_t *t, uint32_t len, uint32_t cp) {
    if (cp > 0xffff || len < 14) return 0;
    uint16_t segs = idwr_u16(t + 6) / 2;
    if (14 + 8 * (uint32_t)segs + 2 > len) return 0;
    const uint8_t *ends = t + 14, *starts = ends + 2 * segs + 2, *deltas = starts + 2 * segs, *ranges = deltas + 2 * segs;
    for (uint16_t i = 0; i < segs; i++) {
        if (cp > idwr_u16(ends + 2 * i)) continue;
        uint16_t start = idwr_u16(starts + 2 * i);
        if (cp < start) return 0;
        uint16_t delta = idwr_u16(deltas + 2 * i), range = idwr_u16(ranges + 2 * i);
        if (range == 0) return (uint16_t)(cp + delta);
        uint32_t at = (uint32_t)(ranges + 2 * i - t) + range + 2 * (cp - start);
        if (at + 2 > len) return 0;
        uint16_t g = idwr_u16(t + at);
        return g ? (uint16_t)(g + delta) : 0;
    }
    return 0;
}

static uint16_t cmap12(const uint8_t *t, uint32_t len, uint32_t cp) {
    if (len < 16) return 0;
    uint32_t n = be32(t + 12);
    if (16 + 12 * (uint64_t)n > len) return 0;
    for (uint32_t i = 0; i < n; i++) {
        const uint8_t *g = t + 16 + 12 * i;
        uint32_t lo = be32(g), hi = be32(g + 4);
        if (cp >= lo && cp <= hi) return (uint16_t)(be32(g + 8) + (cp - lo));
    }
    return 0;
}

/* finds the (platform, encoding) subtable; returns its format or -1 */
static int cmap_subtable(const idwr_font *font, uint16_t platform, uint16_t encoding, const uint8_t **sub, uint32_t *sublen) {
    uint32_t len;
    const uint8_t *cmap = idwr_font_table(font, IDWR_TAG('c', 'm', 'a', 'p'), &len);
    if (!cmap || len < 4) return -1;
    uint16_t n = idwr_u16(cmap + 2);
    for (uint16_t i = 0; i < n && 4 + 8 * (uint32_t)i + 8 <= len; i++) {
        const uint8_t *r = cmap + 4 + 8 * i;
        if (idwr_u16(r) != platform || idwr_u16(r + 2) != encoding) continue;
        uint32_t off = be32(r + 4);
        if (off + 4 > len) return -1;
        *sub = cmap + off;
        *sublen = len - off;
        return idwr_u16(cmap + off);
    }
    return -1;
}

/* OpenTypeFontFaceBuilder::ReadCharacterMap: the face is symbol when the cmap subtable it
 * reads is the symbol one, or when the OS/2 PANOSE family is 5 (pictorial). The subtable
 * order below is the one GetGlyphIndices follows; no system font has both a symbol and a
 * Unicode subtable, so that case is unverified. */
int idwr_font_is_symbol(const idwr_font *font) {
    const uint8_t *sub;
    uint32_t len;
    int unicode = cmap_subtable(font, 3, 10, &sub, &len) == 12 || cmap_subtable(font, 3, 1, &sub, &len) == 4;
    if (!unicode && cmap_subtable(font, 3, 0, &sub, &len) >= 0) return 1;
    const uint8_t *os2 = idwr_font_table(font, IDWR_TAG('O', 'S', '/', '2'), &len);
    return os2 && len > 32 && os2[32] == 5;
}

uint16_t idwr_font_glyph_index(const idwr_font *font, uint32_t cp) {
    const uint8_t *sub;
    uint32_t len;
    int fmt;
    if ((fmt = cmap_subtable(font, 3, 10, &sub, &len)) == 12) return cmap12(sub, len, cp);
    if ((fmt = cmap_subtable(font, 3, 1, &sub, &len)) == 4) return cmap4(sub, len, cp);
    if ((fmt = cmap_subtable(font, 3, 0, &sub, &len)) == 4) {
        uint16_t g = cmap4(sub, len, cp);
        if (!g && cp >= 0x20 && cp <= 0xff) g = cmap4(sub, len, 0xf000 + cp);
        return g;
    }
    return 0;
}
