/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "StealthJugglerGate.h"

#include "nsCOMPtr.h"
#include "nsIRemoteDebuggingPipe.h"
#include "nsServiceManagerUtils.h"

// Juggler is this build's automation agent, and an attached Juggler pipe is
// the same condition as a running Marionette or Remote Agent. Without it
// Browser.setIgnoreHTTPSErrors failed with NS_ERROR_NOT_AVAILABLE on every
// call, so ignore_https_errors=True could not create a context at all.
// Upstream Playwright returns true there unconditionally; this keeps the stock
// refusal for any chrome code that runs without an automation client attached.
// Nothing here is visible to a page: it only decides whether the per-context
// switch may be flipped.
//
// The lookup goes through the contract id on purpose: it creates the pipe
// singleton on first use, exactly as the inline code did, and it must run
// before the Marionette and Remote Agent lookups in the caller. This file is
// built only where Juggler is (juggler/ is added under ENABLE_WEBDRIVER in
// toolkit/toolkit.mozbuild), as is the call site.
bool StealthJugglerPipeConnected() {
  nsCOMPtr<nsIRemoteDebuggingPipe> jugglerPipe =
      do_GetService("@mozilla.org/juggler/remotedebuggingpipe;1");
  if (!jugglerPipe) {
    return false;
  }
  bool jugglerConnected = false;
  jugglerPipe->GetConnected(&jugglerConnected);
  return jugglerConnected;
}
