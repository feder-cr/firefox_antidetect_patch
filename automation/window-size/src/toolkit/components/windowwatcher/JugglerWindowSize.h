/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef TOOLKIT_COMPONENTS_WINDOWWATCHER_JUGGLERWINDOWSIZE_H_
#define TOOLKIT_COMPONENTS_WINDOWWATCHER_JUGGLERWINDOWSIZE_H_

#include <stdint.h>

#include "mozilla/dom/WindowFeatures.h"

namespace mozilla {

// Chrome flag set on a popup whose window.open features give a size. It is
// ours, so it is not declared in nsIWebBrowserChrome.idl: Mozilla fills the
// free bits there (159 took 1 << 25, where this flag used to be). Bit 1 is
// one nsIWebBrowserChrome leaves free; juggler/TargetRegistry.js reads the
// same value, and juggler/tests/explicit_size_flag_is_free.test.mjs checks
// that both agree and that no CHROME_* flag uses the bit.
// STEALTH-FLAG-SPACE(toolkit/components/browser/nsIWebBrowserChrome.idl, CHROME_)
constexpr uint32_t kJugglerWindowExplicitSize = 1u << 1;

/**
 * The chrome flags of a popup that a page asked for (ShouldOpenPopup was true):
 * CHROME_MINIMAL_POPUP, plus kJugglerWindowExplicitSize when the features
 * give a size. Called by nsWindowWatcher::CalculateChromeFlagsForContent only
 * on its popup branch.
 */
uint32_t JugglerPopupChromeFlags(const dom::WindowFeatures& aFeatures);

}  // namespace mozilla

#endif  // TOOLKIT_COMPONENTS_WINDOWWATCHER_JUGGLERWINDOWSIZE_H_
