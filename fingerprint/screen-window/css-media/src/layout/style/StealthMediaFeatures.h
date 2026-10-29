/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* The stealth declarations that nsMediaFeatures.cpp reads: the device size and
 * the pointer capabilities. Logic and comments live here; the Mozilla file only
 * carries STEALTH-HOOK(fingerprint/screen-window/css-media) calls. */

#ifndef LAYOUT_STYLE_STEALTHMEDIAFEATURES_H_
#define LAYOUT_STYLE_STEALTHMEDIAFEATURES_H_

#include <stdint.h>

#include "StealthDeclarationGate.h"
#include "Units.h"
#include "mozilla/LookAndFeel.h"
#include "mozilla/Maybe.h"
#include "mozilla/StaticPrefs_zoom.h"
#include "nsSize.h"

namespace mozilla {

/**
 * The device size declared by the launcher (zoom.stealth.screen.width and
 * zoom.stealth.screen.height), in app units, or Nothing() when it is not
 * declared (either value <= 0).
 *
 * Deliberately NOT gated on gfx::StealthEngineActive() and it does not call
 * gfx::StealthAssertDeclarationsComplete(): that is how this block always
 * behaved, and nsScreen::GetRect (dom/base/nsScreen.cpp) reads the same two
 * prefs the same way. Changing either would change what a page sees.
 */
inline Maybe<nsSize> StealthDeclaredDeviceSize() {
  // Stealth: spoof device size from prefs set by Python at launch
  const int32_t w = StaticPrefs::zoom_stealth_screen_width();
  const int32_t h = StaticPrefs::zoom_stealth_screen_height();
  if (w > 0 && h > 0) {
    return Some(CSSPixel::ToAppUnits(CSSIntSize(w, h)));
  }
  return Nothing();
}

/**
 * The pointer capabilities declared for aID, as the raw bitmask of
 * PointerCapabilities, or Nothing() when the engine is off or the pref for
 * this ID is < 0 (not declared): the caller then falls through to the compiled
 * default. Pointer capabilities are DECLARED, one value for the primary
 * pointer and one for the union of all of them.
 *
 * This used to be a compiled `Fine | Hover` for both, with the comment
 * "always report desktop pointer, consistent with maxTouchPoints=0". The
 * intent was right and it stopped a real leak -
 * `WinUtils::GetAllPointerCapabilities` adds Coarse whenever
 * `SystemHasTouch()`, so a developer machine with a digitizer would have said
 * `any-pointer: coarse` under a persona claiming a desktop. But it was a
 * second source of truth for something the core already describes
 * (`hardware.max_touch_points`), and it could not be pinned: a persona for a
 * touch laptop was not expressible at all.
 *
 * The core derives both from the touch-point count, so `any-pointer`,
 * `dom.w3c_touch_events.enabled` and `navigator.maxTouchPoints` cannot
 * disagree - and they did. Measured 2026-08-09, our build reported the Touch
 * interfaces present (the host has touch) with maxTouchPoints 0 and
 * `any-pointer: coarse` false: a machine that supports touch events, has no
 * touch points, and has no coarse pointer. No such machine exists.
 *
 * The prefs are read ONLY when the engine is active, and the declaration set
 * is asserted complete only when a value was actually declared.
 */
inline Maybe<int32_t> StealthDeclaredPointerCapabilities(
    LookAndFeel::IntID aID) {
  if (!gfx::StealthEngineActive()) {
    return Nothing();
  }
  const int32_t declared =
      aID == LookAndFeel::IntID::PrimaryPointerCapabilities
          ? StaticPrefs::zoom_stealth_pointer_primary()
          : StaticPrefs::zoom_stealth_pointer_all();
  if (declared >= 0) {
    gfx::StealthAssertDeclarationsComplete();
    return Some(declared);
  }
  return Nothing();
}

}  // namespace mozilla

#endif  // LAYOUT_STYLE_STEALTHMEDIAFEATURES_H_
