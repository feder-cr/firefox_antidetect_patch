/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_dom_StealthNavigationEvents_h
#define mozilla_dom_StealthNavigationEvents_h

#include <inttypes.h>

#include "mozilla/Assertions.h"
#include "mozilla/Services.h"
#include "mozilla/dom/CanonicalBrowsingContext.h"
#include "nsCOMPtr.h"
#include "nsDocShellLoadState.h"
#include "nsIObserverService.h"
#include "nsISupportsImpl.h"
#include "nsPrintfCString.h"
#include "nsString.h"

namespace mozilla::dom {

// Tells the automation layer (observer topic
// "juggler-navigation-started-browser") that a top-level load is about to
// start. The subject is the browsing context, the data is the load identifier
// of aLoadState as a decimal string. Call it right before the Mozilla
// CanonicalBrowsingContext::LoadURI(aLoadState, true) in the two entry points
// that start a load from a URI (LoadURI with LoadURIOptions and
// FixupAndLoadURIString). Main thread only.
//
// Header-only on purpose: it is included from one file of docshell/base, and
// this way docshell/base/moz.build needs no extra line.
inline void StealthNotifyNavigationStarted(CanonicalBrowsingContext* aContext,
                                           nsDocShellLoadState* aLoadState) {
  MOZ_ASSERT(aContext);
  MOZ_ASSERT(aLoadState);
  nsCOMPtr<nsIObserverService> observerService =
      mozilla::services::GetObserverService();
  if (observerService) {
    observerService->NotifyObservers(
        ToSupports(aContext), "juggler-navigation-started-browser",
        NS_ConvertASCIItoUTF16(
            nsPrintfCString("%" PRIu64, aLoadState->GetLoadIdentifier()))
            .get());
  }
}

}  // namespace mozilla::dom

#endif  // mozilla_dom_StealthNavigationEvents_h
