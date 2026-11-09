/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef MOZILLA_GFX_STEALTHIDWRTEXTVARS_H_
#define MOZILLA_GFX_STEALTHIDWRTEXTVARS_H_

#include <stdint.h>

namespace mozilla {
namespace gfx {

// The prefs the declaration comes in (-1: not set), and the values they make, by the
// rules of gfxDWriteFont::UpdateClearTypeVars under a declared state.
struct StealthIdwrTextPrefs {
  int32_t mQuality = -1;    // zoom.stealth.text.system_quality
  int32_t mLevel = -1;      // gfx.font_rendering.cleartype_params.cleartype_level
  int32_t mContrast = -1;   // ...enhanced_contrast
  int32_t mGamma = -1;      // ...gamma
  int32_t mStructure = -1;  // ...pixel_structure
  int32_t mMode = -1;       // ...rendering_mode
};
struct StealthIdwrTextVars {
  int32_t mQuality = 5;
  float mLevel = 1.0f;
  float mContrast = 1.0f;
  float mGamma = 2.2f;
  int32_t mPixelGeometry = 1;  // DWRITE_PIXEL_GEOMETRY_RGB
  int32_t mRenderingMode = 0;  // DWRITE_RENDERING_MODE_DEFAULT
  // Set when the declaration cannot be honoured: a quality that is not 0, 4 or 5 (as
  // StealthDeclaredTextQuality refuses on Windows), or no rendering mode (Windows forces
  // GDI classic for small text then, which is not reproduced). The caller refuses with it.
  const char* mRefusal = nullptr;
};
StealthIdwrTextVars StealthIdwrTextVarsFromPrefs(const StealthIdwrTextPrefs& aPrefs);

// Sets the gfxVars SystemTextQuality, SystemTextGamma, SystemTextEnhancedContrast,
// SystemTextClearTypeLevel, SystemTextPixelGeometry and SystemTextRenderingMode from
// the prefs, and the subpixel order from the declared pixel structure. Nothing happens
// with the engine off.
void StealthIdwrUpdateTextVars();

// Whether the declared pixel structure is BGR (DWRITE_PIXEL_GEOMETRY_BGR, 2), read from
// gfxVars; RGB for every other value and while gfxVars is not up. The one place the order
// comes from: the font list's cairo options and the 2D factory's subpixel order both ask it.
bool StealthIdwrPixelGeometryIsBGR();

// Factory::SetSubpixelOrder to the declared order. StealthIdwrUpdateTextVars does it after
// the gfxVars are set, and so does anything that would write the order afterwards.
void StealthIdwrApplyPixelGeometry();

}  // namespace gfx
}  // namespace mozilla

#endif  // MOZILLA_GFX_STEALTHIDWRTEXTVARS_H_
