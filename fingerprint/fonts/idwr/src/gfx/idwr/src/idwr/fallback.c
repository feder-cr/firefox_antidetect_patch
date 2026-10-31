/*
 * DirectWrite's system font fallback (DWrite.dll 10.0.26100.9711), ported from the disassembly:
 * notes/fallback.md. The data (FONTFALLBACK, the Unicode property trie slice) is copied byte for byte
 * by reimpl/gen_fallback_table.py into fallback_tables.h; the NLS parent locales into fallback_nls.h.
 * Checked against the real DLL by reimpl/fallback_check.py.
 *
 * Each function names the DWrite function it reproduces. Offsets such as "+0x74" are the
 * FallbackState fields of FontFallback::MapCharacters 0x180034344.
 */
#include <stdlib.h>
#include <string.h>
#include "idwr.h"
#include "idwr_internal.h"
#include "fallback_tables.h"
#include "fallback_nls.h"

/* -DIDWR_NEGATIVE_CONTROL_FALLBACK=n breaks one rule on purpose (reimpl/fallback_check.py
 * --negative-control): 1 targets in reverse order, 2 no extend property (0xCB), 3 GetMatchingFonts
 * without its sort, 4 no symbol cross-population, 5 no default-ignorable property (0x14) in
 * MatchFontToText, 6 no NLS parent locales, 7 no NO_VISUAL rule in the text layout path */
#ifndef IDWR_NEGATIVE_CONTROL_FALLBACK
#define IDWR_NEGATIVE_CONTROL_FALLBACK 0
#endif
#define NC(n) (IDWR_NEGATIVE_CONTROL_FALLBACK == (n))

#define HR_E_INVALIDARG ((int32_t)0x80070057)
#define HR_E_OUTOFMEMORY ((int32_t)0x8007000e)

static uint32_t le16(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8; }
static uint32_t le32(const uint8_t *p) { return le16(p) | le16(p + 2) << 16; }
static uint32_t be16(const uint8_t *p) { return (uint32_t)p[0] << 8 | p[1]; }
static uint32_t be32(const uint8_t *p) { return be16(p) << 16 | be16(p + 2); }

/* ---------------------------------------------------------------- Unicode properties (UcdInternal) */

static uint32_t ucd_index(uint32_t i) { return le16(idwr_ucd_index + 2 * i); }

static uint32_t ucd_leaf(uint32_t root, uint32_t c) {
    uint32_t a = ucd_index(root + (c >> 18 & 0x3f));
    uint32_t b = ucd_index(a * 0x40 + (c >> 12 & 0x3f));
    return ucd_index(b * 0x40 + (c >> 6 & 0x3f));
}

/* UcdInternal::GetProperty1 0x180035e80 */
static int ucd_bit(uint32_t prop, uint32_t c) {
    if (NC(2) && prop == IDWR_UCD_EXTEND_PROP) return 0;
    uint32_t leaf = ucd_leaf(prop * 0x40, c);
    return idwr_ucd_bits[leaf * 8 + ((c & 0x3f) >> 3)] >> (c & 7) & 1;
}

/* TextIterator::GetTextProp<Script / BidiClass / GeneralCategory> */
static uint32_t ucd_byte(uint32_t root, uint32_t c) { return idwr_ucd_bytes[ucd_leaf(root, c) * 0x40 + (c & 0x3f)]; }

/* the joiners after which FindTextRangeEnd and MatchFontToText keep a character with its base */
static int is_joiner(uint32_t c) {
    return c == 0x34f || c == 0x200d || c == 0x2d7f || c == 0x1107f || c == 0x11a47 || c == 0x11a99 ||
           c == 0x13430 || c == 0x13431;
}

/* The itemizer (Itemization::Itemization 0x1800e1580, Itemizer::Analyze 0x18002eaa4) for a text of
 * one character: the rule that sets DWRITE_SCRIPT_SHAPES_NO_VISUAL (Itemization::SetHidden) matches a
 * character of GeneralCategory 0 or 1 (DWrite's own classes, not Unicode's) when the earlier
 * alternatives do not: a script-specific character (Script not 1 or 2) starts a script run, and
 * BidiClass 0 or 1 an Arabic run (Itemization::SetArabic); Script 2 (inherited) with nothing before
 * it is a run of script 0. Checked on every code point against AnalyzeScript. */
IDWR_API int idwr_fallback_no_visual(uint32_t c) {
    if (c > 0x10ffff) return 0;
    uint32_t cls = ucd_byte(IDWR_UCD_CLASS_ROOT, c);
    return cls <= 1 && ucd_byte(IDWR_UCD_SCRIPT_ROOT, c) == 1 && ucd_byte(IDWR_UCD_BIDI_ROOT, c) > 1;
}

/* ---------------------------------------------------------------- text iteration (TextIterator) */

/* TextIterator::operator* 0x180044df0: a surrogate pair, else the code unit */
static uint32_t cp_at(const uint16_t *t, uint32_t len, uint32_t pos) {
    uint32_t u = t[pos];
    if (len - pos > 1 && (u & 0xfc00) == 0xd800 && (t[pos + 1] & 0xfc00) == 0xdc00)
        return ((u & 0x3ff) << 10 | (t[pos + 1] & 0x3ffu)) + 0x10000;
    return u;
}

/* TextIterator::AdvanceOne 0x18002d4a0: the units of one code point */
static uint32_t advance_one(const uint16_t *t, uint32_t len, uint32_t pos) {
    if (pos >= len) return 0;
    if (len - pos == 1) return 1;
    return (t[pos] & 0xfc00) == 0xd800 && (t[pos + 1] & 0xfc00) == 0xdc00 ? 2 : 1;
}

/* TextIterator::Advance 0x180046af0: one code point, and a following character with property 0x14 */
static uint32_t advance(const uint16_t *t, uint32_t len, uint32_t pos) {
    uint32_t n = advance_one(t, len, pos);
    if (n && pos + n != len && ucd_bit(IDWR_UCD_IGNORABLE_PROP, cp_at(t, len, pos + n)))
        n += advance_one(t, len, pos + n);
    return n;
}

/* ---------------------------------------------------------------- coverage (CompactCmap) */

typedef struct {
    uint16_t page;   /* code point >> 6 */
    uint64_t bits;
} cov_page;

typedef struct {
    cov_page *pages;
    uint32_t n, cap;
    int bad;         /* FileFormatException */
    int legacy;      /* a code-page cmap idwr does not reproduce */
} cov_builder;

static int cov_find(const cov_builder *b, uint32_t page) {
    for (uint32_t i = 0; i < b->n; i++)
        if (b->pages[i].page == page) return (int)i;
    return -1;
}

static cov_page *cov_get(cov_builder *b, uint32_t page) {
    uint32_t lo = 0, hi = b->n;
    while (lo < hi) {
        uint32_t mid = (lo + hi) / 2;
        if (b->pages[mid].page < page) lo = mid + 1; else hi = mid;
    }
    if (lo < b->n && b->pages[lo].page == page) return &b->pages[lo];
    if (b->n == b->cap) {
        uint32_t cap = b->cap ? 2 * b->cap : 64;
        cov_page *p = realloc(b->pages, cap * sizeof *p);
        if (!p) { b->bad = 1; return NULL; }
        b->pages = p;
        b->cap = cap;
    }
    memmove(b->pages + lo + 1, b->pages + lo, (b->n - lo) * sizeof *b->pages);
    b->pages[lo].page = (uint16_t)page;
    b->pages[lo].bits = 0;
    b->n++;
    return &b->pages[lo];
}

/* CmapRegionBuilder<CompactCmapRegion>::Insert 0x18007f2b0 */
static void cov_insert(cov_builder *b, uint32_t u, uint32_t glyph) {
    if ((glyph & 0xffff) == 0 || u >= 0x110000) return;
    cov_page *p = cov_get(b, u >> 6);
    if (p) p->bits |= (uint64_t)1 << (u & 0x3f);
}

/* MacRomanCharToUnicode 0x1800d5ac0, UnicodeCharToUnicode 0x1800c5930 */
static uint32_t to_unicode(int macroman, uint32_t c) {
    if (!macroman || c < 0x80) return c;
    return c < 0x100 ? le16(idwr_macroman + 2 * (c - 0x80)) : 0;
}

uint32_t idwr_macroman_to_unicode(uint32_t c) { return to_unicode(1, c); }

/* OpenTypeCharacterMap::ReadFormat12or13 0x180095c00 (and Read's inline format 12) */
static void read_format12(cov_builder *b, const uint8_t *s, uint32_t size, uint32_t nglyphs, int format13) {
    if (size < 16) { b->bad = 1; return; }
    uint32_t n = be32(s + 12);
    if ((size - 16) / 12 < n) { b->bad = 1; return; }
    for (uint32_t i = 0; i < n; i++) {
        const uint8_t *g = s + 16 + 12 * i;
        uint32_t c = be32(g), end = be32(g + 4), glyph = be32(g + 8);
        if (end == 0xffffffff) end = 0xfffffffe;
        if (end > 0x10ffff) end = 0x10ffff;  /* Insert drops the rest; same result, bounded loop */
        for (; c <= end && glyph < nglyphs; c++) {
            cov_insert(b, c, glyph & 0xffff);
            if (!format13) glyph++;
        }
    }
}

/* OpenTypeCharacterMap::ReadFormat0 0x1800f0068 */
static void read_format0(cov_builder *b, const uint8_t *s, uint32_t size, uint32_t nglyphs, int mac) {
    if (size < 0x106) { b->bad = 1; return; }
    for (uint32_t c = 0; c < 0x100; c++) {
        uint32_t g = s[6 + c];
        if (g != 0 && g < nglyphs) cov_insert(b, to_unicode(mac, c), g);
    }
}

/* OpenTypeCharacterMap::ReadFormat6 0x1800f22f8 */
static void read_format6(cov_builder *b, const uint8_t *s, uint32_t size, uint32_t nglyphs, int mac) {
    if (size < 10) { b->bad = 1; return; }
    uint32_t c = be16(s + 6), n = be16(s + 8);
    if ((size - 10) / 2 < n) { b->bad = 1; return; }
    for (uint32_t i = 0; i < n; i++, c++) {
        uint32_t g = be16(s + 10 + 2 * i);
        if (g != 0 && g < nglyphs) {
            uint32_t u = to_unicode(mac, c);
            if (u) cov_insert(b, u, g);
        }
    }
}

/* OpenTypeCharacterMap::ReadFormat4 0x18005d1e0 */
static void read_format4(cov_builder *b, const uint8_t *s, uint32_t size, uint32_t nglyphs, int mac) {
    if (size < 14) { b->bad = 1; return; }
    uint32_t seg = (uint32_t)s[6] << 7 | s[7] >> 1;
    if ((size - 14) / 2 < seg) { b->bad = 1; return; }
    uint32_t starts = seg * 2 + 16, deltas = seg * 4 + 16, ros = seg * 6 + 16;
    if (size < starts || (size - starts) / 2 < seg || size < deltas || (size - deltas) / 2 < seg || size < ros ||
        (size - ros) / 2 < seg) {
        b->bad = 1;
        return;
    }
    uint32_t next = 0;
    for (uint32_t i = 0; i < seg; i++) {
        uint32_t start = be16(s + starts + 2 * i), end = be16(s + 14 + 2 * i);
        if (start == 0xffff && end == 0xffff) return;
        uint32_t first = start < next ? next : start;
        next = end + 1;
        if (end < first) continue;
        uint32_t count = end - start + 1;
        uint32_t delta = be16(s + deltas + 2 * i), ro = be16(s + ros + 2 * i);
        const uint8_t *arr = NULL;
        if (ro) {
            uint32_t pos = ro + i * 2 + ros;
            if (size < pos || (size - pos) / 2 < count)
                count = pos < size ? (size - pos) / 2 : 0;
            if (count) arr = s + pos;
        }
        uint32_t k = first - start;
        for (uint32_t c = first; k < count; c++, k++) {
            uint32_t u = to_unicode(mac, c);
            if (u >= 0xffff) continue;
            if (!ro) {
                uint32_t g = (c + delta) & 0xffff;
                if (g != 0 && g < nglyphs) cov_insert(b, u, g);
            } else if (arr) {
                uint32_t raw = be16(arr + 2 * k), g = (delta + raw) & 0xffff;
                if (raw != 0 && g != 0 && g < nglyphs) cov_insert(b, u, g);
            }
        }
    }
}

/* PlatformIdAndEncodingIdToCharacterEncoding 0x18005da84 */
static int cmap_encoding(uint32_t pid, uint32_t eid) {
    if (pid == 0) return eid == 4 || eid == 6 ? 9 : 2;
    if (pid == 1) return eid == 0;
    if (pid == 3) {
        switch (eid) {
        case 0: return 3;
        case 1: return 8;
        case 2: return 4;
        case 3: return 5;
        case 4: return 6;
        case 5: return 7;
        case 10: return 9;
        }
    }
    return 0;
}

/* OpenTypeFontFaceBuilder::ReadCharacterMap 0x18005d690 → OpenTypeCharacterMapSubtable 0x18005d894,
 * CompactCmapRegion::Create 0x18005db24, OpenTypeCharacterMap::Read 0x18005cd60,
 * CrossPopulateSymbols 0x180088ee8 */
static void build_coverage(cov_builder *b, const idwr_font *font) {
    uint32_t cmap_len = 0, maxp_len = 0, os2_len = 0;
    const uint8_t *cmap = idwr_font_table(font, IDWR_TAG('c', 'm', 'a', 'p'), &cmap_len);
    const uint8_t *maxp = idwr_font_table(font, IDWR_TAG('m', 'a', 'x', 'p'), &maxp_len);
    const uint8_t *os2 = idwr_font_table(font, IDWR_TAG('O', 'S', '/', '2'), &os2_len);
    uint32_t nglyphs = maxp && maxp_len >= 6 ? be16(maxp + 4) : 0;
    if (!cmap) return;
    if (cmap_len < 4) { b->bad = 1; return; }
    uint32_t n = be16(cmap + 2);
    if ((cmap_len - 4) / 8 < n) { b->bad = 1; return; }
    int enc = 0;
    const uint8_t *sub = NULL;
    uint32_t size = 0;
    for (uint32_t i = 0; i < n; i++) {
        const uint8_t *r = cmap + 4 + 8 * i;
        int e = cmap_encoding(be16(r), be16(r + 2));
        uint32_t off = be32(r + 4);
        if (off > cmap_len) { b->bad = 1; return; }
        if (e > enc) {
            enc = e;
            sub = cmap + off;
            size = cmap_len - off;
        }
    }
    /* an OS/2 fsSelection high byte of 0xB1..0xB3 (a Windows Hebrew/Arabic charset) selects encodings
     * 10..12, whose MirrorCodepoints tables idwr does not carry */
    if (os2 && os2_len > 0x3e && os2[0x3e] >= 0xb1 && os2[0x3e] <= 0xb3) { b->legacy = 1; return; }
    if (!enc) return;
    if (enc >= 4 && enc <= 7) { b->legacy = 1; return; }  /* Shift-JIS, PRC, Big5, Wansung */
    if (size < 2) { b->bad = 1; return; }
    uint32_t format = be16(sub);
    int mac = enc == 1;
    if (mac) {
        if (format == 0) read_format0(b, sub, size, nglyphs, 1);
        else if (format == 4) read_format4(b, sub, size, nglyphs, 1);
        else if (format == 6) read_format6(b, sub, size, nglyphs, 1);
    } else {
        if (format == 12) read_format12(b, sub, size, nglyphs, 0);
        else if (format == 0) read_format0(b, sub, size, nglyphs, 0);
        else if (format == 4) read_format4(b, sub, size, nglyphs, 0);
        else if (format == 6) read_format6(b, sub, size, nglyphs, 0);
        else if (format == 13) read_format12(b, sub, size, nglyphs, 1);
    }
    if (enc == 3 && !NC(4)) {  /* symbol: U+0000..00FF and U+F000..F0FF take each other's pages */
        for (uint32_t p = 0; p < 4; p++) {
            int lo = cov_find(b, p), hi = cov_find(b, 0x3c0 + p);
            if (lo < 0 && hi < 0) continue;
            uint64_t bits = (lo >= 0 ? b->pages[lo].bits : 0) | (hi >= 0 ? b->pages[hi].bits : 0);
            cov_page *q = cov_get(b, p);
            if (q) q->bits = bits;
            q = cov_get(b, 0x3c0 + p);
            if (q) q->bits = bits;
        }
    }
}

/* ---------------------------------------------------------------- the font set */

typedef struct {
    uint16_t weight;
    uint8_t stretch, style;
    int instance_filter;
    int32_t family;
    cov_page *pages;
    uint32_t npages;
} fs_font;

typedef struct {
    uint16_t **names;   /* UTF-16, NUL-terminated */
    uint32_t nnames;
    int32_t *fonts;
    uint32_t nfonts;
} fs_family;

struct idwr_font_set {
    fs_family *families;
    uint32_t nfamilies;
    fs_font *fonts;
    uint32_t nfonts;
};

IDWR_API idwr_font_set *idwr_font_set_create(void) { return calloc(1, sizeof(idwr_font_set)); }

IDWR_API void idwr_font_set_destroy(idwr_font_set *set) {
    if (!set) return;
    for (uint32_t f = 0; f < set->nfamilies; f++) {
        for (uint32_t i = 0; i < set->families[f].nnames; i++) free(set->families[f].names[i]);
        free(set->families[f].names);
        free(set->families[f].fonts);
    }
    for (uint32_t i = 0; i < set->nfonts; i++) free(set->fonts[i].pages);
    free(set->families);
    free(set->fonts);
    free(set);
}

/* UTF-8 → UTF-16 (malformed bytes become U+FFFD); *n receives the length without the NUL */
static uint16_t *utf16(const char *s, uint32_t *n) {
    size_t len = strlen(s);
    uint16_t *o = malloc((len + 1) * sizeof *o);
    uint32_t k = 0;
    if (!o) return NULL;
    for (size_t i = 0; i < len;) {
        uint32_t c = (uint8_t)s[i], need = 0;
        if (c < 0x80) need = 0;
        else if ((c & 0xe0) == 0xc0) { c &= 0x1f; need = 1; }
        else if ((c & 0xf0) == 0xe0) { c &= 0x0f; need = 2; }
        else if ((c & 0xf8) == 0xf0) { c &= 0x07; need = 3; }
        else { o[k++] = 0xfffd; i++; continue; }
        size_t j = 1;
        for (; j <= need && i + j < len && ((uint8_t)s[i + j] & 0xc0) == 0x80; j++) c = c << 6 | ((uint8_t)s[i + j] & 0x3f);
        if (j <= need) { o[k++] = 0xfffd; i += j; continue; }
        i += j;
        if (c >= 0x10000) {
            o[k++] = (uint16_t)(0xd800 + ((c - 0x10000) >> 10));
            o[k++] = (uint16_t)(0xdc00 + (c & 0x3ff));
        } else
            o[k++] = (uint16_t)c;
    }
    o[k] = 0;
    if (n) *n = k;
    return o;
}

IDWR_API int32_t idwr_font_set_add_family(idwr_font_set *set, const char *const *names, uint32_t name_count) {
    if (!set) return -1;
    fs_family *f = realloc(set->families, (set->nfamilies + 1) * sizeof *f);
    if (!f) return -1;
    set->families = f;
    f = &set->families[set->nfamilies];
    memset(f, 0, sizeof *f);
    f->names = calloc(name_count ? name_count : 1, sizeof *f->names);
    if (!f->names) return -1;
    for (uint32_t i = 0; i < name_count; i++)
        if ((f->names[f->nnames] = utf16(names[i], NULL))) f->nnames++;
    return (int32_t)set->nfamilies++;
}

IDWR_API int32_t idwr_font_set_add_font(idwr_font_set *set, int32_t family, const idwr_font *font, uint16_t weight,
                                        uint8_t stretch, uint8_t style, int instance_filter) {
    if (!set || !font || family < 0 || (uint32_t)family >= set->nfamilies) return -1;
    cov_builder b = {0};
    build_coverage(&b, font);
    if (b.legacy || b.bad) {
        free(b.pages);
        return b.legacy ? -2 : -3;
    }
    fs_font *fonts = realloc(set->fonts, (set->nfonts + 1) * sizeof *fonts);
    fs_family *fam = &set->families[family];
    int32_t *list = realloc(fam->fonts, (fam->nfonts + 1) * sizeof *list);
    if (fonts) set->fonts = fonts;
    if (list) fam->fonts = list;
    if (!fonts || !list) { free(b.pages); return -1; }
    fs_font *f = &set->fonts[set->nfonts];
    f->weight = weight;
    f->stretch = stretch;
    f->style = style;
    f->instance_filter = instance_filter != 0;
    f->family = family;
    f->pages = b.pages;
    f->npages = b.n;
    fam->fonts[fam->nfonts++] = (int32_t)set->nfonts;
    return (int32_t)set->nfonts++;
}

/* CompactCmap::operator[] (DWriteFont::HasCharacter 0x1800ac4f0) */
static int font_has(const fs_font *f, uint32_t c) {
    if (c >= 0x110000) return 0;
    uint32_t page = c >> 6, lo = 0, hi = f->npages;
    while (lo < hi) {
        uint32_t mid = (lo + hi) / 2;
        if (f->pages[mid].page < page) lo = mid + 1;
        else if (f->pages[mid].page > page) hi = mid;
        else return (int)(f->pages[mid].bits >> (c & 0x3f) & 1);
    }
    return 0;
}

IDWR_API int idwr_font_set_has_character(const idwr_font_set *set, int32_t font, uint32_t c) {
    if (!set || font < 0 || (uint32_t)font >= set->nfonts) return 0;
    return font_has(&set->fonts[font], c);
}

/* Family names compare as CompareStringW(LOCALE_INVARIANT, NORM_IGNORECASE) == CSTR_EQUAL
 * (StringCheckedPtr::LessThan, the comparator of DWriteFontCollection::FindFamilyName's map) and
 * CompareInvariantIgnoreCase (FindNextMapping). idwr folds case for ASCII, Latin-1, Greek,
 * Cyrillic and the fullwidth Latin letters and compares the rest by code unit; the families of
 * the persona's fonts and the table's targets need no more (fallback_check.py checks them). */
static uint32_t fold(uint32_t c) {
    if (c >= 'A' && c <= 'Z') return c + 32;
    if (c >= 0xc0 && c <= 0xde && c != 0xd7) return c + 32;
    if (c >= 0x391 && c <= 0x3a9 && c != 0x3a2) return c + 32;
    if (c >= 0x410 && c <= 0x42f) return c + 32;
    if (c >= 0x400 && c <= 0x40f) return c + 80;
    if (c >= 0xff21 && c <= 0xff3a) return c + 32;
    return c;
}

static int name_equal(const uint16_t *a, const uint16_t *b) {
    for (;; a++, b++) {
        if (fold(*a) != fold(*b)) return 0;
        if (!*a) return 1;
    }
}

/* DWriteFontCollection::FindFamilyName 0x1800264b0: the map FontCollection::InitializeInternal fills
 * family by family, every name of a family when its first font is met, keeping the first family a
 * name was given to */
static int32_t find_family(const idwr_font_set *set, const uint16_t *name) {
    for (uint32_t f = 0; f < set->nfamilies; f++)
        for (uint32_t i = 0; i < set->families[f].nnames; i++)
            if (name_equal(set->families[f].names[i], name)) return (int32_t)f;
    return -1;
}

IDWR_API int32_t idwr_font_set_find_family(const idwr_font_set *set, const char *name) {
    if (!set || !name) return -1;
    uint16_t *n = utf16(name, NULL);
    if (!n) return -1;
    int32_t f = find_family(set, n);
    free(n);
    return f;
}

/* ---------------------------------------------------------------- font matching (WWS) */

typedef struct {
    int32_t font;
    uint32_t sims;
    int32_t stretch_key, style_key, weight_key;  /* MatchingFont +0x10, +0x14, +0x18 */
    const fs_font *entry;
} matching_font;

typedef struct { int32_t stretch, style, weight; } matching_style;

/* MatchingFont::MatchingFont 0x1800cc3e4 */
static void matching_font_init(matching_font *m, const fs_font *e, int32_t index, uint32_t weight, uint32_t style) {
    uint32_t sims = 0;
    m->font = index;
    m->entry = e;
    if ((int32_t)weight > 0x226 && e->weight < 0x227) sims = (int32_t)e->weight < (int32_t)(weight - 100);
    if (style != 0 && e->style == 0) sims |= 2;
    m->sims = sims;
    uint32_t st = (sims & 2) ? 1 : e->style;
    uint32_t w = e->weight;
    if ((sims & 1) && w <= 700) w = 700;
    m->stretch_key = ((int32_t)e->stretch - 5) * 0x44c;
    m->style_key = (int32_t)st * 700;
    m->weight_key = ((int32_t)w - 400) * 5;
}

/* MatchingStyle::IsBetterMatch 0x1800c56a8 */
static int is_better(const matching_style *s, const matching_font *a, const matching_font *b) {
    uint32_t sa = a->sims, sb = b->sims;
    if (sa != sb && a->entry->stretch == b->entry->stretch) {
        uint32_t oa = sa & 2;
        if (oa != (sb & 2)) {
            uint32_t wa = a->entry->weight, wb = b->entry->weight;
            if ((sa & 1) && wa <= 700) wa = 700;
            if ((sb & 1) && wb <= 700) wb = 700;
            if (wa == wb) {
                if (oa == 0) {
                    if (a->entry->style == 1) return 1;
                } else if (b->entry->style == 1)
                    return 0;
            }
        }
        if ((sa & 1) != (sb & 1)) {
            uint32_t ca = oa ? 1 : a->entry->style, cb = (sb & 2) ? 1 : b->entry->style;
            if (ca == cb) {
                if ((sa & 1) == 0) {
                    if (a->entry->weight > 0x226) return 1;
                } else if (b->entry->weight > 0x226)
                    return 0;
            }
        }
    }
    int32_t da = (a->stretch_key - s->stretch) * (a->stretch_key - s->stretch) +
                 (a->style_key - s->style) * (a->style_key - s->style) +
                 (a->weight_key - s->weight) * (a->weight_key - s->weight);
    int32_t db = (b->stretch_key - s->stretch) * (b->stretch_key - s->stretch) +
                 (b->style_key - s->style) * (b->style_key - s->style) +
                 (b->weight_key - s->weight) * (b->weight_key - s->weight);
    if (da != db) return da < db;
    int32_t pa = a->stretch_key * s->stretch + a->style_key * s->style + a->weight_key * s->weight;
    int32_t pb = b->stretch_key * s->stretch + b->style_key * s->style + b->weight_key * s->weight;
    if (pa != pb) return pa > pb;
    if (a->stretch_key != b->stretch_key) return a->stretch_key > b->stretch_key;
    if (a->style_key != b->style_key) return a->style_key > b->style_key;
    return a->weight_key > b->weight_key;
}

/* std::sort as MSVC's STL implements it (std::_Sort_unchecked 0x1800ff518 and its helpers):
 * insertion sort up to 32 elements, else quicksort partitions by a median guess, heap sort when
 * the partitions go too deep. The order of equal elements follows from it. */
static void swap_mf(matching_font *a, matching_font *b) {
    matching_font t = *a;
    *a = *b;
    *b = t;
}

static void insertion_sort(matching_font *first, matching_font *last, const matching_style *s) {
    if (first == last) return;
    for (matching_font *next = first + 1; next != last; next++) {
        matching_font val = *next;
        if (is_better(s, &val, first)) {
            memmove(first + 1, first, (size_t)(next - first) * sizeof *first);
            *first = val;
        } else {
            matching_font *hole = next;
            for (matching_font *prev = hole - 1; is_better(s, &val, prev); prev--) {
                *hole = *prev;
                hole = prev;
            }
            *hole = val;
        }
    }
}

static void med3(matching_font *first, matching_font *mid, matching_font *last, const matching_style *s) {
    if (is_better(s, mid, first)) swap_mf(mid, first);
    if (is_better(s, last, mid)) {
        swap_mf(mid, last);
        if (is_better(s, mid, first)) swap_mf(mid, first);
    }
}

static void guess_median(matching_font *first, matching_font *mid, matching_font *last, const matching_style *s) {
    ptrdiff_t count = last - first;
    if (40 < count) {
        ptrdiff_t step = (count + 1) >> 3, two = step << 1;
        med3(first, first + step, first + two, s);
        med3(mid - step, mid, mid + step, s);
        med3(last - two, last - step, last, s);
        med3(first + step, mid, last - step, s);
    } else
        med3(first, mid, last, s);
}

static void partition(matching_font *first, matching_font *last, const matching_style *s, matching_font **out_first,
                      matching_font **out_last) {
    matching_font *mid = first + ((last - first) >> 1);
    guess_median(first, mid, last - 1, s);
    matching_font *pfirst = mid, *plast = pfirst + 1;
    while (first < pfirst && !is_better(s, pfirst - 1, pfirst) && !is_better(s, pfirst, pfirst - 1)) --pfirst;
    while (plast < last && !is_better(s, plast, pfirst) && !is_better(s, pfirst, plast)) ++plast;
    matching_font *gfirst = plast, *glast = pfirst;
    for (;;) {
        for (; gfirst < last; ++gfirst) {
            if (is_better(s, pfirst, gfirst)) continue;
            else if (is_better(s, gfirst, pfirst)) break;
            else if (plast != gfirst) { swap_mf(plast, gfirst); ++plast; }
            else ++plast;
        }
        for (; first < glast; --glast) {
            matching_font *gp = glast - 1;
            if (is_better(s, gp, pfirst)) continue;
            else if (is_better(s, pfirst, gp)) break;
            else if (--pfirst != gp) swap_mf(pfirst, gp);
        }
        if (glast == first && gfirst == last) {
            *out_first = pfirst;
            *out_last = plast;
            return;
        }
        if (glast == first) {
            if (plast != gfirst) swap_mf(pfirst, plast);
            ++plast;
            swap_mf(pfirst, gfirst);
            ++pfirst;
            ++gfirst;
        } else if (gfirst == last) {
            if (--glast != --pfirst) swap_mf(glast, pfirst);
            swap_mf(pfirst, --plast);
        } else
            swap_mf(gfirst++, --glast);
    }
}

static void pop_heap_hole(matching_font *first, ptrdiff_t hole, ptrdiff_t bottom, matching_font val,
                          const matching_style *s) {
    const ptrdiff_t top = hole;
    ptrdiff_t idx = hole;
    const ptrdiff_t max_seq = (bottom - 1) >> 1;
    while (idx < max_seq) {
        idx = 2 * idx + 2;
        if (is_better(s, first + idx, first + idx - 1)) --idx;
        first[hole] = first[idx];
        hole = idx;
    }
    if (idx == max_seq && bottom % 2 == 0) {
        first[hole] = first[bottom - 1];
        hole = bottom - 1;
    }
    for (idx = (hole - 1) >> 1; top < hole && is_better(s, first + idx, &val); idx = (hole - 1) >> 1) {
        first[hole] = first[idx];
        hole = idx;
    }
    first[hole] = val;
}

static void heap_sort(matching_font *first, matching_font *last, const matching_style *s) {
    ptrdiff_t bottom = last - first;
    for (ptrdiff_t hole = bottom >> 1; 0 < hole;) {
        --hole;
        pop_heap_hole(first, hole, bottom, first[hole], s);
    }
    for (; 2 <= last - first; --last) {
        matching_font val = last[-1];
        last[-1] = *first;
        pop_heap_hole(first, 0, last - 1 - first, val, s);
    }
}

static void msvc_sort(matching_font *first, matching_font *last, ptrdiff_t ideal, const matching_style *s) {
    for (;;) {
        if (last - first <= 32) {
            insertion_sort(first, last, s);
            return;
        }
        if (ideal <= 0) {
            heap_sort(first, last, s);
            return;
        }
        matching_font *mf, *ml;
        partition(first, last, s, &mf, &ml);
        ideal = (ideal >> 1) + (ideal >> 2);
        if (mf - first < last - ml) {
            msvc_sort(first, mf, ideal, s);
            first = ml;
        } else {
            msvc_sort(ml, last, ideal, s);
            last = mf;
        }
    }
}

/* DWriteFontFamily::GetMatchingFonts 0x180040db0 → FontSet::ApplySimulationsForWwsFallback 0x180040f5c:
 * the family's fonts in set order, each with the simulations the request needs, sorted by
 * MatchingStyle, then the fonts whose FontFaceKey has its high bit set dropped unless they have
 * the best one's stretch, weight and style or are its stretch's regular
 * (<lambda_00c964407f05052c6f4652953b47ff65> 0x1800c8020). Returns the count; `out` has room
 * for the family's font count. */
static uint32_t matching_fonts(const idwr_font_set *set, int32_t family, uint32_t weight, uint32_t stretch,
                               uint32_t style, matching_font *out) {
    const fs_family *fam = &set->families[family];
    uint32_t n = fam->nfonts;
    for (uint32_t i = 0; i < n; i++)
        matching_font_init(&out[i], &set->fonts[fam->fonts[i]], fam->fonts[i], weight, style);
    matching_style s = {((int32_t)stretch - 5) * 0x44c, (int32_t)style * 700, ((int32_t)weight - 400) * 5};
    if (!NC(3)) msvc_sort(out, out + n, (ptrdiff_t)n, &s);
    if (n < 2) return n;
    const fs_font *best = out[0].entry;
    uint32_t k = 1;
    for (uint32_t i = 1; i < n; i++) {
        const fs_font *e = out[i].entry;
        int drop = e->instance_filter &&
                   (e->stretch != best->stretch ||
                    ((e->weight != best->weight || e->style != best->style) && (e->weight != 400 || e->style != 0)));
        if (!drop) out[k++] = out[i];
    }
    return k;
}

IDWR_API uint32_t idwr_font_set_matching_fonts(const idwr_font_set *set, int32_t family, uint16_t weight,
                                               uint8_t stretch, uint8_t style, int32_t *fonts,
                                               uint32_t *simulations, uint32_t cap) {
    if (!set || family < 0 || (uint32_t)family >= set->nfamilies) return 0;
    uint32_t n = set->families[family].nfonts;
    matching_font *m = malloc((n ? n : 1) * sizeof *m);
    if (!m) return 0;
    n = matching_fonts(set, family, weight, stretch, style, m);
    for (uint32_t i = 0; i < n && i < cap; i++) {
        if (fonts) fonts[i] = m[i].font;
        if (simulations) simulations[i] = m[i].sims;
    }
    free(m);
    return n < cap ? n : cap;
}

/* ---------------------------------------------------------------- locale tags (LanguageTagImpl) */

#define TAG_MAX 0x55

/* GetLocaleInfoEx(LOCALE_SPARENT) lowercased, from fallback_nls.h (non-prefix parents only):
 * writes the parent and returns its length, or 0 */
static uint32_t nls_parent(const uint16_t *tag, uint16_t *out) {
    char t[TAG_MAX + 1];
    if (NC(6)) return 0;
    uint32_t n = 0;
    for (; tag[n]; n++) {
        if (n >= TAG_MAX || tag[n] >= 0x80) return 0;
        uint32_t c = tag[n];
        t[n] = (char)(c >= 'A' && c <= 'Z' ? c + 32 : c);
    }
    t[n] = 0;
    uint32_t lo = 0, hi = sizeof idwr_nls_parent / sizeof idwr_nls_parent[0];
    while (lo < hi) {
        uint32_t mid = (lo + hi) / 2;
        int c = strcmp(idwr_nls_parent[mid][0], t);
        if (c < 0) lo = mid + 1;
        else if (c > 0) hi = mid;
        else {
            const char *p = idwr_nls_parent[mid][1];
            uint32_t k = 0;
            for (; p[k]; k++) out[k] = (uint8_t)p[k];
            out[k] = 0;
            return k;
        }
    }
    return 0;
}

/* LanguageTagImpl::IsSubtagEndChar 0x18002e9e4 */
static int subtag_end(uint32_t c) { return c == 0 || c == '-' || c == ';'; }

/* `range` (up to NUL or ';') is a prefix of `tag` ending at a subtag boundary */
static int prefix_of(const uint16_t *range, const uint16_t *tag) {
    uint32_t i = 0;
    for (; range[i] && range[i] != ';'; i++)
        if (range[i] != tag[i]) return 0;
    return subtag_end(tag[i]);
}

/* LanguageTagImpl::RangeIncludes 0x180035948 (LangGetNonPrefixParent 0x180035c18, GetPrefixLength
 * 0x180035ce4). The NLS table holds only non-prefix parents, which is all both read. */
static int range_includes(const uint16_t *range, const uint16_t *tag) {
    uint16_t parent[TAG_MAX + 1], t[TAG_MAX + 1];
    if (!range[0]) return 1;
    if (prefix_of(range, tag)) return 1;
    if (nls_parent(tag, parent) && prefix_of(range, parent)) return 1;
    uint32_t len = 0;  /* GetPrefixLength: the position of the last '-' within the first 0x55 units */
    for (uint32_t i = 0; tag[i] && i < TAG_MAX; i++)
        if (tag[i] == '-') len = i;
    if (len == 0) return 0;
    memcpy(t, tag, len * sizeof *t);
    t[len] = 0;
    for (;;) {
        if (nls_parent(t, parent) && prefix_of(range, parent)) return 1;
        uint32_t cut = 0;
        for (uint32_t i = 0; t[i] && i < TAG_MAX; i++)
            if (t[i] == '-') cut = i;
        if (cut == 0) return 0;
        t[cut] = 0;
    }
}

/* LanguageTagImpl::ValidateAndNormalize 0x18001cde0: lowercases one tag in place; 0 if malformed */
static int normalize_tag(uint16_t *s, uint32_t len) {
    if (len == 0) return 1;
    uint32_t i = 0;
    for (; i < len; i++) {
        uint32_t c = s[i];
        if (c < 'a') {
            if ((uint16_t)(c - 'A') > 25) break;
            s[i] = (uint16_t)(c + 32);
        } else if (c > 'z')
            break;
    }
    if (i - 1 >= 8) return 0;
    while (i < len) {
        if (s[i] != '-' && s[i] != '_') return 0;
        uint32_t start = ++i;
        for (; i < len; i++) {
            uint32_t c = s[i];
            if (c < 'a') {
                if ((uint16_t)(c - '0') > 9) {
                    if ((uint16_t)(c - 'A') > 25) break;
                    s[i] = (uint16_t)(c + 32);
                }
            } else if (c > 'z')
                break;
        }
        if (i == start || i - start > 8) return 0;
    }
    return 1;
}

/* LanguageTagImpl::ValidateAndNormalizeList 0x180035d18: tags separated by ',' or ';' (or NUL),
 * each normalized, separators replaced by NUL */
static int normalize_list(uint16_t *s, uint32_t len) {
    uint32_t start = 0, i = 0;
    for (; i < len; i++)
        if (s[i] == 0 || s[i] == ',' || s[i] == ';') {
            if (!normalize_tag(s + start, i - start)) return 0;
            start = i + 1;
            s[i] = 0;
        }
    return normalize_tag(s + start, i - start);
}

/* ---------------------------------------------------------------- FontFallback::MapCharacters */

typedef struct {
    uint32_t first, last;
} urange;

typedef struct {
    const idwr_font_set *set;
    const uint16_t *text;
    uint32_t len;
    int32_t font;       /* +0x00 */
    uint32_t sims;
    float scale;        /* +0x48 */
    uint32_t weight, style, stretch;  /* +0x64, +0x68, +0x6c */
    uint32_t mapped;    /* +0x74 */
    uint32_t unmapped;  /* +0x78: the length returned when nothing maps */
    uint32_t limit;     /* +0x7c: the run still to be mapped */
    matching_font *scratch;
} fb_state;

/* FindTextRangeEnd 0x1800356c4 */
static void find_text_range_end(fb_state *st, urange r) {
    if (!st->limit) return;
    uint32_t i = 0, prev = 0;
    while (i < st->limit) {
        uint32_t c = cp_at(st->text, st->len, i);
        if (c < r.first || r.last < c) {
            if (!ucd_bit(IDWR_UCD_EXTEND_PROP, c) && !is_joiner(prev)) break;
            if (i != 0 && i < st->unmapped) st->unmapped = i;
        }
        prev = c;
        i += advance(st->text, st->len, i);
    }
    if (i < st->unmapped) st->unmapped = i;
    st->limit = i;
}

/* MatchFontToText 0x180034fc0, for a local font (no font face reference, not EUDC) */
static void match_font_to_text(fb_state *st, const fs_font *f) {
    uint32_t i = 0, unmapped = st->unmapped, prev = 0;
    int state = 1;
    while (i < st->limit) {
        uint32_t c = cp_at(st->text, st->len, i);
        int covered = font_has(f, c), cov = covered, next = state;
        if (cov != state) {
            next = cov;
            if (i != 0) {
                if (covered || NC(5) || !ucd_bit(IDWR_UCD_IGNORABLE_PROP, c)) {
                    if (i < unmapped) unmapped = i;
                    if (!cov) break;
                    next = state;
                    if (!ucd_bit(IDWR_UCD_EXTEND_PROP, c) && !is_joiner(prev)) break;
                } else
                    next = state;
            }
        }
        state = next;
        prev = c;
        i += advance_one(st->text, st->len, i);
    }
    st->limit = i;
    st->unmapped = unmapped;
    st->mapped = state ? i : 0;
}

/* MatchFamilyToText 0x180034bd8 (GetFontFamily 0x180035db8, GetMatchingFonts) */
static void match_family_to_text(fb_state *st, const uint16_t *name) {
    int32_t fam = find_family(st->set, name);
    if (fam < 0) return;
    uint32_t n = matching_fonts(st->set, fam, st->weight, st->stretch, st->style, st->scratch);
    for (uint32_t i = 0; i < n; i++) {
        match_font_to_text(st, st->scratch[i].entry);
        if (st->mapped != 0 || (st->limit == 0 && st->font < 0)) {
            st->font = st->scratch[i].font;
            st->sims = st->scratch[i].sims;
            return;
        }
    }
}

static const uint8_t *ff_at(uint32_t off) { return idwr_fontfallback + off; }

/* a UTF-16LE string of the region */
static void ff_string(uint32_t off, uint16_t *out, uint32_t cap) {
    uint32_t k = 0;
    for (; k + 1 < cap && le16(ff_at(off + 2 * k)); k++) out[k] = (uint16_t)le16(ff_at(off + 2 * k));
    out[k] = 0;
}

/* FontFallbackLookupIterator */
typedef struct {
    uint32_t lm, lm_end;        /* this+8, this+0x18: LookupMapping cursor */
    uint32_t lm_begin;          /* this+0x10 */
    uint32_t t, t_end;          /* this+0x20, this+0x28: target cursor (byte offsets) */
    const uint16_t *base;       /* this+0x30 */
    const uint16_t *chosen;     /* this+0x38 */
    const uint16_t *locales, *locales_end;  /* this+0x40, this+0x48 */
    uint32_t collection;        /* this+0x58 */
    float scale;                /* this+0x5c */
} lookup_iter;

/* FontFallbackLookupIterator::FindNextMapping 0x1800357e0 */
static int find_next_mapping(lookup_iter *it) {
    uint16_t buf[TAG_MAX * 4];
    for (;; it->lm += 4) {
        if (it->lm == it->lm_end) {
            it->t = it->t_end = 0;
            it->collection = 0;
            it->scale = 1.0f;
            return 0;
        }
        uint32_t m = le32(ff_at(it->lm));
        if (le32(ff_at(m + 0xc)) == 0) continue;
        uint32_t bo = le32(ff_at(m + 0x14));
        if (le16(ff_at(bo))) {
            ff_string(bo, buf, sizeof buf / sizeof buf[0]);
            if (!name_equal(it->base, buf)) continue;
        }
        ff_string(le32(ff_at(m + 0x10)), buf, sizeof buf / sizeof buf[0]);
        if (range_includes(buf, it->chosen)) {
            it->t = le32(ff_at(m + 8));
            it->t_end = it->t + le32(ff_at(m + 0xc)) * 4;
            it->collection = le32(ff_at(m + 0x18));
            uint32_t sc = le32(ff_at(m + 0x1c));
            memcpy(&it->scale, &sc, 4);
            return 1;
        }
    }
}

/* FontFallbackLookupIterator::GetFirstMatchingLocale 0x1800358ac */
static const uint16_t *first_matching_locale(lookup_iter *it) {
    uint16_t buf[TAG_MAX * 4];
    const uint16_t *s = it->locales;
    for (;;) {
        if (s == it->locales_end) return it->locales;
        for (uint32_t p = it->lm_begin; p != it->lm_end; p += 4) {
            uint32_t m = le32(ff_at(p));
            uint32_t lo = le32(ff_at(m + 0x10));
            if (le16(ff_at(lo))) {
                ff_string(lo, buf, sizeof buf / sizeof buf[0]);
                if (range_includes(buf, s)) return s;
            }
        }
        while (s < it->locales_end && *s) s++;
        if (s == it->locales_end) return it->locales;
        s++;
    }
}

/* ClampUnicodeRange 0x180035660 */
static urange clamp_range(uint32_t ch, urange r, urange p) {
    if (r.last < p.first || r.first > p.last) return r;
    if (ch < p.first) { r.last = p.first - 1; return r; }
    if (ch > p.last) { r.first = p.last + 1; return r; }
    if (r.first < p.first) r.first = p.first;
    if (p.last < r.last) r.last = p.last;
    return r;
}

static const urange PUA = {0xe000, 0xf8ff};

/* FontFallback::MapCharacters 0x180034344 with the system fallback's data, the set as collection 0
 * (and as the base collection), no axis values, and an empty EUDC collection (the persona has no
 * end-user-defined characters). base_font >= 0 is TextLayout's base font (TextLayout::MapCharacters
 * 0x18002f5c8 passes the format's first matching font). */
static int32_t map_characters(const idwr_font_set *set, const uint16_t *text, uint32_t len, const char *locale,
                              const char *default_locale, const char *base_family, uint32_t weight, uint32_t style,
                              uint32_t stretch, int32_t base_font, uint32_t base_sims, idwr_fallback_result *out) {
    uint32_t maxfonts = 1;
    for (uint32_t f = 0; f < set->nfamilies; f++)
        if (set->families[f].nfonts > maxfonts) maxfonts = set->families[f].nfonts;
    fb_state st = {set, text, len, base_font, base_sims, 1.0f, weight, style, stretch, 0, len, len, NULL};
    st.scratch = malloc(maxfonts * sizeof *st.scratch);
    uint16_t *base = utf16(base_family ? base_family : "", NULL);
    uint16_t *loc = NULL;
    int32_t hr = 0;
    if (!st.scratch || !base) { hr = HR_E_OUTOFMEMORY; goto done; }
    uint32_t ch = len ? cp_at(text, len, 0) : 0xffffffffu;

    if (base_font >= 0) match_font_to_text(&st, &set->fonts[base_font]);
    if (st.mapped == 0 && (!base[0] || (match_family_to_text(&st, base), st.mapped == 0))) {
        if (ch - 0xe000 < 0x1900) {
            /* GetEudcFontCollection gives a collection with no family: the range end is still set */
            find_text_range_end(&st, PUA);
        }
    } else if (len != 0 || st.font >= 0)
        goto done;

    /* the locale: the source's (one locale for the whole text here), else the user default */
    {
        const char *l = locale && *locale ? locale : default_locale ? default_locale : "";
        uint32_t ln = 0;
        loc = utf16(l, &ln);
        if (!loc) { hr = HR_E_OUTOFMEMORY; goto done; }
        /* a source locale goes through ValidateAndNormalizeList; the user default
         * (GenericLanguageTag(UserDefaultTag)) is already a normalized tag */
        if (!normalize_list(loc, ln) && locale && *locale) { hr = HR_E_INVALIDARG; goto done; }

        uint32_t nlookups = le32(ff_at(8)), lookups = le32(ff_at(0x20));
        uint32_t lo = lookups, count = nlookups;  /* first lookup whose last >= ch */
        while (count > 0) {
            uint32_t half = count >> 1;
            if (le32(ff_at(lo + half * 16 + 4)) < ch) {
                lo += half * 16 + 16;
                count = count - 1 - half;
            } else
                count = half;
        }
        urange lr = {le32(ff_at(lo)), le32(ff_at(lo + 4))};
        lookup_iter it;
        memset(&it, 0, sizeof it);
        it.lm_begin = it.lm = le32(ff_at(lo + 8));
        it.lm_end = it.lm + le32(ff_at(lo + 12)) * 4;
        it.base = base;
        it.locales = loc;
        it.locales_end = loc + ln;
        it.collection = 0;
        it.scale = 1.0f;
        it.chosen = first_matching_locale(&it);
        find_next_mapping(&it);
        find_text_range_end(&st, clamp_range(ch, lr, PUA));
        while (st.mapped == 0 && it.t != it.t_end) {
            if (it.collection == 0) {  /* collections other than 0 are null in the system fallback */
                uint16_t name[256];
                uint32_t tp = it.t;
                if (NC(1)) {  /* the mapping's targets in reverse order */
                    uint32_t m = le32(ff_at(it.lm)), t0 = le32(ff_at(m + 8));
                    tp = t0 + (le32(ff_at(m + 0xc)) - 1) * 4 - (it.t - t0);
                }
                ff_string(le32(ff_at(tp)), name, 256);
                match_family_to_text(&st, name);
                if (st.mapped != 0 || st.limit == 0) {
                    st.scale = it.scale;
                    break;
                }
            }
            it.t += 4;
            if (it.t == it.t_end && it.lm != it.lm_end) {
                it.lm += 4;
                find_next_mapping(&it);
            }
        }
    }
done:
    if (out && hr == 0) {
        out->mapped_length = st.mapped ? st.mapped : st.unmapped;
        out->font = st.font;
        out->family = st.font >= 0 ? set->fonts[st.font].family : -1;
        out->simulations = st.font >= 0 ? st.sims : 0;
        out->scale = st.scale;
    }
    free(st.scratch);
    free(base);
    free(loc);
    return hr;
}

IDWR_API int32_t idwr_fallback_map_characters(const idwr_font_set *set, const uint16_t *text, uint32_t length,
                                              const char *locale, const char *default_locale,
                                              const char *base_family, uint16_t weight, uint8_t style,
                                              uint8_t stretch, idwr_fallback_result *out) {
    if (!set || (!text && length) || !out) return HR_E_INVALIDARG;
    return map_characters(set, text, length, locale, default_locale, base_family, weight, style, stretch, -1, 0, out);
}

IDWR_API int32_t idwr_fallback_layout_family(const idwr_font_set *set, uint32_t codepoint, const char *locale,
                                             const char *base_family, uint16_t weight, uint8_t style,
                                             uint8_t stretch, int32_t *font) {
    if (font) *font = -1;
    if (!set || codepoint > 0x10ffff) return -1;
    /* the format's font: RecentFontCache::GetFirstMatchingFont, the family's first matching font */
    int32_t base_font = -1;
    uint32_t base_sims = 0;
    uint16_t *b = utf16(base_family ? base_family : "", NULL);
    int32_t bf = b ? find_family(set, b) : -1;
    free(b);
    if (bf >= 0 && set->families[bf].nfonts) {
        matching_font *m = malloc(set->families[bf].nfonts * sizeof *m);
        if (!m) return -1;
        if (matching_fonts(set, bf, weight, stretch, style, m)) {
            base_font = m[0].font;
            base_sims = m[0].sims;
        }
        free(m);
    }
    /* TextLayout::MapCharacters keeps a NO_VISUAL run in the format's font */
    if (NC(7) || !idwr_fallback_no_visual(codepoint)) {
        uint16_t t[2];
        uint32_t n = 1;
        if (codepoint >= 0x10000) {
            t[0] = (uint16_t)(0xd800 + ((codepoint - 0x10000) >> 10));
            t[1] = (uint16_t)(0xdc00 + (codepoint & 0x3ff));
            n = 2;
        } else
            t[0] = (uint16_t)codepoint;
        idwr_fallback_result r;
        if (map_characters(set, t, n, locale, locale, base_family, weight, style, stretch, base_font, base_sims, &r) == 0 &&
            r.font >= 0) {
            if (font) *font = r.font;
            return r.family;
        }
    }
    if (base_font < 0) return -1;
    if (font) *font = base_font;
    return set->fonts[base_font].family;
}
