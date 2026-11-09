/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// See StealthSyntheticBold.h.

#include "StealthSyntheticBold.h"

#include "StealthDeclarationGate.h"
#include "gfxFont.h"
#include "gfxFontEntry.h"
#include "gfxFontUtils.h"
#include "mozilla/gfx/StealthMultistrikeBold.h"

namespace mozilla {
namespace gfx {

bool StealthMultistrikeBoldRule(bool aEngineOn, bool aNeedsSyntheticBold,
                                bool aIsWebfont, bool aHasColr) {
  // gfx.font_rendering.directwrite.bold_simulation == 1, the retail default:
  // "use DWrite simulation for installed fonts except COLR fonts, but not
  // webfonts". The pref exists only on Windows; the declared behaviour is the
  // retail one, so it is a constant here and not a second declaration.
  return aEngineOn && aNeedsSyntheticBold && (aIsWebfont || aHasColr);
}

bool StealthLinuxApplySyntheticBold(gfxFontEntry* aFontEntry,
                                    const gfxFontStyle* aStyle) {
  if (!StealthEngineActive() || !aFontEntry || !aStyle ||
      !aStyle->NeedsSyntheticBold(aFontEntry)) {
    return false;
  }
  // A real @font-face: data the page brought. A face of the bundle is built
  // from data too (mIsDataUserFont) but belongs to the shared list (mShmemFace,
  // which gfxUserFontSet clears on every downloaded font): it stands for an
  // installed font, which Windows tells apart by the entry class.
  const bool isWebfont = aFontEntry->mIsDataUserFont && !aFontEntry->mShmemFace;
  const bool hasColr =
      aFontEntry->HasFontTable(TRUETYPE_TAG('C', 'O', 'L', 'R'));
  return StealthMultistrikeBoldRule(true, true, isWebfont, hasColr);
}

void StealthDeclareMultistrikeBold(FcPattern* aPattern, bool aMultistrike) {
  if (!aMultistrike) {
    return;
  }
  FcPatternDel(aPattern, FC_EMBOLDEN);
  FcPatternDel(aPattern, kStealthMultistrikeBoldProperty);
  FcPatternAddBool(aPattern, kStealthMultistrikeBoldProperty, FcTrue);
}

void StealthDeclareSyntheticBold(FcPattern* aPattern, gfxFontEntry* aFontEntry,
                                 const gfxFontStyle* aStyle) {
  StealthDeclareMultistrikeBold(
      aPattern, StealthLinuxApplySyntheticBold(aFontEntry, aStyle));
}

void StealthSyntheticBoldMetrics(gfxFont& aFont, gfxFont::Metrics& aMetrics) {
  if (!aFont.ApplySyntheticBold()) {
    return;
  }
  const gfxFloat delta = aFont.GetSyntheticBoldOffset();
  aMetrics.spaceWidth += delta;
  aMetrics.aveCharWidth += delta;
  aMetrics.maxAdvance += delta;
  if (aMetrics.zeroWidth > 0) {
    aMetrics.zeroWidth += delta;
  }
  if (aMetrics.ideographicWidth > 0) {
    aMetrics.ideographicWidth += delta;
  }
}

}  // namespace gfx
}  // namespace mozilla
