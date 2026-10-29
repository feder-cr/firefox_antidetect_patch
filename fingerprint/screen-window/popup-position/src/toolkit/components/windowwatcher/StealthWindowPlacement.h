/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef TOOLKIT_COMPONENTS_WINDOWWATCHER_STEALTHWINDOWPLACEMENT_H_
#define TOOLKIT_COMPONENTS_WINDOWWATCHER_STEALTHWINDOWPLACEMENT_H_

#include "Units.h"

class nsIBaseWindow;

namespace mozilla {

/**
 * Called by SizeOpenedWindow (nsWindowWatcher.cpp) after it has taken left/top
 * from the page's features and before it sets width and height.
 *
 * A window a page opens without left/top is born at the declared default
 * position. Writes aLeft only when !aHasLeft and aTop only when !aHasTop.
 * Returns true when a position was declared, and then the caller treats the
 * position as specified; false (nothing written, nothing read from the window)
 * for a chrome caller, or when the engine is off.
 */
[[nodiscard]] bool StealthPlacePopupAtDefault(bool aIsCallerChrome,
                                              nsIBaseWindow* aTreeOwnerAsWin,
                                              bool aHasLeft, bool aHasTop,
                                              DesktopIntCoord& aLeft,
                                              DesktopIntCoord& aTop);

}  // namespace mozilla

#endif  // TOOLKIT_COMPONENTS_WINDOWWATCHER_STEALTHWINDOWPLACEMENT_H_
