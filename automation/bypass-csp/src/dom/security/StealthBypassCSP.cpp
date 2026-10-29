/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// Automation: bypassCSP. The one place that answers whether the automation
// asked this document's docshell to ignore the policies the page itself
// sends. Declared in nsCSPUtils.h; called by Document::InitCSP (header
// policies) and CSP_ApplyMetaCSPToDoc (a <meta> policy).

#include "nsCSPUtils.h"

#include "mozilla/dom/Document.h"
#include "nsError.h"
#include "nsIDocShell.h"

// Whether the automation asked this document's docshell to ignore the page's
// own policies (Juggler's `bypassCSP` context setting, Playwright's
// `bypass_csp`). The one place the question is answered: InitCSP for the
// header policies, CSP_ApplyMetaCSPToDoc for a <meta> one.
//
// True when the docshell the document lives in has bypassCSPEnabled set (the
// automation's per-context `bypassCSP` setting, written through
// nsIDocShell::SetBypassCSPEnabled). A document without a docshell (loaded as
// data, or already detached) is never bypassed.
// Until this existed the docshell attribute was stored and never read:
// the 2026-05 port added it as storage only, so `bypass_csp=True` changed
// nothing a page enforced (issue #295 of invisible_playwright).
bool CSP_IsBypassedFor(const mozilla::dom::Document& aDoc) {
  nsIDocShell* docShell = aDoc.GetDocShell();
  if (!docShell) {
    return false;
  }
  bool bypass = false;
  return NS_SUCCEEDED(docShell->GetBypassCSPEnabled(&bypass)) && bypass;
}
