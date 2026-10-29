/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef TOOLKIT_COMPONENTS_WINDOWWATCHER_JUGGLERWINDOWSIZE_H_
#define TOOLKIT_COMPONENTS_WINDOWWATCHER_JUGGLERWINDOWSIZE_H_

#include <stdint.h>

#include "mozilla/dom/WindowFeatures.h"

namespace mozilla {

/**
 * The chrome flags of a popup that a page asked for (ShouldOpenPopup was true):
 * CHROME_MINIMAL_POPUP, plus JUGGLER_WINDOW_EXPLICIT_SIZE when the features
 * give a size. Called by nsWindowWatcher::CalculateChromeFlagsForContent only
 * on its popup branch.
 */
uint32_t JugglerPopupChromeFlags(const dom::WindowFeatures& aFeatures);

}  // namespace mozilla

#endif  // TOOLKIT_COMPONENTS_WINDOWWATCHER_JUGGLERWINDOWSIZE_H_
