/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef GFX_STEALTHSYNTHETICBOLD_H
#define GFX_STEALTHSYNTHETICBOLD_H

// Synthetic bold on the FreeType (Linux) backend, decided as gfxDWriteFont
// decides it on Windows retail with gfx.font_rendering.directwrite.bold_simulation
// at its default (1), gfxDWriteFonts.cpp, gfxDWriteFont::gfxDWriteFont:
//
//   webfont (a downloaded @font-face) or a font with a COLR table
//       -> Gecko's multi-strike: gfxFont::mApplySyntheticBold is true, the
//          glyphs are drawn again at one-device-pixel offsets
//          (gfxFont::DrawGlyphs), the advance of every cluster grows by
//          GetSyntheticBoldOffset() (gfxFont::PostShapingFixup) and the font
//          metrics grow by the same amount; the rasteriser's embolden is NOT
//          applied to that face.
//   an installed font (here: a face of the bundle, which is built from data
//   like a webfont but belongs to the shared font list)
//       -> the rasteriser's own simulation (DirectWrite on Windows, idwr's
//          kSimulationBold here), as before.
//
// Stock Linux never sets mApplySyntheticBold: it asks FC_EMBOLDEN for every
// face, which FreeType or idwr applies and which also widens the advances
// (gfxFT2FontBase::GetEmboldenStrength). With the engine on that made a
// webfont's bold another drawing and another width than on Windows: the 60
// bold cases of the canvas text e2e (group 0_base) differed, and measureText
// gave other widths.
//
// Where the decision is taken: once, in gfxFontconfigFontEntry::CreateFontInstance
// (StealthDeclareSyntheticBold, as StealthDeclareTextOptions declares the text
// options into the same pattern). The answer stays in the render pattern:
// FC_EMBOLDEN is removed (so neither FreeType nor idwr embolden, and
// gfxFT2FontBase adds no bold width to the advances Gecko already widens) and
// kStealthMultistrikeBoldProperty is set. The readers take it from there:
// gfxFontconfigFont's constructor sets mApplySyntheticBold before InitMetrics,
// and ScaledFontFontconfig::InstanceData records it (WebRender, which has no
// multi-strike on FreeType, is still told SYNTHETIC_BOLD for such a font).
// Engine off: nothing is written, stock.

#include <fontconfig/fontconfig.h>

#include "gfxFont.h"
#include "mozilla/gfx/StealthMultistrikeBold.h"

class gfxFontEntry;

namespace mozilla {
namespace gfx {

/**
 * The rule from its facts (so it can be tested without a font). aIsWebfont: a
 * downloaded @font-face (gfxFontEntry::mIsDataUserFont on Windows). aHasColr:
 * the font has a COLR table.
 */
bool StealthMultistrikeBoldRule(bool aEngineOn, bool aNeedsSyntheticBold,
                                bool aIsWebfont, bool aHasColr);

/** The rule for an entry and a style: the engine gate and the facts read from
 *  the entry. */
bool StealthLinuxApplySyntheticBold(gfxFontEntry* aFontEntry,
                                    const gfxFontStyle* aStyle);

/** Writes the answer into the render pattern (see above); does nothing when
 *  aMultistrike is false. */
void StealthDeclareMultistrikeBold(FcPattern* aPattern, bool aMultistrike);

/** StealthLinuxApplySyntheticBold, then StealthDeclareMultistrikeBold. */
void StealthDeclareSyntheticBold(FcPattern* aPattern, gfxFontEntry* aFontEntry,
                                 const gfxFontStyle* aStyle);

/**
 * What gfxDWriteFont::ComputeMetrics widens when ApplySyntheticBold():
 * spaceWidth, aveCharWidth and maxAdvance by GetSyntheticBoldOffset(), and
 * zeroWidth and ideographicWidth too when the font has the glyph. Called at the
 * end of gfxFT2FontBase::InitExtraMetrics; does nothing for a font that does
 * not apply the multi-strike.
 */
void StealthSyntheticBoldMetrics(gfxFont& aFont, gfxFont::Metrics& aMetrics);

}  // namespace gfx
}  // namespace mozilla

#endif  // GFX_STEALTHSYNTHETICBOLD_H
