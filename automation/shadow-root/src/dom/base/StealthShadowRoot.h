/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_dom_StealthShadowRoot_h
#define mozilla_dom_StealthShadowRoot_h

#include "ExpandedPrincipal.h"
#include "StealthDeclarationGate.h"
#include "mozilla/Assertions.h"
#include "mozilla/BasePrincipal.h"
#include "mozilla/dom/ShadowRoot.h"

class nsIPrincipal;

namespace mozilla::dom {

/**
 * The answer of Element::GetShadowRootForBindings (the `shadowRoot` getter)
 * for a root the spec says to hide: `aShadowRoot` is null or closed, and
 * Element.cpp has already tested that. Returns the root only for the
 * automation sandbox, nullptr for everybody else.
 *
 * The closed-mode filter gets one exception, for the automation sandbox only.
 *
 * Why here and not at the shadow root's birth: the mode is a fact the page
 * declared and can read back. Forcing a root open when it is created shows up
 * three separate ways - root.mode reads "open", !!el.shadowRoot becomes true,
 * and getHTML() writes shadowrootmode="open" - so the bit stays exactly as the
 * page set it. This is the single place in the whole tree where the mode is
 * filtered for content (Element::GetOpenOrClosedShadowRoot, just below
 * GetShadowRootForBindings in Element.cpp, does not filter it and is left
 * alone), so one exception here covers everything without moving the
 * observable.
 *
 * Who gets the exception: Juggler's isolated world is a Cu.Sandbox built on an
 * ExpandedPrincipal, while the page always has a ContentPrincipal, so the two
 * are distinguishable for free and the page falls through to the spec answer.
 *
 * With the engine off this is byte-for-byte upstream behaviour.
 *
 * It lives in a header, and is inline, on purpose: the first two tests below
 * are the content path, and what content pays there is readable from JS (see
 * the principal test), so it must compile to what it did when it sat inside
 * Element.cpp.
 */
inline ShadowRoot* StealthClosedShadowRootForBindings(
    ShadowRoot* aShadowRoot, nsIPrincipal& aSubject) {
  if (!aShadowRoot) {
    return nullptr;
  }
  MOZ_ASSERT(aShadowRoot->IsClosed());

  // Content is the overwhelmingly common caller here and it must not pay for
  // an exception it never takes, because what it pays is READABLE FROM JS. The
  // first version asked nsIPrincipal::GetIsExpandedPrincipal(), which is an
  // NS_IMETHOD - a virtual XPCOM dispatch with an nsresult out-param - and put
  // it after the pref read, so a page reading .shadowRoot off its own closed
  // root paid both. Measured against retail 151 that showed up as a ratio a
  // page can compute without knowing anything about the machine: time a read
  // on a closed root over a read on an element with no root, and retail says
  // 1.13 while we said 1.95. Absolute nanoseconds are not a tell (nobody knows
  // how fast this box is); a self-normalising ratio is. So the cheap form -
  // one load and one compare off BasePrincipal::mKind - and it goes first, so
  // content returns on it and never reaches the pref.
  if (!BasePrincipal::Cast(aSubject).Is<ExpandedPrincipal>()) {
    return nullptr;
  }

  // A UA WIDGET IS NOT AUTHOR CONTENT, AND HANDING ONE TO AUTOMATION BREAKS
  // THE PAGE'S OWN DOM.
  //
  // The exception above was written for shadow roots the PAGE created and
  // declared closed. Firefox also builds its own controls inside form
  // elements - the calendar button of <input type=date|time>, the video
  // controls, and so on - in a shadow root that is likewise closed, and
  // nothing in the conditions above tells the two apart. So a selector
  // engine running in the sandbox, which pierces shadow roots by design and
  // is right to, started collecting engine internals as if the page had
  // written them: `page.locator("button")` returned two matches on a document
  // containing one button, the extra one invisible and therefore never
  // clickable.
  //
  // Element::GetOpenOrClosedShadowRoot, in Element.cpp 29 lines below
  // GetShadowRootForBindings, has carried exactly this guard since upstream
  // bug 2035665 refused UA widgets to extensions. The note above says that
  // function "does not filter it and is left alone", and that was true of the
  // filter it was talking about, the MODE one. It has a SECOND filter, and
  // the sandbox path needed that one too.
  //
  // Ordering: this sits after the principal compare, not before it, so the
  // content path stays byte for byte what was measured against retail. Only
  // the sandbox pays the extra test, and the sandbox is not what a page can
  // time.
  if (aShadowRoot->IsUAWidget()) {
    return nullptr;
  }

  // Only the sandbox gets this far, so the pref read is off the hot path.
  // Measured 2026-08-22, which is why the subject principal is a parameter at
  // all: that a sandbox access arrives carrying the ExpandedPrincipal was
  // reasoned about before it was observed, so a printf_stderr sat on the
  // refusal path until a locator reached an element inside a closed root -
  // and inside a closed root nested in another one - while the page's own
  // script still read null off the same host.
  if (!mozilla::gfx::StealthEngineActive()) {
    return nullptr;  // stock Firefox
  }

  return aShadowRoot;
}

}  // namespace mozilla::dom

#endif  // mozilla_dom_StealthShadowRoot_h
