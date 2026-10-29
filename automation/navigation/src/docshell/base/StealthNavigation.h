/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef docshell_base_StealthNavigation_h
#define docshell_base_StealthNavigation_h

#include <stdint.h>

class nsDocShell;
class nsDocShellLoadState;
class nsIContent;
class nsISupports;

// STEALTHFOX (juggler): notifications Juggler needs from the navigation
// machinery. Observer notifications: nothing a page can see.

// Called by nsDocShell::InternalLoad right before DoURILoad. When aNotify is
// true (a navigation that is neither a javascript: URL nor a same-document
// one) reports 'juggler-navigation-started-renderer' for aDocShell. This
// drives the Juggler protocol's Page.navigationStarted and, ultimately,
// Page.ready.
void StealthNavigationStarted(nsDocShell* aDocShell,
                              nsDocShellLoadState* aLoadState, bool aNotify);

// 'Before' of a planned navigation: a link click or a form submission is a
// PLANNED navigation, an OnLinkClickEvent that runs after the input that caused
// it returns; from 157 a form submission is one too
// (dom.forms.submit_async_navigation, true as in the retail). Juggler reports
// the plan as Page.linkClicked "before" and its end as "after"
// (juggler/content/PageAgent.js), so a client can tell an action that is about
// to navigate from one that is not: without it the POST of a submitted form was
// announced after page.click() had returned. Sent by
// nsDocShell::OnLinkClickWithLoadState once the event is dispatched: the plan
// exists only once it is dispatched, so the "before" is never sent for a click
// that planned nothing. Topic 'juggler-link-click'.
void StealthLinkClickBefore(nsIContent* aContent);

// State and rules of the 'after' for one OnLinkClickEvent. Every "before" gets
// exactly one "after" (topic 'juggler-link-click-sync'), sent when the
// navigation starts (Run, once OnLinkClickSync has started it) or when the plan
// is cancelled (another submission replaces it), whichever comes first, so a
// client waiting on the pair is never left waiting.
class StealthLinkClickAfter final {
 public:
  // Run() brackets OnLinkClickSync with these.
  void SetRunning(bool aRunning) { mRunning = aRunning; }

  // Cancel(): Mozilla's comment there says it can be called from
  // OnLinkClickSync(); it is, for a form: InternalLoad stops this docshell's
  // network, which cancels its planned form navigation - the one running
  // here - before the navigation is announced. While Run is inside
  // OnLinkClickSync it says "after" itself once the navigation has started,
  // so Cancel stays silent then.
  void Cancelled(nsIContent* aContent) {
    if (!mRunning) {
      Done(aContent);
    }
  }

  // Sends the "after", once, whichever of Run and Cancel comes first.
  void Done(nsIContent* aContent);

 private:
  bool mRunning = false;
  bool mNotified = false;
};

#endif  // docshell_base_StealthNavigation_h
