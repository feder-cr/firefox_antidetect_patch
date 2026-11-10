/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_StealthFrameCrash_h
#define mozilla_StealthFrameCrash_h

#include "mozilla/dom/BrowsingContext.h"
#include "nsString.h"

namespace mozilla {

/**
 * Stealthfox: the data of the "oop-frameloader-crashed" notification, the id of
 * the BrowsingContext that crashed. An out-of-process iframe's BrowserParent has
 * the TAB's owner element (BrowserBridgeParent::Init), so the frameloader of the
 * notification is the tab's for the crash of an iframe too, and an observer that
 * cannot tell the two closes the tab for an iframe (automation/juggler,
 * TargetRegistry.js). Null when the caller does not say which context crashed
 * (a frameloader that failed to build).
 *
 * `aBuffer` holds the digits for as long as the notification runs.
 */
inline const char16_t* StealthCrashedContextData(
    dom::BrowsingContext* aBrowsingContext, nsAutoString& aBuffer) {
  if (!aBrowsingContext) {
    return nullptr;
  }
  aBuffer.AppendInt(aBrowsingContext->Id());
  return aBuffer.get();
}

}  // namespace mozilla

#endif  // mozilla_StealthFrameCrash_h
