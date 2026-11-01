/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef GFX_STEALTHTEXTMEASURING_H
#define GFX_STEALTHTEXTMEASURING_H

// The declared DirectWrite rendering mode, readable on any thread. Defined in
// StealthDesignMetrics.cpp; cross-platform, unlike StealthDesignMetrics.h.
//
// This is the one header gfxFont.cpp's hooks include, so it also brings
// StealthBundleFontList.h for the SanitizeMetrics hook: one include line
// instead of two keeps gfxFont.cpp inside its hook budget.

#include <stdint.h>

#include "StealthBundleFontList.h"

namespace mozilla {
namespace gfx {

/**
 * Registers the mirror of gfx.font_rendering.cleartype_params.rendering_mode.
 * Main thread only. Called from gfxFontCache::Init, which gfxPlatform::Init
 * runs on the main thread of every process before any gfxFont can exist (every
 * font goes through the cache), so no font metrics are ever computed before
 * the mirror is filled.
 *
 * Why a mirror. gfxFT2FontBase::InitMetrics runs wherever a font is first
 * measured, workers included (OffscreenCanvas measureText), and
 * Preferences::GetInt is main-thread only. The pref has no StaticPrefs mirror
 * (it lives in all.js, Windows only), and the gfxVar Windows reads it through
 * (SystemTextRenderingMode) is only filled by gfxDWriteFont. A pref callback
 * that stores into an atomic is the thread-safe reader that is left. Windows
 * itself reads a copy too: the parent's UpdateClearTypeVars turns the pref into
 * gfxVars, which content processes receive.
 */
void StealthTextMeasuringInit();

/**
 * The declared rendering mode as the pref holds it, any thread: -1 when it is
 * not set or before StealthTextMeasuringInit.
 */
int32_t StealthDeclaredRenderingMode();

}  // namespace gfx
}  // namespace mozilla

#endif  // GFX_STEALTHTEXTMEASURING_H
