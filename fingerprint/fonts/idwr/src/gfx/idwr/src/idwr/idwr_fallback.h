/*
 * idwr font fallback: DirectWrite 10.0.26100.9711's system font fallback (IDWriteFontFallback,
 * the FONTFALLBACK table of DWrite.dll) over a font set given as data. notes/fallback.md has
 * the model, its provenance and its checks (reimpl/fallback_check.py).
 *
 * The font set stands in for the collection DirectWrite would see: families in collection
 * order, each with its localized names, each font with the weight, stretch and style DirectWrite
 * gives it. Character coverage is computed from the font's own cmap, as DirectWrite's font cache
 * computes it (IDWriteFont::HasCharacter). Nothing is read from the OS.
 *
 * Included by idwr.h.
 */
#ifndef IDWR_FALLBACK_H
#define IDWR_FALLBACK_H
#include <stdint.h>

typedef struct idwr_font_set idwr_font_set;

IDWR_API idwr_font_set *idwr_font_set_create(void);
IDWR_API void idwr_font_set_destroy(idwr_font_set *set);

/* Adds a family; returns its index (families are numbered in the order they are added, which
 * must be the collection's family order: FindFamilyName returns the first family that has a
 * name). `names` are UTF-8, NUL-terminated: every localized family name, the en-us one first.
 * -1 on allocation failure. */
IDWR_API int32_t idwr_font_set_add_family(idwr_font_set *set, const char *const *names, uint32_t name_count);

/* Adds a font to a family; returns its index in the set (fonts are numbered in the order they are
 * added; a family's fonts must be added in the collection's font set order). weight, stretch and
 * style are the DWRITE_FONT_WEIGHT / _STRETCH / _STYLE values of the font (not simulated: the
 * model derives bold and oblique simulations itself). `instance_filter` is the high bit of the
 * font's FontFaceKey: GetMatchingFonts then drops the font from a match unless it has the best
 * match's stretch and weight/style, or is that stretch's regular. DirectWrite does not set it on
 * local font files (the system collection or a collection built from files, named instances of
 * variable fonts included): pass 0 for a persona's fonts. The font is read only during the call
 * (its cmap, maxp and OS/2). Returns -1 on bad arguments, -2 when the font's cmap is one DirectWrite reads through a
 * legacy code page (Shift-JIS, PRC, Big5, Wansung subtables, or an OS/2 Hebrew/Arabic charset),
 * which idwr does not reproduce, -3 when DirectWrite would reject the cmap (FileFormatException). */
IDWR_API int32_t idwr_font_set_add_font(idwr_font_set *set, int32_t family, const idwr_font *font, uint16_t weight,
                                        uint8_t stretch, uint8_t style, int instance_filter);

/* IDWriteFont::HasCharacter of a font of the set */
IDWR_API int idwr_font_set_has_character(const idwr_font_set *set, int32_t font, uint32_t codepoint);
/* IDWriteFontCollection::FindFamilyName (UTF-8): the family index, or -1 */
IDWR_API int32_t idwr_font_set_find_family(const idwr_font_set *set, const char *name);
/* IDWriteFontFamily::GetMatchingFonts(weight, stretch, style): writes up to `cap` font indices
 * and their simulations (DWRITE_FONT_SIMULATIONS) in DirectWrite's order; returns the count. */
IDWR_API uint32_t idwr_font_set_matching_fonts(const idwr_font_set *set, int32_t family, uint16_t weight,
                                               uint8_t stretch, uint8_t style, int32_t *fonts,
                                               uint32_t *simulations, uint32_t cap);

typedef struct {
    uint32_t mapped_length;  /* UTF-16 code units, as IDWriteFontFallback::MapCharacters' mappedLength */
    int32_t family;          /* family of the mapped font, -1 when no font was found */
    int32_t font;            /* font index in the set, -1 when no font was found */
    uint32_t simulations;    /* DWRITE_FONT_SIMULATIONS of the mapped font */
    float scale;             /* the mapping's scale factor */
} idwr_fallback_result;

/* IDWriteFontFallback::MapCharacters of the system fallback (IDWriteFactory2::GetSystemFontFallback)
 * whose collection is the set: `text` is the analysis source's text (UTF-16, from position 0),
 * `locale` its locale name or list (UTF-8; empty or NULL means the user default locale, which is
 * `default_locale`), `base_family` the base family name (UTF-8, may be NULL) looked up in the set,
 * which is also the base collection. Returns 0 (S_OK) or the HRESULT MapCharacters fails with
 * (E_INVALIDARG for a malformed locale). */
IDWR_API int32_t idwr_fallback_map_characters(const idwr_font_set *set, const uint16_t *text, uint32_t length,
                                              const char *locale, const char *default_locale,
                                              const char *base_family, uint16_t weight, uint8_t style,
                                              uint8_t stretch, idwr_fallback_result *out);

/* The family IDWriteTextLayout draws a single character in, with a text format of `base_family`,
 * weight, style, stretch and `locale` over the set, and the set's system fallback: the engine's
 * gfxDWriteFontList::PlatformGlobalFontFallback asks this with "Arial", regular, en-us, for one
 * character (a surrogate pair above U+FFFF). A DWRITE_SCRIPT_SHAPES_NO_VISUAL character, or one no
 * fallback font covers, is drawn in the base font. Returns the family index (the base family's when
 * the base font draws it), or -1 when the base family is not in the set and no fallback font maps
 * the character. `font` (may be NULL) receives the font index. */
IDWR_API int32_t idwr_fallback_layout_family(const idwr_font_set *set, uint32_t codepoint, const char *locale,
                                             const char *base_family, uint16_t weight, uint8_t style,
                                             uint8_t stretch, int32_t *font);

/* DWRITE_SCRIPT_SHAPES_NO_VISUAL for a one-character text (IDWriteTextAnalyzer::AnalyzeScript) */
IDWR_API int idwr_fallback_no_visual(uint32_t codepoint);

#endif
