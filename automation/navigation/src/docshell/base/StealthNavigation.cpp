/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "StealthNavigation.h"

#include <inttypes.h>

#include "MainThreadUtils.h"
#include "mozilla/Assertions.h"
#include "mozilla/Services.h"
#include "nsCOMPtr.h"
#include "nsDocLoader.h"
#include "nsDocShell.h"
#include "nsDocShellLoadState.h"
#include "nsIContent.h"
#include "nsIObserverService.h"
#include "nsPrintfCString.h"
#include "nsString.h"

// Sends aTopic with subject aSubject and the load identifier as a decimal
// string payload.
static void NotifyNavigationStarted(nsISupports* aSubject, const char* aTopic,
                                    uint64_t aLoadIdentifier) {
  MOZ_ASSERT(NS_IsMainThread());
  nsCOMPtr<nsIObserverService> observerService =
      mozilla::services::GetObserverService();
  if (observerService) {
    observerService->NotifyObservers(
        aSubject, aTopic,
        NS_ConvertASCIItoUTF16(nsPrintfCString("%" PRIu64, aLoadIdentifier))
            .get());
  }
}

void StealthNavigationStarted(nsDocShell* aDocShell,
                              nsDocShellLoadState* aLoadState, bool aNotify) {
  if (!aNotify) {
    return;
  }
  MOZ_ASSERT(aDocShell);
  MOZ_ASSERT(aLoadState);
  // GetAsSupports is the static of nsDocLoader that nsDocShell inherits;
  // from a free function it must be named through the class. Same pointer as
  // GetAsSupports(this) inside nsDocShell.
  NotifyNavigationStarted(nsDocLoader::GetAsSupports(aDocShell),
                          "juggler-navigation-started-renderer",
                          aLoadState->GetLoadIdentifier());
}

// The one place a link click topic is sent. Observer notifications: nothing a
// page can see.
static void NotifyLinkClick(nsIContent* aContent, const char* aTopic) {
  MOZ_ASSERT(NS_IsMainThread());
  if (nsCOMPtr<nsIObserverService> observerService =
          mozilla::services::GetObserverService()) {
    observerService->NotifyObservers(ToSupports(aContent), aTopic, nullptr);
  }
}

void StealthLinkClickBefore(nsIContent* aContent) {
  NotifyLinkClick(aContent, "juggler-link-click");
}

void StealthLinkClickAfter::Done(nsIContent* aContent) {
  if (mNotified) {
    return;
  }
  mNotified = true;
  NotifyLinkClick(aContent, "juggler-link-click-sync");
}
