/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "InterceptedHttpChannel.h"

#include "nsCOMPtr.h"
#include "nsError.h"
#include "nsINetworkInterceptController.h"
#include "nsIURI.h"
#include "nsNetUtil.h"

// What the automation driver (Juggler) needs from an intercepted channel that
// a service worker may or may not answer.
//
// The driver intercepts the requests it is told to. A request a service worker
// could answer is offered to the worker first: the driver calls
// InterceptAfterServiceWorkerResets() to be called again if the worker lets
// the request through. ResetInterception() (InterceptedHttpChannel.cpp, one
// STEALTH-HOOK) then hands the channel back to the driver's interceptor
// (StealthInterceptAgain) instead of going on, and keeps the bypass the worker
// asked for in mServiceWorkerResetBypass. The driver finally resumes the
// request with ResetInterceptionWithURI().
//
// State, declared in InterceptedHttpChannel.h:
//  - mInterceptAfterServiceWorkerResets: after resetInterception is called,
//    this request calls into the interceptors again.
//  - mServiceWorkerResetBypass: the bypass a service worker asked for when it
//    let this request through and the reset was handed to the interceptors
//    instead. A resume that does not change the URL carries it out: false when
//    the worker simply did not respond, which keeps the navigation's client
//    controlled. True when no worker was involved (there is no controller to
//    clear then).
//
// OnStartRequest() (InterceptedHttpChannel.cpp) keeps Mozilla's two asserts on
// the loading principal and the service worker tainting. Main requests that
// the driver intercepts have no loading principal and no service worker
// tainting, so both would fail in a DEBUG or diagnostic build. (Mozilla added
// these checks in
// https://github.com/mozilla-firefox/firefox/commit/bb16ca6496682c3b0ddd452d0dd4c1dd46ff71f8.)
// Shipped builds are release builds, where MOZ_ASSERT_IF and
// MOZ_DIAGNOSTIC_ASSERT compile to nothing, so nothing is lost there.

namespace mozilla::net {

NS_IMETHODIMP
InterceptedHttpChannel::InterceptAfterServiceWorkerResets() {
  mInterceptAfterServiceWorkerResets = true;
  return NS_OK;
}

NS_IMETHODIMP
InterceptedHttpChannel::ResetInterceptionWithURI(nsIURI* aURI) {
  if (aURI) {
    mURI = aURI;
    // A controller chosen for the original URL does not follow the request
    // somewhere else.
    return ResetInterception(true);
  }
  // Unchanged: do what the service worker that let it through asked for.
  // Forcing true here cleared the controller of every navigation a worker
  // did not answer, so with Juggler attached no page was ever controlled by
  // its service worker, route or not.
  return ResetInterception(mServiceWorkerResetBypass);
}

// Called by ResetInterception() when InterceptAfterServiceWorkerResets() was
// called before: the interceptors get the channel again.
nsresult InterceptedHttpChannel::StealthInterceptAgain(bool aBypass) {
  mInterceptAfterServiceWorkerResets = false;
  mServiceWorkerResetBypass = aBypass;
  nsCOMPtr<nsINetworkInterceptController> controller;
  GetCallback(controller);
  if (!controller) {
    return NS_ERROR_DOM_INVALID_STATE_ERR;
  }
  return controller->ChannelIntercepted(this);
}

}  // namespace mozilla::net
