/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// The Linux counterpart of gfxDWriteFont::UpdateSystemTextVars and UpdateClearTypeVars
// with the state declared, and of SystemTextQualityChanged: the declared text prefs
// into the gfxVars that Skia's idwr text is made of (gfx/2d/StealthSkiaIdwr*.cpp).

#include "StealthIdwrTextVars.h"

#include "gfxPlatform.h"
#include "mozilla/Preferences.h"
#include "mozilla/StaticPrefs_zoom.h"
#include "mozilla/gfx/2D.h"
#include "mozilla/gfx/StealthRefuse.h"
#include "mozilla/gfx/gfxVars.h"
#include "nsXULAppAPI.h"

namespace mozilla {
namespace gfx {

namespace {

constexpr int32_t kDefaultQuality = 0;
constexpr int32_t kAntialiasedQuality = 4;
constexpr int32_t kClearTypeQuality = 5;
constexpr int32_t kPixelGeometryBGR = 2;
constexpr int32_t kNotDeclared = -1;

// What Windows does when the text quality changes: the caches that depended on the old
// setting go, and everything is laid out again with the new glyph metrics.
void FlushTextCaches() {
  // gfxPlatformGtk's constructor runs the first update, before gPlatform is set: a flush
  // there would reach GetPlatform() (PurgeSkiaFontCache) and start Init() a second time,
  // which MOZ_RELEASE_ASSERT(!gEverInitialized) turns into a crash. Nothing is cached yet.
  if (!gfxPlatform::Initialized()) {
    return;
  }
  gfxPlatform::FlushFontAndWordCaches();
  gfxPlatform::ForceGlobalReflow(gfxPlatform::GlobalReflowFlags::None);
}

void TextPrefChanged(const char*, void*) {
  StealthIdwrUpdateTextVars();
  // The gamma, contrast, level or mode may have changed without the quality.
  FlushTextCaches();
}

// Registered once, after the first values are set (a gfxVar takes one listener, and the
// pref callbacks must not be doubled).
bool sListenerRegistered = false;
bool sPrefsRegistered = false;

void RegisterListeners() {
  if (!sListenerRegistered) {
    sListenerRegistered = true;
    // In every process, as gfxWindowsPlatform does for SystemTextQuality: the content
    // processes learn of a change through gfxVars.
    gfxVars::SetSystemTextQualityListener(FlushTextCaches);
  }
  if (XRE_IsParentProcess() && !sPrefsRegistered) {
    sPrefsRegistered = true;
    Preferences::RegisterPrefixCallback(TextPrefChanged,
                                        "gfx.font_rendering.cleartype_params.");
    Preferences::RegisterCallback(TextPrefChanged,
                                  "zoom.stealth.text.system_quality"_ns);
  }
}

}  // namespace

bool StealthIdwrPixelGeometryIsBGR() {
  // Until gfxVars is up the structure is the hard-coded starting point, RGB.
  return gfxVars::IsInitialized() &&
         gfxVars::SystemTextPixelGeometry() == kPixelGeometryBGR;
}

void StealthIdwrApplyPixelGeometry() {
  // DWriteSettings.cpp's UpdatePixelGeometry: the order the surfaces draw LCD text in is
  // the declared pixel structure's, not fontconfig's.
  Factory::SetSubpixelOrder(StealthIdwrPixelGeometryIsBGR() ? SubpixelOrder::BGR
                                                            : SubpixelOrder::RGB);
}

StealthIdwrTextVars StealthIdwrTextVarsFromPrefs(const StealthIdwrTextPrefs& aPrefs) {
  // gfxDWriteFont::UpdateClearTypeVars with the state declared: its hard-coded starting
  // point, then each pref when it is in range.
  StealthIdwrTextVars vars;
  if (aPrefs.mQuality == kDefaultQuality ||
      aPrefs.mQuality == kAntialiasedQuality ||
      aPrefs.mQuality == kClearTypeQuality) {
    vars.mQuality = aPrefs.mQuality;
  } else {
    vars.mRefusal =
        "stealth: zoom.stealth.text.system_quality is not one of 0 "
        "(DEFAULT_QUALITY), 4 (ANTIALIASED_QUALITY), 5 (CLEARTYPE_QUALITY). A "
        "missing or malformed declaration refuses rather than asking the host.";
  }
  if (aPrefs.mLevel >= 0 && aPrefs.mLevel <= 100) {
    vars.mLevel = float(aPrefs.mLevel / 100.0);
  }
  if (aPrefs.mContrast >= 0 && aPrefs.mContrast <= 1000) {
    vars.mContrast = float(aPrefs.mContrast / 100.0);
  }
  if (aPrefs.mGamma >= 1000 && aPrefs.mGamma <= 2200) {
    vars.mGamma = float(aPrefs.mGamma / 1000.0);
  }
  if (aPrefs.mStructure >= 0 && aPrefs.mStructure <= 2) {
    vars.mPixelGeometry = aPrefs.mStructure;
  }
  if (aPrefs.mMode >= 0 && aPrefs.mMode <= 5) {
    vars.mRenderingMode = aPrefs.mMode;
  } else if (aPrefs.mMode == kNotDeclared && !vars.mRefusal) {
    // Windows with the mode not declared forces GDI classic for the sizes between 6 px
    // and the maximum (ScaledFontDWrite, gfxDWriteFont::GetScaledFont), which the Linux
    // text does not reproduce: it takes the mode as declared. The core always declares it.
    vars.mRefusal =
        "stealth: gfx.font_rendering.cleartype_params.rendering_mode is not declared. "
        "Without it Windows draws small text with GDI classic, which is not reproduced "
        "here; a missing declaration refuses rather than guessing.";
  }
  return vars;
}

void StealthIdwrUpdateTextVars() {
  // The engine off is stock: nothing is declared.
  if (StaticPrefs::zoom_stealth_fpp_hw_seed() <= 0 || !gfxVars::IsInitialized()) {
    return;
  }
  if (!XRE_IsParentProcess()) {
    // The values reach gfxVars from the parent; the listener is how a later change of
    // the quality reaches this process.
    RegisterListeners();
    StealthIdwrApplyPixelGeometry();
    return;
  }

  StealthIdwrTextPrefs prefs;
  prefs.mQuality = StaticPrefs::zoom_stealth_text_system_quality();
  prefs.mLevel = Preferences::GetInt(
      "gfx.font_rendering.cleartype_params.cleartype_level", -1);
  prefs.mContrast = Preferences::GetInt(
      "gfx.font_rendering.cleartype_params.enhanced_contrast", -1);
  prefs.mGamma =
      Preferences::GetInt("gfx.font_rendering.cleartype_params.gamma", -1);
  prefs.mStructure = Preferences::GetInt(
      "gfx.font_rendering.cleartype_params.pixel_structure", -1);
  prefs.mMode = Preferences::GetInt(
      "gfx.font_rendering.cleartype_params.rendering_mode", -1);
  StealthIdwrTextVars vars = StealthIdwrTextVarsFromPrefs(prefs);
  if (vars.mRefusal) {
    StealthRefuse("%s", vars.mRefusal);
  }

  if (gfxVars::SystemTextQuality() != vars.mQuality) {
    gfxVars::SetSystemTextQuality(vars.mQuality);
  }
  if (gfxVars::SystemTextClearTypeLevel() != vars.mLevel) {
    gfxVars::SetSystemTextClearTypeLevel(vars.mLevel);
  }
  if (gfxVars::SystemTextEnhancedContrast() != vars.mContrast) {
    gfxVars::SetSystemTextEnhancedContrast(vars.mContrast);
  }
  if (gfxVars::SystemTextGamma() != vars.mGamma) {
    gfxVars::SetSystemTextGamma(vars.mGamma);
  }
  if (gfxVars::SystemTextPixelGeometry() != vars.mPixelGeometry) {
    gfxVars::SetSystemTextPixelGeometry(vars.mPixelGeometry);
  }
  if (gfxVars::SystemTextRenderingMode() != vars.mRenderingMode) {
    gfxVars::SetSystemTextRenderingMode(vars.mRenderingMode);
  }
  StealthIdwrApplyPixelGeometry();
  // After the values: a listener registered before them would flush the caches for the
  // first value set, inside the platform's constructor.
  RegisterListeners();
}

}  // namespace gfx
}  // namespace mozilla
