/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef xpc_StealthSandbox_h
#define xpc_StealthSandbox_h

#include "ExpandedPrincipal.h"
#include "mozilla/BasePrincipal.h"
#include "nsIPrincipal.h"
#include "xpcprivate.h"

#include "js/ErrorReport.h"  // JS_ReportErrorASCII
#include "js/TypeDecls.h"

namespace xpc {

/**
 * STEALTHFOX [B322]: what js/xpconnect/src/Sandbox.cpp needs for the caller's
 * world (the `hybridXrays` option of Cu.Sandbox, which only Juggler passes;
 * the compartment flag is CompartmentPrivate::hybridXrays in xpcprivate.h and
 * the wrapper rule is written above HybridXrayWrapper in XrayWrapper.h).
 *
 * Sections, one per hook in Sandbox.cpp:
 *
 *   getPropertyDescriptorImpl (SandboxProxyHandler): in a caller's world the
 *   prototype is the page's window behind a hybrid Xray, and a bare name
 *   (`document`, the page's `myFunction`, its top-level `const config`)
 *   resolves by the hybrid rule, in the order the page's realm resolves it
 *   (HybridXrayGetBareName, XrayWrapper.h). Walking the wrapper generically
 *   would join the two views level by level, so a property the page put on
 *   the window could hide a standard one.
 *
 *   CreateSandboxObject: StealthCheckHybridXrays, below.
 *
 *   CreateSandboxObject (compartment flag) and SandboxOptions::Parse: the
 *   flag is read from the option, checked against a reused compartment
 *   (MOZ_RELEASE_ASSERT) and stored on the new one, next to wantXrays.
 *
 * Main thread.
 */

// Checks the `hybridXrays` option of a sandbox about to be created. A
// caller's world has the page's own principal, and `hybridXrays` replaces
// `wantXrays` instead of adding to it: `wantXrays` would impose Xrays on the
// page reading the caller's objects as well. So the option needs a content
// principal (not the system principal, not an expanded one) and
// `wantXrays: false`. On refusal this reports a JS error on aCx and returns
// false; the caller returns NS_ERROR_INVALID_ARG. Without the option it
// returns true and does nothing.
inline bool StealthCheckHybridXrays(JSContext* aCx, nsIPrincipal* aPrincipal,
                                    const SandboxOptions& aOptions) {
  if (aOptions.hybridXrays &&
      (aPrincipal->IsSystemPrincipal() ||
       mozilla::BasePrincipal::Cast(aPrincipal)
           ->Is<ExpandedPrincipal>() ||
       aOptions.wantXrays)) {
    JS_ReportErrorASCII(aCx,
                        "hybridXrays needs a content principal and "
                        "wantXrays: false");
    return false;
  }
  return true;
}

}  // namespace xpc

#endif  // xpc_StealthSandbox_h
