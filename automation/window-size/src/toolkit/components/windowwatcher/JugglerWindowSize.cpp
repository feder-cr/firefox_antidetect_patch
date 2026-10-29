/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "JugglerWindowSize.h"

#include "nsIWebBrowserChrome.h"
#include "nsWindowWatcher.h"

namespace mozilla {

uint32_t JugglerPopupChromeFlags(const dom::WindowFeatures& aFeatures) {
  // Juggler reads this flag to tell a popup that asked for a size from one
  // that did not: without it every popup got the browser context's default
  // viewport, so window.open(url, '', 'width=500,height=400') reported the
  // tab's 1920x947 where Firefox reports 499x400. HaveSpecifiedSize is the
  // test nsWindowWatcher already uses to decide whether the window gets sized.
  if (nsWindowWatcher::HaveSpecifiedSize(aFeatures)) {
    return nsIWebBrowserChrome::CHROME_MINIMAL_POPUP |
           nsIWebBrowserChrome::JUGGLER_WINDOW_EXPLICIT_SIZE;
  }
  return nsIWebBrowserChrome::CHROME_MINIMAL_POPUP;
}

}  // namespace mozilla
