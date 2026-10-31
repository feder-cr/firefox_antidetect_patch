/*
 * The weight DWrite.dll 10.0.26100.9711 gives a face: what OpenTypeFontFaceBuilder::ReadFontFace
 * (0x18005e0e0) stores at builder + 0x68, which is FontFace data + 0x58, the word FontFaceLight::
 * GetFontWeight (0x180024a54) reads (notes/recommended-mode.md, "The weight words of the names"):
 *
 *   STAT present (OpenTypeFace + 0x1b4 and + 0x1b8, the table entry, both non-zero)
 *       -> FontStyleAttributes::GetWeight (0x1800e43dc) over the face's axis values (idwr_face_weight,
 *          variation.c): the wght value, else GetFontWeightFromFace (0x180021fc4, the OS/2 rule);
 *   otherwise
 *       -> FontDifferentiator (0x180021114) + 0xb2: the OS/2 weight resolved with the weight words of
 *          the names (this file).
 *
 * FontDifferentiator, as far as the weight goes:
 *   OS/2 weight w (GetFontWeightFromFace). The lists of name IDs 16, 17, 21, 22 (FontNameList::
 *   AppendItems 0x18001bda0: records with a language tag and a supported encoding). The canonical
 *   family and subfamily and their LCID (TryGetCanonicalNamesAndLanguage 0x180022044: ID 1 and ID 2
 *   records of an English LCID, the lowest LCID both have). The weight stays w when
 *   (ID 16 or 17 listed and ID 21 and 22 listed), no canonical names, STAT present, or OS/2
 *   fsSelection bit 8 (WWS). Otherwise:
 *     family = name 16 in that LCID (TryGetName 0x180022c90) or the canonical family,
 *     face   = name 17 in that LCID or the canonical subfamily,
 *     full   = CombineFamilyAndFaceName(Trim(family), RemoveRegularFromFaceName(Trim(face))),
 *   cut into tokens (FontDifferentiatorTokens 0x180020f34, delimiters ' ' '-' '.' '_'); the last style
 *   word (ParseFontStyleName 0x18002326c) and the best stretch words (ExtractFontStretch 0x180023570)
 *   are deleted, then ExtractFontWeight (0x180023974) takes the best weight words from token 1 on;
 *   without any, and with names 16 and 17 both found and full == the canonical family,
 *   ExtractAbbreviatedFontWeight (0x1800df35c). Unless ID 21 and 22 are listed, the weight is
 *   GetResolvedWeight (0x1800dcbc8) of w and the name weight.
 *
 * The word lists and the weight, stretch and score tables are DWrite.dll's (read_bytes, notes).
 */
#include <stdlib.h>
#include <string.h>
#include "idwr.h"
#include "idwr_internal.h"
#include "lcid_table.h"

#define TAG(a, b, c, d) IDWR_TAG(a, b, c, d)

/* ---------------------------------------------------------------------------------------------- */
/* strings                                                                                          */
/* ---------------------------------------------------------------------------------------------- */

typedef struct {
    uint16_t *s;
    uint32_t n, cap;
    int bad;
} wstr;

static void ws_push(wstr *w, uint16_t c) {
    if (w->n == w->cap) {
        uint32_t cap = w->cap ? 2 * w->cap : 32;
        uint16_t *p = realloc(w->s, cap * sizeof *p);
        if (!p) {
            w->bad = 1;
            return;
        }
        w->s = p;
        w->cap = cap;
    }
    w->s[w->n++] = c;
}

static void ws_set(wstr *w, const uint16_t *s, uint32_t n) {
    w->n = 0;
    for (uint32_t i = 0; i < n; i++) ws_push(w, s[i]);
}

static void ws_free(wstr *w) {
    free(w->s);
    memset(w, 0, sizeof *w);
}

static int ws_equal(const wstr *a, const wstr *b) {  /* DWrite::operator== 0x18003d2b4: ordinal */
    return a->n == b->n && (a->n == 0 || memcmp(a->s, b->s, a->n * sizeof *a->s) == 0);
}

/* FontDifferentiatorTokens::IsDelimiter 0x1800ff350: c - 0x20 < 0x40 and bit of 0x8000000000006001 */
static int is_delim(uint16_t c) {
    uint16_t d = (uint16_t)(c - 0x20);
    return d < 0x40 && (0x8000000000006001ull >> d & 1);
}

/* the "c | 0x20" comparisons of the word parsers, against a lowercase ASCII word */
static int word_is(const uint16_t *s, uint32_t n, const char *w) {
    uint32_t k = (uint32_t)strlen(w);
    if (n != k) return 0;
    for (uint32_t i = 0; i < k; i++)
        if ((uint16_t)(s[i] | 0x20) != (uint16_t)((uint8_t)w[i] | 0x20)) return 0;
    return 1;
}

static int prefix_is(const uint16_t *s, const char *w) {
    for (uint32_t i = 0; w[i]; i++)
        if ((uint16_t)(s[i] | 0x20) != (uint16_t)((uint8_t)w[i] | 0x20)) return 0;
    return 1;
}

/* CompareStringW(LOCALE_INVARIANT, NORM_IGNORECASE) == CSTR_EQUAL on two runs of equal length, through
 * the simple case pairs of ASCII, Latin-1, Latin Extended-A, Greek and Cyrillic (notes: other scripts,
 * ignorable characters and expansions are not modelled) */
static uint16_t fold(uint16_t c) {
    if (c >= 'A' && c <= 'Z') return (uint16_t)(c + 0x20);
    if ((c >= 0xc0 && c <= 0xde && c != 0xd7)) return (uint16_t)(c + 0x20);
    if (c == 0x178) return 0xff;
    if (c >= 0x100 && c <= 0x17f && c != 0x130 && c != 0x131 && c != 0x138 && c != 0x149 && c != 0x17f) {
        int odd_upper = (c >= 0x139 && c <= 0x148) || (c >= 0x179 && c <= 0x17e);
        if (odd_upper ? (c & 1) : !(c & 1)) return (uint16_t)(c + 1);
        return c;
    }
    if (c >= 0x391 && c <= 0x3ab && c != 0x3a2) return (uint16_t)(c + 0x20);
    if (c >= 0x410 && c <= 0x42f) return (uint16_t)(c + 0x20);
    if (c >= 0x400 && c <= 0x40f) return (uint16_t)(c + 0x50);
    return c;
}

static int equal_ignore_case(const uint16_t *a, const uint16_t *b, uint32_t n) {
    for (uint32_t i = 0; i < n; i++)
        if (fold(a[i]) != fold(b[i])) return 0;
    return 1;
}

/* ---------------------------------------------------------------------------------------------- */
/* the name table                                                                                   */
/* ---------------------------------------------------------------------------------------------- */

typedef struct {
    const uint8_t *recs, *storage;
    uint32_t count, storage_len;
} name_table;

typedef struct {
    uint16_t platform, encoding, language, id;
    const uint8_t *data;
    uint32_t len;
} name_rec;

static name_rec rec_at(const name_table *t, uint32_t i) {
    const uint8_t *r = t->recs + 12 * i;
    name_rec n = {idwr_u16(r), idwr_u16(r + 2), idwr_u16(r + 4), idwr_u16(r + 6), NULL, idwr_u16(r + 8)};
    n.data = t->storage + idwr_u16(r + 10);
    return n;
}

static int rec_in_range(const name_table *t, uint32_t i) {  /* the OnOutOfRange checks of the readers */
    const uint8_t *r = t->recs + 12 * i;
    uint32_t len = idwr_u16(r + 8), off = idwr_u16(r + 10);
    return off <= t->storage_len && t->storage_len - off >= len;
}

/* OpenTypeNameTable::OpenTypeNameTable 0x180022b10; -1 where DWrite throws FileFormatException
 * (here and when a record the differentiator reads points outside the storage) */
static int name_open(const idwr_font *f, name_table *t) {
    uint32_t len;
    const uint8_t *p = idwr_font_table(f, TAG('n', 'a', 'm', 'e'), &len);
    memset(t, 0, sizeof *t);
    if (!p || len == 0) return 0;
    if (len < 6) return -1;
    uint32_t count = idwr_u16(p + 2), storage = idwr_u16(p + 4);
    if ((len - 6) / 12 < count || storage > len) return -1;
    t->recs = p + 6;
    t->count = count;
    t->storage = p + storage;
    t->storage_len = len - storage;
    for (uint32_t i = 0; i < count; i++) {
        uint16_t id = rec_at(t, i).id;
        if ((id == 1 || id == 2 || id == 5 || id == 16 || id == 17 || id == 21 || id == 22) && !rec_in_range(t, i))
            return -1;
    }
    return 0;
}

/* UTF-16BE up to the first zero unit (TryParseNameString 0x18001d110 encodings 2, 3, 4, 8, 9) */
static void decode_utf16(const uint8_t *p, uint32_t len, wstr *out) {
    out->n = 0;
    for (uint32_t i = 0; i < len / 2; i++) {
        uint16_t c = idwr_u16(p + 2 * i);
        if (!c) break;
        ws_push(out, c);
    }
}

/* ParseMacRomanName 0x18001ae10: a NUL only as the last byte drops it; a NUL anywhere else keeps every
 * byte (NULs decode to U+0000) */
static void decode_macroman(const uint8_t *p, uint32_t len, wstr *out) {
    out->n = 0;
    const uint8_t *nul = memchr(p, 0, len);
    if (nul && nul == p + len - 1) len--;
    for (uint32_t i = 0; i < len; i++) ws_push(out, (uint16_t)(p[i] < 0x80 ? p[i] : idwr_macroman_to_unicode(p[i])));
}

/* ConvertOpenTypeName 0x18006054c (code pages 936, 950, 949): the NUL bytes removed, then
 * MultiByteToWideChar. Modelled by structure: an ASCII byte is itself, a lead byte (0x81-0xfe) takes
 * the next byte with it into one character ('?' when the pair is not valid), 0x80 and 0xff are one
 * character. The non-ASCII characters are opaque codes (lead << 8 | trail), not their Unicode values:
 * they are never delimiters nor letters of a word, only their identity in a comparison differs */
static void decode_dbcs(const uint8_t *p, uint32_t len, wstr *out) {
    out->n = 0;
    uint8_t *b = malloc(len ? len : 1);
    if (!b) {
        out->bad = 1;
        return;
    }
    uint32_t n = 0;
    for (uint32_t i = 0; i < len; i++)
        if (p[i]) b[n++] = p[i];
    for (uint32_t i = 0; i < n; i++) {
        uint8_t c = b[i];
        if (c < 0x80) ws_push(out, c);
        else if (c >= 0x81 && c <= 0xfe) {
            uint16_t t = i + 1 < n ? b[++i] : 0;
            ws_push(out, (uint16_t)(c << 8 | t));
        } else ws_push(out, (uint16_t)(0xf800 | c));
    }
    free(b);
}

/* OpenTypeName::TryGetName 0x18001d458 and TryParseNameString with PlatformIdAndEncodingIdToCharacterEncoding:
 * 0 for an encoding they do not read */
static int decode(const name_rec *r, wstr *out) {
    if (r->platform == 0) {
        decode_utf16(r->data, r->len, out);
        return 1;
    }
    if (r->platform == 1) {
        if (r->encoding != 0) return 0;
        decode_macroman(r->data, r->len, out);
        return 1;
    }
    if (r->platform == 3) {
        switch (r->encoding) {
        case 0: case 1: case 2: case 10:
            decode_utf16(r->data, r->len, out);
            return 1;
        case 3: case 4: case 5:
            decode_dbcs(r->data, r->len, out);
            return 1;
        }
    }
    return 0;
}

static int lcid_rejected(uint16_t id) {
    for (unsigned i = 0; i < sizeof idwr_lcid_rejected / sizeof idwr_lcid_rejected[0]; i++)
        if (idwr_lcid_rejected[i] == id) return 1;
    return 0;
}

/* FontNameList::AppendItems 0x18001bda0 leaves the list of `id` empty unless a record has a language tag
 * and an encoding it decodes (platform 3: 0-5, 10; platform 1: 0). OpenTypeName::TryGetLanguage 0x18001caf0:
 * platform 3 "und" for 0x0f00 and 0x1000; an LCID LCIDToLocaleName rejects keeps the record too [output];
 * an LCID whose name GenericLanguageTag rejects (lcid_table.h: 0x007f) drops it. Platform 1: language 0
 * only. Platform 0: none. */
static int name_listed(const name_table *t, uint16_t id) {
    for (uint32_t i = 0; i < t->count; i++) {
        name_rec r = rec_at(t, i);
        if (r.id != id) continue;
        if (r.platform == 3) {
            int und = ((uint16_t)(r.language - 0xf00) & 0xfeff) == 0;
            if ((und || !lcid_rejected(r.language)) && (r.encoding <= 5 || r.encoding == 10)) return 1;
        } else if (r.platform == 1 && r.language == 0 && r.encoding == 0) {
            return 1;
        }
    }
    return 0;
}

/* TryGetCanonicalNamesAndLanguage 0x180022044: the name 1 and 2 records of platform 3 with an English
 * language ((language & 0x3ff) == 9, the LCID is the language) and of platform 1 language 0 (LCID 0x409),
 * each list sorted by (LCID, platform 3 first, record order) (OpenTypeNameList<int>::Sort 0x180022840);
 * the first LCID both have where both decode. 0 for none. */
typedef struct {
    uint16_t lcid, p3;
    uint32_t index;
} canon_entry;

static int canon_cmp(const void *a, const void *b) {
    const canon_entry *x = a, *y = b;
    if (x->lcid != y->lcid) return x->lcid < y->lcid ? -1 : 1;
    if (x->p3 != y->p3) return x->p3 ? -1 : 1;
    return x->index < y->index ? -1 : x->index > y->index;
}

static uint16_t canonical_names(const name_table *t, wstr *family, wstr *sub) {
    canon_entry *l1 = malloc((t->count + 1) * sizeof *l1), *l2 = malloc((t->count + 1) * sizeof *l2);
    uint32_t n1 = 0, n2 = 0;
    uint16_t lcid = 0;
    if (!l1 || !l2) goto done;
    for (uint32_t i = 0; i < t->count; i++) {
        name_rec r = rec_at(t, i);
        if (r.id != 1 && r.id != 2) continue;
        canon_entry e = {0, r.platform == 3, i};
        if (r.platform == 3 && (r.language & 0x3ff) == 9) e.lcid = r.language;
        else if (r.platform == 1 && r.language == 0) e.lcid = 0x409;
        else continue;
        if (r.id == 1) l1[n1++] = e;
        else l2[n2++] = e;
    }
    qsort(l1, n1, sizeof *l1, canon_cmp);
    qsort(l2, n2, sizeof *l2, canon_cmp);
    for (uint32_t i = 0, j = 0; i < n1 && j < n2;) {
        if (l1[i].lcid < l2[j].lcid) {
            i++;
        } else if (l1[i].lcid == l2[j].lcid) {
            name_rec a = rec_at(t, l1[i].index), b = rec_at(t, l2[j].index);
            if (!decode(&a, family)) i++;
            else if (!decode(&b, sub)) j++;
            else {
                lcid = l1[i].lcid;
                break;
            }
        } else {
            j++;
        }
    }
done:
    free(l1);
    free(l2);
    return lcid;
}

/* TryGetName 0x180022c90: the records of `id` whose language is `lcid` (platform 3: the language;
 * platform 1: 0x409 for language 0); the first platform-3 one that decodes, else the first other one */
static int name_in(const name_table *t, uint16_t id, uint16_t lcid, wstr *out) {
    int found = 0;
    wstr tmp = {0};
    for (uint32_t i = 0; i < t->count; i++) {
        name_rec r = rec_at(t, i);
        if (r.id != id) continue;
        uint32_t key = r.platform == 3 ? r.language : r.platform == 1 && r.language == 0 ? 0x409 : 0;
        if (key != lcid || (found && r.platform != 3)) continue;
        if (!decode(&r, &tmp)) continue;
        ws_set(out, tmp.s, tmp.n);
        found = 1;
        if (r.platform == 3) break;
    }
    ws_free(&tmp);
    return found;
}

/* ---------------------------------------------------------------------------------------------- */
/* the face name                                                                                    */
/* ---------------------------------------------------------------------------------------------- */

/* Trim 0x180022918: spaces (only U+0020) at both ends */
static void trim(const wstr *in, wstr *out) {
    uint32_t a = 0, b = in->n;
    while (a < b && in->s[a] == ' ') a++;
    while (b > a && in->s[b - 1] == ' ') b--;
    ws_set(out, in->s + a, b - a);
}

/* FindRegularFaceName 0x1800cfa30: from the end, the first position p (not the last three characters)
 * where "roman", "regular", "book", "normal" or "upright" starts, case-insensitive (c | 0x20), with no
 * word boundary check; [p, end of the word). Only for 4 characters or more. */
static int find_regular(const uint16_t *s, uint32_t n, uint32_t *rb, uint32_t *re) {
    if (n < 4) return 0;
    for (uint32_t p = n - 4;; p--) {
        uint32_t rem = n - p - 1;
        uint16_t c = (uint16_t)(s[p] | 0x20);
        const uint16_t *q = s + p + 1;
        if (c == 'r') {
            if (rem >= 4 && prefix_is(q, "oman")) { *rb = p, *re = p + 5; return 1; }
            if (rem >= 6 && prefix_is(q, "egular")) { *rb = p, *re = p + 7; return 1; }
        } else if (c == 'b') {
            if (rem >= 3 && prefix_is(q, "ook")) { *rb = p, *re = p + 4; return 1; }
        } else if (c == 'n') {
            if (rem >= 5 && prefix_is(q, "ormal")) { *rb = p, *re = p + 6; return 1; }
        } else if (c == 'u') {
            if (rem >= 6 && prefix_is(q, "pright")) { *rb = p, *re = p + 7; return 1; }
        }
        if (p == 0) return 0;
    }
}

/* RemoveRegularFromFaceName 0x180020dec: the word cut out with the delimiters around it; the two sides
 * joined by one space */
static void remove_regular(const wstr *in, wstr *out) {
    uint32_t rb, re;
    if (!find_regular(in->s, in->n, &rb, &re)) {
        ws_set(out, in->s, in->n);
        return;
    }
    uint32_t pre = rb, post = re;
    while (pre > 0 && is_delim(in->s[pre - 1])) pre--;   /* ReverseFindIf(not IsDelimiter) */
    while (post < in->n && is_delim(in->s[post])) post++;
    if (pre == 0) {
        ws_set(out, in->s + post, in->n - post);
    } else if (post == in->n) {
        ws_set(out, in->s, pre);
    } else {
        ws_set(out, in->s, pre);
        ws_push(out, ' ');
        for (uint32_t i = post; i < in->n; i++) ws_push(out, in->s[i]);
    }
}

/* ContainsToken 0x180064048: `face` at a word start of `family` (case-insensitive), followed by the end
 * or a delimiter */
static int contains_token(const wstr *family, const wstr *face) {
    uint32_t n = face->n;
    if (n == 0 || n > family->n) return 0;
    uint32_t last = family->n - (n - 1);
    for (uint32_t p = 0; p != last;) {
        if (equal_ignore_case(family->s + p, face->s, n) && (p + n == family->n || is_delim(family->s[p + n])))
            return 1;
        while (p != last && !is_delim(family->s[p])) p++;   /* find_if(IsDelimiter) */
        while (p != last && is_delim(family->s[p])) p++;    /* find_if_not(IsDelimiter) */
    }
    return 0;
}

/* CombineFamilyAndFaceName 0x1800234b0 */
static void combine(const wstr *family, const wstr *face, wstr *out) {
    ws_set(out, family->s, family->n);
    if (face->n != 0 && !contains_token(family, face)) {
        ws_push(out, ' ');
        for (uint32_t i = 0; i < face->n; i++) ws_push(out, face->s[i]);
    }
}

/* ---------------------------------------------------------------------------------------------- */
/* tokens and words                                                                                 */
/* ---------------------------------------------------------------------------------------------- */

typedef struct {
    uint32_t start, end;
    uint8_t deleted;
} token;

typedef struct {
    const uint16_t *s;
    token *t;
    uint32_t n, cap;
    int bad;
} tokens;

static void tok_push(tokens *k, uint32_t a, uint32_t b) {
    if (k->n == k->cap) {
        uint32_t cap = k->cap ? 2 * k->cap : 16;
        token *p = realloc(k->t, cap * sizeof *p);
        if (!p) {
            k->bad = 1;
            return;
        }
        k->t = p;
        k->cap = cap;
    }
    k->t[k->n++] = (token){a, b, 0};
}

/* FontDifferentiatorTokens::FontDifferentiatorTokens 0x180020f34: the words, plus one token for a run
 * of delimiters at the start and one at the end */
static void tokenize(const wstr *w, tokens *k) {
    uint32_t n = w->n, p = 0, q = 0;
    k->s = w->s;
    while (p < n && is_delim(w->s[p])) p++;
    if (p != 0) tok_push(k, 0, p);
    if (p == n) return;
    while (p != n) {
        q = p;
        while (q < n && !is_delim(w->s[q])) q++;
        tok_push(k, p, q);
        p = q;
        while (p < n && is_delim(w->s[p])) p++;
    }
    if (q != n) tok_push(k, q, n);
}

typedef struct {
    const uint16_t *s;
    uint32_t n;
} view;

static view tok_view(const tokens *k, uint32_t i) { return (view){k->s + k->t[i].start, k->t[i].end - k->t[i].start}; }

/* ParseFontStyleName 0x18002326c: non-zero for a style word (strings at 0x1801e0200 .. 0x1801e02a8) */
static int style_word(view v) {
    static const char *const words[] = {"ita", "ital", "italic", "kursiv", "cursive", "oblique", "slanted",
                                        "inclined", "backslant", "backslanted"};
    for (unsigned i = 0; i < sizeof words / sizeof *words; i++)
        if (word_is(v.s, v.n, words[i])) return 1;
    return 0;
}

/* TryParseQualifierPrefix 0x180026d30: the length of a leading qualifier, and the qualifier
 * (1 semi, 2 demi, 3 extra, 4 ultra); "ext" (0x1801dff78) is extra too, with "ra" (0x1801d9928) after it
 * taken when present */
static uint32_t qualifier_prefix(view v, int *q) {
    *q = 0;
    if (v.n < 3) return 0;
    uint16_t c = (uint16_t)(v.s[0] | 0x20);
    if (c == 'd' && v.n >= 4 && prefix_is(v.s, "demi")) return *q = 2, 4;
    if (c == 'e' && prefix_is(v.s, "ext")) {
        *q = 3;
        return v.n >= 5 && prefix_is(v.s + 3, "ra") ? 5 : 3;
    }
    if (c == 's' && v.n >= 4 && prefix_is(v.s, "semi")) return *q = 1, 4;
    if (c == 'u' && v.n >= 5 && prefix_is(v.s, "ultra")) return *q = 4, 5;
    return 0;
}

/* ParseUnqualifiedStretchName 0x180023e84: 0 compressed, 1 cond / condensed, 2 narrow, 3 compact,
 * 4 wide, 5 expanded / extended, 6 none */
static int stretch_name(view v) {
    if (word_is(v.s, v.n, "cond") || word_is(v.s, v.n, "condensed")) return 1;
    if (word_is(v.s, v.n, "wide")) return 4;
    if (word_is(v.s, v.n, "narrow")) return 2;
    if (word_is(v.s, v.n, "compact")) return 3;
    if (word_is(v.s, v.n, "compressed")) return 0;
    if (word_is(v.s, v.n, "expanded") || word_is(v.s, v.n, "extended")) return 5;
    return 6;
}

/* ParseUnqualifiedWeightName 0x180024078: 0 thin, 1 light, 2 medium, 3 demi, 4 bold, 5 ultra, 6 black,
 * 7 heavy / nord, 8 none (strings at 0x1801dff90 .. 0x1801e0070) */
static int weight_name(view v) {
    static const struct { const char *w; int k; } words[] = {
        {"bold", 4}, {"thin", 0}, {"demi", 3}, {"nord", 7}, {"light", 1}, {"black", 6}, {"heavy", 7},
        {"ultra", 5}, {"medium", 2},
    };
    for (unsigned i = 0; i < sizeof words / sizeof *words; i++)
        if (word_is(v.s, v.n, words[i].w)) return words[i].k;
    return 8;
}

/* [name * 5 + qualifier] (qualifier 0 none, 1 semi, 2 demi, 3 extra, 4 ultra): the stretch (0x1801e0338),
 * its score (0x1801e0358), the weight (0x1801e02c0) and its score (0x1801e0310) */
static const uint8_t STRETCH[6][5] = {{2, 0, 0, 1, 1}, {3, 4, 0, 2, 1}, {4, 0, 0, 0, 0},
                                      {4, 0, 0, 0, 0}, {6, 0, 0, 0, 0}, {7, 6, 0, 8, 9}};
static const uint8_t STRETCH_SCORE[6][5] = {{7, 0, 0, 8, 8}, {2, 6, 0, 7, 8}, {6, 0, 0, 0, 0},
                                            {6, 0, 0, 0, 0}, {5, 0, 0, 0, 0}, {1, 5, 0, 4, 3}};
static const uint16_t WEIGHT[8][5] = {{100, 0, 0, 100, 100}, {300, 0, 0, 200, 200}, {500, 0, 0, 0, 0},
                                      {600, 0, 0, 0, 0},     {700, 600, 600, 800, 800}, {800, 0, 0, 0, 0},
                                      {900, 0, 0, 950, 950}, {900, 0, 0, 0, 0}};
static const uint8_t WEIGHT_SCORE[8][5] = {{6, 0, 0, 12, 12}, {5, 0, 0, 11, 11}, {4, 0, 0, 0, 0}, {2, 0, 0, 0, 0},
                                           {7, 10, 10, 9, 9}, {1, 0, 0, 0, 0},  {3, 0, 0, 8, 8}, {3, 0, 0, 0, 0}};

static void mark(tokens *k, uint32_t start, uint32_t count) {
    for (uint32_t i = 0; i < count; i++) k->t[start + i].deleted = 1;   /* MarkAsDeleted 0x1800cc90c */
}

/* the last style word from token 1 on (FontDifferentiator, before ExtractFontStretch) */
static void delete_style(tokens *k) {
    for (uint32_t i = k->n; i-- > 1;)
        if (!k->t[i].deleted && style_word(tok_view(k, i))) {
            k->t[i].deleted = 1;
            return;
        }
}

/* ExtractFontStretch 0x180023570: windows of up to two live tokens ending at i (n - 1 down to 1): the
 * word last, a qualifier inside it or as the token before; the best score (first found on ties) */
static void extract_stretch(tokens *k) {
    uint32_t best = 0, best_start = 0, best_count = 0;
    if (k->n < 2) return;
    for (uint32_t i = k->n - 1; i >= 1; i--) {
        if (!k->t[i].deleted) {
            uint32_t w = i == 1 || k->t[i - 1].deleted ? 1 : 2;
            view a[2];
            for (uint32_t j = 0; j < w; j++) a[j] = tok_view(k, i - w + 1 + j);
            uint32_t lv = w - 1;
            view v = a[lv];
            int q = 0, from_prev = 0, name = stretch_name(v);
            int ok = 1;
            if (name == 6) {
                int pq;
                uint32_t pre = qualifier_prefix(v, &pq);
                v.s += pre, v.n -= pre;
                q = pq;
                ok = pq != 0 && (name = stretch_name(v)) != 6;
            }
            if (ok) {
                if (q == 0 && lv != 0) {
                    int pq;
                    if (qualifier_prefix(a[lv - 1], &pq) == a[lv - 1].n) {
                        q = pq;
                        if (pq) lv = w - 2, from_prev = 1;
                    }
                }
                uint32_t val = STRETCH[name][q];
                if (val == 0 && from_prev) val = STRETCH[name][0], lv++, q = 0;
                uint32_t count = val ? w - lv : 0;
                uint32_t score = STRETCH_SCORE[name][q];
                if (best < score) best = score, best_count = count, best_start = i - count + 1;
            }
        }
        if (i == 1) break;
    }
    if (best) mark(k, best_start, best_count);
}

/* ExtractFontWeight 0x180023974 from token `first` on: windows of up to three live tokens ending at i
 * (n - 1 down to first + 1 ... first); the last token loses a "face" suffix (0x1801c1640), a token that is
 * only "face" gives the word to the token before; a qualifier inside the word or as the token before it.
 * The best score (first found on ties); its tokens are deleted. 0 for none. */
static int extract_weight(tokens *k, uint32_t first) {
    uint32_t best = 0, best_weight = 0, best_start = 0, best_count = 0;
    if (k->n <= first) return 0;
    for (uint32_t i = k->n - 1;; i--) {
        if (!k->t[i].deleted) {
            uint32_t w = i == first || k->t[i - 1].deleted ? 1 : i == first + 1 || k->t[i - 2].deleted ? 2 : 3;
            view a[3];
            for (uint32_t j = 0; j < w; j++) a[j] = tok_view(k, i - w + 1 + j);
            uint32_t lv = w - 1;
            view v = a[lv];
            if (v.n >= 4 && prefix_is(v.s + v.n - 4, "face")) {
                v.n -= 4;
                if (v.n == 0 && lv != 0) v = a[--lv];
            }
            int q = 0, from_prev = 0, name = weight_name(v), ok = 1;
            if (name == 8) {
                int pq;
                uint32_t pre = qualifier_prefix(v, &pq);
                v.s += pre, v.n -= pre;
                q = pq;
                ok = pq != 0 && (name = weight_name(v)) != 8;
            }
            if (ok) {
                if (q == 0 && lv != 0) {
                    int pq;
                    if (qualifier_prefix(a[lv - 1], &pq) == a[lv - 1].n) {
                        q = pq;
                        if (pq) lv--, from_prev = 1;
                    } else {
                        q = 0;
                    }
                }
                uint32_t val = WEIGHT[name][q];
                if (val == 0 && from_prev) val = WEIGHT[name][0], lv++, q = 0;
                uint32_t count = val ? w - lv : 0;
                uint32_t score = WEIGHT_SCORE[name][q];
                if (best < score) best = score, best_weight = val, best_count = count, best_start = i - count + 1;
            }
        }
        if (i <= first) break;
    }
    if (!best) return 0;
    mark(k, best_start, best_count);
    return (int)best_weight;
}

/* ParseAbbreviatedFontWeightName 0x1800df488 */
static int abbreviated_weight(view v) {
    if (v.n == 1) {
        switch (v.s[0] | 0x20) {
        case 'b': return 700;
        case 'h': return 900;
        case 'l': return 300;
        case 'm': return 500;
        case 'r': return 400;
        case 'u': return 800;
        }
        return 0;
    }
    if (v.n == 2) {
        uint32_t c = ((uint32_t)v.s[0] << 16 | v.s[1]) | 0x200020;
        if (c == 0x650062) return 800;   /* eb */
        if (c == 0x65006c) return 200;   /* el */
        if (c == 0x730062) return 600;   /* sb */
        if (c == 0x750068) return 950;   /* uh */
    }
    if (v.n < 2 || (v.s[0] | 0x20) != 'w') return 0;
    uint32_t x = 0;
    for (uint32_t i = 1; i < v.n && x <= 999; i++) {
        if ((uint16_t)(v.s[i] - '0') > 9) return 0;
        x = 10 * x + (uint32_t)(v.s[i] - '0');
    }
    return x < 10 ? (int)x * 100 : x <= 999 ? (int)x : 0;
}

/* ExtractAbbreviatedFontWeight 0x1800df35c: from the last token down to token 1, the first live one with
 * an abbreviation within 100 of the OS/2 weight */
static int extract_abbreviated(tokens *k, int os2) {
    if (k->n < 2) return 0;
    for (uint32_t i = k->n - 1; i >= 1; i--) {
        if (!k->t[i].deleted) {
            int w = abbreviated_weight(tok_view(k, i));
            if (w && (w > os2 ? w - os2 : os2 - w) < 101) {
                k->t[i].deleted = 1;
                return w;
            }
        }
    }
    return 0;
}

/* GetResolvedWeight 0x1800dcbc8 */
static int resolved_weight(int os2, int name) {
    if (os2 == name || name == 0) return os2;
    if (os2 < 400 && name < 400) return os2;
    if (os2 > 500) {
        if (name < 500) goto diff;
        if (os2 != 700) return os2;
    }
    if ((os2 == 400 || os2 == 500) && (name == 500 || name == 400)) return os2;
diff:;
    unsigned d = (unsigned)(name > os2 ? name - os2 : os2 - name);
    if (os2 != 400 && d < 0x97 && os2 != 500) return os2 == 700 ? name : os2;
    return name;
}

/* ---------------------------------------------------------------------------------------------- */

/* GetFontWeightFromFace 0x180021fc4: OS/2 usWeightClass (0 -> 1, above 999 -> 999, below 10 x 100,
 * at most 500 with fsSelection BOLD -> 700); no OS/2: head macStyle bold -> 700, else 400 */
int idwr_font_os2_weight(const idwr_font *f) {
    uint32_t len;
    const uint8_t *os2 = idwr_font_table(f, TAG('O', 'S', '/', '2'), &len);
    if (os2 == NULL || len < 64) {
        if (f->head && f->head_len >= 46 && (f->head[45] & 1)) return 700;   /* macStyle bold */
        return 400;
    }
    uint32_t w = idwr_u16(os2 + 4);
    if (w == 0) {
        w = 1;
    } else {
        if (w > 999) return 999;
        if (w < 10) w *= 100;
        if (w > 500) return (int)w;
    }
    if (os2[0x3f] & 0x20) w = 700;   /* fsSelection BOLD */
    return (int)w;
}

/* the STAT entry of OpenTypeFace (+ 0x1b4 offset, + 0x1b8 length) both non-zero */
int idwr_font_has_stat(const idwr_font *f) {
    uint32_t len;
    const uint8_t *p = idwr_font_table(f, TAG('S', 'T', 'A', 'T'), &len);
    return p != NULL && p != f->data && len != 0;
}

/* FontDifferentiator::FontDifferentiator 0x180021114, its + 0xb2; -1 where DWrite throws */
int idwr_font_name_weight(const idwr_font *f) {
    int os2 = idwr_font_os2_weight(f);
    name_table t;
    if (name_open(f, &t) < 0) return -1;
    int l16 = name_listed(&t, 16), l17 = name_listed(&t, 17), l21 = name_listed(&t, 21), l22 = name_listed(&t, 22);
    int typographic = l16 || l17, wws = l21 && l22;
    wstr fam = {0}, sub = {0}, n16 = {0}, n17 = {0}, ft = {0}, st = {0}, sr = {0}, full = {0};
    tokens k = {0};
    int result = os2;
    uint16_t lcid = canonical_names(&t, &fam, &sub);
    uint32_t os2_len;
    const uint8_t *os2t = idwr_font_table(f, TAG('O', 'S', '/', '2'), &os2_len);
    int wws_bit = os2t != NULL && os2_len >= 64 && (os2t[0x3e] & 1);   /* fsSelection bit 8 */
    if ((typographic && wws) || lcid == 0 || idwr_font_has_stat(f) || wws_bit) goto done;
    int got16 = name_in(&t, 16, lcid, &n16), got17 = name_in(&t, 17, lcid, &n17);
    trim(got16 ? &n16 : &fam, &ft);
    trim(got17 ? &n17 : &sub, &st);
    remove_regular(&st, &sr);
    combine(&ft, &sr, &full);
    tokenize(&full, &k);
    if (k.bad || full.bad || fam.bad || sub.bad || n16.bad || n17.bad || ft.bad || st.bad || sr.bad) {
        result = -1;
        goto done;
    }
    delete_style(&k);
    extract_stretch(&k);
    int w = extract_weight(&k, 1);
    if (w == 0 && got16 && got17 && ws_equal(&full, &fam)) w = extract_abbreviated(&k, os2);
    if (!wws) result = resolved_weight(os2, w);
done:
    free(k.t);
    ws_free(&fam), ws_free(&sub), ws_free(&n16), ws_free(&n17), ws_free(&ft), ws_free(&st), ws_free(&sr);
    ws_free(&full);
    return result;
}

/* FontFaceLight::GetFontWeight for a face without simulations, without STAT (and with STAT but no wght
 * axis): the differentiator's weight. The OS/2 rule where DWrite would throw. */
int idwr_font_weight(const idwr_font *f) {
#ifdef IDWR_NEGATIVE_CONTROL_NAMEWEIGHT
    return idwr_font_os2_weight(f);   /* the earlier model: the names not read */
#endif
    int w = idwr_font_name_weight(f);
    return w < 0 ? idwr_font_os2_weight(f) : w;
}
