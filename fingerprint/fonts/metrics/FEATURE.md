# fingerprint/fonts/metrics

Category: spoof
Prefs: zoom.stealth.text.system_quality
Test: TODO

## What it does

TODO: one paragraph, from the notes below.

## Hooks in Mozilla files

- `gfx/skia/skia/src/core/SkScalerContext.cpp` (SkScalerContext::getImage, already-hook): This file is in the vendored Skia tree, so every Skia update conflicts with this block. The call sits before the mask filter on purpose: a blur must act on the already quantised mask, as the comment explains. The include is the only Mozilla header in this file (other Skia files do include mozilla/ headers, e.g. gfx/skia/skia/src/ports/SkMemory_mozalloc.cpp:12). The comment says this replaces the r
- `gfx/thebes/gfxDWriteFonts.cpp` (GetSystemTextQuality; GetSystemGDIGamma (comment only); gfxDWriteFont::UpdateClearTypeVars, movable): With the engine on, GetSystemTextQuality returns the declared zoom.stealth.text.system_quality (only 0, 4 or 5 accepted; anything else calls StealthRefuse) and never asks SystemParametersInfo. UpdateClearTypeVars skips CreateRenderingParams (stealthDeclared forces E_FAIL) and always applies the declared ClearType values. The six hard-coded defaults remain as the stock Firefox baseline. GDI gamma i
- `gfx/thebes/gfxFT2FontBase.cpp` (gfxFT2FontBase::GetFTGlyphExtents; gfxFT2FontBase::GetGlyphBounds, movable): With the engine on, the advance and the ink box come from HarfBuzz design-space values (GetDesignAdvance and GetDesignGlyphExtents) scaled by the unrounded size, not from FreeType's hinted values. No fallback to FreeType on this path (engine rule 7 in comments). The declaredAdvance flag skips the hinting round and the extentsScale multiply. Comments include the measured Courier New case at 13.3833
- `gfx/thebes/gfxFont.cpp` (gfxFont::GetStealthHostIndepVMetrics (new, placed before InitMetricsFromSfntTables); GetStealthBundleVMetrics (new static); gfxFont::SanitizeMetrics, movable): SanitizeMetrics is the shared point where DirectWrite, FreeType and CoreText finish their metrics. For a bundled face, the manifest's vertical metrics overwrite maxAscent/maxDescent/emHeight/emAscent/emDescent/internalLeading/externalLeading and xHeight/capHeight/underline/strikeout when the manifest has them, then the existing clamps run. GetStealthHostIndepVMetrics reads OS/2 directly for canvas
- `gfx/thebes/gfxFont.h` (gfxFont class, public: GetStealthHostIndepVMetrics declaration (next to the mVerticalMetrics accessor), already-hook): Declaration only; definition is in gfxFont.cpp. The comment says it exists to make canvas TextMetrics vertical fields host-independent.
- `gfx/thebes/gfxHarfBuzzShaper.cpp` (gfxHarfBuzzShaper::~gfxHarfBuzzShaper; gfxHarfBuzzShaper::GetDesignFont (new); gfxHarfBuzzShaper::GetDesignGlyphExtents (new); gfxHarfBuzzShaper::GetDesignAdvance (new); gfxHarfBuzzShaper::ShapeText (comment only), movable): GetDesignFont builds a second hb_font on the same face, used only for design-space metrics and never for shaping: scale = upem, hb_ot_font_set_funcs pinned, variations applied. It holds mMutex (recursive) because the shaper is shared through gfxFontCache. Primary issue: the face is bound through a named AutoHBFace; the comment says an earlier unnamed binding was a dangling pointer (feature hb-face
- `gfx/thebes/gfxHarfBuzzShaper.h` (gfxHarfBuzzShaper class: public GetDesignGlyphExtents and GetDesignAdvance declarations; private mDesignFont member and GetDesignFont declaration, already-hook): Declarations and a mutable cache member for the design font. The header comment gives the measurement that motivates the change (Windows 12960/12960 matched, Linux 1440 differing). One stray blank line is added, no functional change.

## Our own files

- `gfx/2d/StealthGlyphCoverage.cpp`
- `gfx/2d/StealthGlyphCoverage.h`
- `gfx/tests/gtest/TestStealthTextQuality.cpp`
