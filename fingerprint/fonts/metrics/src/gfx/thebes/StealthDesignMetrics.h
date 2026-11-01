/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef GFX_STEALTHDESIGNMETRICS_H
#define GFX_STEALTHDESIGNMETRICS_H

// Design-space glyph advance, ink box and average character width for the
// FreeType backend. Included only by gfxFT2FontBase.cpp (Linux and Android).

#include <stddef.h>
#include <stdint.h>

#include "StealthDeclarationGate.h"
#include "StealthTextMeasuring.h"
#include "gfxFT2FontBase.h"
#include "gfxFont.h"
#include "gfxFontEntry.h"
#include "gfxFontUtils.h"
#include "gfxHarfBuzzShaper.h"
#include "gfxRect.h"
#include "harfbuzz/hb.h"
#include "mozilla/Assertions.h"
#include "mozilla/StaticPrefs_zoom.h"
#include "nsMathUtils.h"

namespace mozilla {
namespace gfx {

/**
 * The advance of glyph aGID the way DirectWrite scales it: the DESIGN advance
 * times the size the font was asked for, not the size FreeType rounded it to.
 *
 * Called from gfxFT2FontBase::GetFTGlyphExtents, before the FreeType advance
 * is read. Returns true when it has written the final value to *aAdvance (the
 * caller must then skip its own computation, including the hint rounding and
 * the extentsScale multiply, because this value is already at the desired
 * size). Returns false, writing nothing, when the engine is off or the design
 * advance cannot be read; the caller then runs stock code.
 *
 * aBoldX is the synthetic-bold strength in 26.6 (bold.x in the caller); it is
 * added after the scaling exactly as the stock FreeType path adds it.
 *
 * Why. FreeType scales the design advance by an INTEGER ppem.
 * FT_Request_Metrics computes x_ppem = (scaled + 32) >> 6 and then derives
 * x_scale from that already-rounded value, so linearHoriAdvance carries the
 * rounding even though ComputeMetrics prefers x_scale precisely because
 * "x_ppem does not have subpixel accuracy". DirectWrite scales by the size it
 * was handed. Whole sizes therefore agree by construction and fractional ones
 * cannot, which is the entire divergence measured on 2026-08-09: 112 of 3120
 * (family, size, string) combinations, every one of them at a fractional size.
 *
 * Courier New is the worked example. Its glyphs are 1229 units on a 2048 em,
 * and at a requested 13.383333px Windows reports 8.0313px per glyph while the
 * stock FreeType path reports 7.8022 - which is 1229 * 13 / 2048, the right
 * arithmetic on the wrong ppem. Scaling the DESIGN advance by the unrounded
 * size is what gfxDWriteFont::MeasureGlyphWidth does on its design path, so
 * doing the same here makes the two agree by construction rather than by luck.
 *
 * The declared path is total in practice, and that is deliberate (engine
 * rule 7). The first version asked FreeType for the design advance and fell
 * through to the rasterised one whenever the face was not scalable or the call
 * failed - the same hole the ink box had, and the ink box's version of it was
 * measured leaking on every CFF webfont. GetDesignAdvance asks HarfBuzz's own
 * OpenType implementation, which reads glyf, CFF and CFF2 alike and needs no
 * FreeType face at all, so a CFF or a bitmap-strike face is served by the same
 * code as a bundled TrueType one. The false return is therefore not a second
 * path to rely on: it covers only a missing shaper, a font with no usable
 * unitsPerEm, or an unreadable design advance, none of which is expected for a
 * font the engine can shape.
 */
inline bool StealthDesignAdvance(gfxFT2FontBase& aFont, uint16_t aGID,
                                 FT_Pos aBoldX, int32_t* aAdvance) {
  MOZ_ASSERT(aAdvance);
  if (!StealthEngineActive() || !(aFont.GetAdjustedSize() > 0.0)) {
    return false;
  }
  gfxHarfBuzzShaper* shaper = aFont.GetHarfBuzzShaper();
  const uint16_t upem = aFont.GetFontEntry()->UnitsPerEm();
  int32_t designAdvance = 0;
  if (!shaper || !upem || !shaper->GetDesignAdvance(aGID, designAdvance)) {
    return false;
  }
  // The design advance arrives in FONT UNITS, so this is the only place the
  // unrounded size enters, and 65536 puts it in the 16.16 the caller speaks.
  FT_Fixed advance = NS_lround(double(designAdvance) * aFont.GetAdjustedSize() *
                               65536.0 / double(upem));
  if (advance) {
    advance += aBoldX << 10;  // convert 26.6 to 16.16
  }
  // Already at the desired size, so it must not go through GetAdjustedSize() /
  // mFTSize a second time.
  *aAdvance = NS_lround(advance);
  return true;
}

/**
 * The ink box of glyph aGID the way DirectWrite reports it: the font's own
 * bounding box scaled by size / upem, with no hinting and no rasteriser.
 *
 * Called from gfxFT2FontBase::GetGlyphBounds. Returns true when *aBounds has
 * been written (the caller returns it as is); returns false, writing nothing,
 * when the engine is off or the design extents cannot be read, and the caller
 * then runs stock code.
 *
 * Why. gfxDWriteFont::GetGlyphBounds calls GetDesignGlyphMetrics, which is the
 * glyf table's own bounding box scaled by size/upem and nothing else. The
 * stock FreeType path asks for HINTED bounds instead, and the two disagree by
 * up to a pixel. That reaches the page directly: measureText's four
 * actualBoundingBox fields are built from this, and measured 2026-08-09
 * against stock Firefox 151 over the whole finite domain of textBaseline x
 * textAlign x direction, Windows matched on 12960 of 12960 fields while Linux
 * differed on 1440.
 *
 * The arithmetic mirrors gfxDWriteFonts.cpp:781-784 deliberately, including the
 * float (not double) conversion factor, so Windows keeps the numbers it already
 * had and Linux is brought TO them. DWrite's Inflate(1.0, 0.0) is NOT mirrored:
 * it is guarded by !aBounds->IsEmpty() on the uninitialised out-parameter,
 * which gfxFont::SetupGlyphExtents default-constructs to an empty rect, so on
 * Windows that branch never runs.
 *
 * The declared path is total in practice, and that is the point (engine
 * rule 7). This used to read the glyf table directly and return false for a
 * font that has none, letting the caller fall through to FreeType. Every CFF
 * webfont a page loads is exactly that case, and it was a live tell rather than
 * a corner: measured 2026-08-09 on a purpose-built CFF face, 108 of 150
 * measureText fields diverged between Windows and Linux, with Linux handing
 * back whole integers because FreeType had grid-fitted them.
 * GetDesignGlyphExtents asks HarfBuzz's own OpenType implementation, which
 * reads glyf, CFF and CFF2 alike, so no ordinary font reaches the false return.
 * It remains only for a missing shaper or a font with no usable unitsPerEm at
 * all, where the number computed here would be meaningless too.
 *
 * The ORDER of operations is copied from gfxDWriteFont::GetGlyphBounds, not
 * just the formula, and it has to be. DWrite builds the rect in font UNITS and
 * calls gfxRect::Scale afterwards, so every product is a double times a float.
 * Multiplying each component as int-times-float instead does the arithmetic in
 * float precision and only then widens, which is close but not equal: measured,
 * that alone left 22 of 150 fields differing in the eighth significant digit
 * after the grid-fitting leak was already closed.
 *
 * HarfBuzz's y_bearing is the top extremum measured upward and its height is
 * negative in a y-up system (hb-common.h says so), which is why both are
 * negated here to reach gfxRect's y-down box.
 */
inline bool StealthDesignGlyphBounds(gfxFT2FontBase& aFont, uint16_t aGID,
                                     gfxRect* aBounds) {
  MOZ_ASSERT(aBounds);
  if (!StealthEngineActive()) {
    return false;
  }
  gfxHarfBuzzShaper* shaper = aFont.GetHarfBuzzShaper();
  const uint16_t upem = aFont.GetFontEntry()->UnitsPerEm();
  int32_t xb, yb, w, h;
  if (!shaper || !upem || !shaper->GetDesignGlyphExtents(aGID, xb, yb, w, h)) {
    return false;
  }
  const float conv = float(aFont.GetAdjustedSize() / gfxFloat(upem));
  gfxRect bounds(double(xb), double(-yb), double(w), double(-h));
  bounds.Scale(conv);
  *aBounds = bounds;
  return true;
}

/**
 * Would gfxDWriteFont::ComputeMetrics measure this font in design space
 * (mUseSubpixelPositions)? Read from the same declaration Windows reads, never
 * a second one.
 *
 * Windows: (anAAOption == kAntialiasDefault && UsingClearType() &&
 * GetMeasuringMode() == DWRITE_MEASURING_MODE_NATURAL). A FreeType font is
 * always built with kAntialiasDefault (gfxFT2FontBase's constructor).
 * - UsingClearType() is gfxVars::SystemTextQuality() == CLEARTYPE_QUALITY (5),
 *   and with the engine on that var is zoom.stealth.text.system_quality
 *   (StealthDeclaredTextQuality, StealthTextQuality.h).
 * - The measuring mode comes from the rendering mode in DWriteSettings'
 *   UpdateRenderingMode: ALIASED (1) and CLEARTYPE_GDI_CLASSIC (2) measure GDI
 *   classic, CLEARTYPE_GDI_NATURAL (3) GDI natural, every other mode natural.
 *   UpdateClearTypeVars takes the mode from
 *   gfx.font_rendering.cleartype_params.rendering_mode when it is 0-5 and
 *   otherwise leaves DWRITE_RENDERING_MODE_DEFAULT (0); with the engine on the
 *   system's own default is never asked (StealthCreateRenderingParams).
 * - GetForceGDIClassic() can force GDI classic per family, but only for the
 *   families in gfx.font_rendering.cleartype_params.force_gdi_classic_for_families,
 *   which Firefox ships empty ("We no longer force GDI Classic mode on any
 *   fonts by default", all.js), and never while a rendering mode is declared.
 */
inline bool StealthDWriteSubpixelPositions() {
  constexpr int32_t kClearTypeQuality = 5;  // CLEARTYPE_QUALITY
  if (StaticPrefs::zoom_stealth_text_system_quality() != kClearTypeQuality) {
    return false;
  }
  // Any thread: InitMetrics runs on workers too, so the pref is read through
  // its mirror (StealthTextMeasuring.h), never Preferences.
  int32_t mode = StealthDeclaredRenderingMode();
  if (mode < 0 || mode > 5) {
    mode = 0;  // DWRITE_RENDERING_MODE_DEFAULT
  }
  return mode != 1 && mode != 2 && mode != 3;
}

/**
 * GetCharAdvance(aChar) as gfxDWriteFont answers it, from the design advance;
 * -1 when the font has no glyph for it (gfxFont::GetCharAdvance's answer).
 * - Design space (aSubpixel): gfxHarfBuzzShaper reads hmtx and returns
 *   FloatToFixed(mFUnitsConvFactor * advance), a float product truncated to
 *   16.16.
 * - Otherwise: MeasureGlyphWidth's GDI-compatible branch, NS_lround(advance *
 *   mFUnitsConvFactor). Windows asks DirectWrite for the GDI-compatible advance
 *   there; the design advance stands for it, as everywhere else on this path.
 */
inline gfxFloat StealthDWriteCharAdvance(gfxFT2FontBase& aFont,
                                         uint32_t aChar, float aConv,
                                         bool aSubpixel) {
  gfxHarfBuzzShaper* shaper = aFont.GetHarfBuzzShaper();
  const hb_codepoint_t gid = shaper ? shaper->GetNominalGlyph(aChar) : 0;
  int32_t design = 0;
  if (!gid || !shaper->GetDesignAdvance(uint16_t(gid), design)) {
    return -1.0;
  }
  const float scaled = design * aConv;
  if (aSubpixel) {
    return int32_t(65536 * scaled) / 65536.0;
  }
  return gfxFloat(NS_lround(scaled));
}

/**
 * The average character width the way gfxDWriteFont::ComputeMetrics sets it.
 *
 * Called from gfxFT2FontBase::InitExtraMetrics after Mozilla's own value is
 * final and before maxAdvance is taken from it. With the engine on it replaces
 * that value; with the engine off it does nothing.
 *
 * Why. FreeType rounds the OS/2 xAvgCharWidth to whole pixels at an integer
 * ppem (ScaleRoundDesignUnits) and then raises it to the '0' advance. Windows
 * does neither. That reaches the page through every element sized in
 * characters: nsTextControlFrame::CalcFixedSize takes max(ZeroOrAve, Ave), and
 * nsFontMetrics::AveCharWidth ceils this value to app units. Consolas (1126 on
 * a 2048 em) at 13px and DPR 1.5 is 19.5 device px: the host gate measured
 * the default textarea at 143px on Windows and 146.667px on Linux, which is
 * an average of 10.7212 against 11.
 *
 * The steps, in DirectWrite's order (gfxDWriteFonts.cpp, ComputeMetrics):
 * 1. In design space (StealthDWriteSubpixelPositions), int16(xAvgCharWidth)
 *    times the float factor float(size / upem): one IEEE float multiply, so
 *    x86-64 and arm64 give the same bits.
 * 2. Below 1 (no OS/2, or not in design space): the 'x' advance
 *    (StealthDWriteCharAdvance).
 * 3. Still below 1: the font's x-height times the factor. DirectWrite's
 *    x-height is the OS/2 sxHeight (version 2 and later); for an older table
 *    this takes the top of the 'x' outline, and with neither it is 0, so the
 *    width is 0 as on Windows. Step 3 is read from the code, not measured.
 *
 * Not mirrored: the CJK bitmap-strike case, where Windows rounds the size and
 * leaves design space (gfxDWriteFontEntry::HasBitmapStrikeForSize).
 */
inline void StealthDWriteAveCharWidth(gfxFT2FontBase& aFont,
                                      gfxFont::Metrics& aMetrics) {
  if (!StealthEngineActive() || !(aFont.GetAdjustedSize() > 0.0)) {
    return;
  }
  gfxFontEntry* fe = aFont.GetFontEntry();
  const uint16_t upem = fe->UnitsPerEm();
  if (!upem || upem == gfxFontEntry::kInvalidUPEM) {
    return;
  }
  // mFUnitsConvFactor = float(mAdjustedSize / fontMetrics.designUnitsPerEm).
  const float conv = float(aFont.GetAdjustedSize() / upem);
  const bool subpixel = StealthDWriteSubpixelPositions();

  gfxFontEntry::AutoTable os2Table(fe, TRUETYPE_TAG('O', 'S', '/', '2'));
  uint32_t os2Len = 0;
  const OS2Table* os2 =
      os2Table ? reinterpret_cast<const OS2Table*>(
                     hb_blob_get_data(os2Table, &os2Len))
               : nullptr;

  gfxFloat ave = 0.0;
  if (subpixel && os2 && os2Len >= 4) {
    ave = int16_t(os2->xAvgCharWidth) * conv;
  }
  if (ave < 1) {
    ave = StealthDWriteCharAdvance(aFont, 'x', conv, subpixel);
  }
  if (ave < 1) {
    int32_t xHeight = 0;
    if (os2 && os2Len >= offsetof(OS2Table, sxHeight) + sizeof(int16_t) &&
        uint16_t(os2->version) >= 2) {
      xHeight = int16_t(os2->sxHeight);
    }
    gfxHarfBuzzShaper* shaper = aFont.GetHarfBuzzShaper();
    const hb_codepoint_t gid = shaper ? shaper->GetNominalGlyph('x') : 0;
    int32_t xb = 0, yb = 0, w = 0, h = 0;
    if (xHeight <= 0 && gid &&
        shaper->GetDesignGlyphExtents(uint16_t(gid), xb, yb, w, h)) {
      xHeight = yb;
    }
    ave = xHeight > 0 ? gfxFloat(uint16_t(xHeight) * conv) : 0.0;
  }
  aMetrics.aveCharWidth = ave;
}

}  // namespace gfx
}  // namespace mozilla

#endif  // GFX_STEALTHDESIGNMETRICS_H
