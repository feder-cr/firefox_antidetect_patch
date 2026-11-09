/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef MOZILLA_GFX_STEALTHMULTISTRIKEBOLD_H_
#define MOZILLA_GFX_STEALTHMULTISTRIKEBOLD_H_

// Linux only (fontconfig). The one property that carries "this font instance
// is made bold by Gecko's own multi-strike, not by the rasteriser" from the
// place that decides it (gfx/thebes/StealthSyntheticBold.h, which writes it
// into the render pattern in place of FC_EMBOLDEN) to its two readers: the
// gfxFontconfigFont constructor (gfxFont::mApplySyntheticBold) and
// ScaledFontFontconfig::InstanceData (which then has no EMBOLDEN, so neither
// FreeType nor idwr's kSimulationBold is applied to the same face).

#include <fontconfig/fontconfig.h>

namespace mozilla::gfx {

constexpr char kStealthMultistrikeBoldProperty[] = "stealth.multistrike_bold";

inline bool StealthPatternMultistrikeBold(FcPattern* aPattern) {
  FcBool multistrike = FcFalse;
  return aPattern &&
         FcPatternGetBool(aPattern, kStealthMultistrikeBoldProperty, 0,
                          &multistrike) == FcResultMatch &&
         multistrike;
}

}  // namespace mozilla::gfx

#endif  // MOZILLA_GFX_STEALTHMULTISTRIKEBOLD_H_
