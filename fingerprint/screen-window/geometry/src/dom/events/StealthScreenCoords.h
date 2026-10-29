/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_dom_StealthScreenCoords_h
#define mozilla_dom_StealthScreenCoords_h

// Defined here, not in a source file of its own: dom/events/moz.build
// registers none for it, so the hook in Event.cpp adds no line to a shared
// build file. Included by Event.cpp only.

#include "mozilla/Maybe.h"
#include "nsGlobalWindowOuter.h"
#include "nsPoint.h"

class nsPresContext;

namespace mozilla::dom {

// The declared content origin, in app units, that Event::GetScreenCoords adds
// to the screen point of a mouse event so that
//
//     event.screenX - event.clientX === window.mozInnerScreenX
//
// stays true. (0, 0) when the engine is off, or when there is nothing to
// declare: the sum in Event.cpp is then exactly stock Firefox's.
//
// It asks nsGlobalWindowOuter::StealthContentOriginShift, the one function
// mozInnerScreenX and mozInnerScreenY ask too. Main thread only, like its
// caller. Arithmetic and widget queries only, no layout: this runs for every
// trusted mouse and wheel event.
//
// The text below was written in Event::GetScreenCoords, where this sum is
// formed: "this function", "here" and "up here" mean that function, which
// calls this one for the last term.
//
// Stealth: the origin comes from the SAME function mozInnerScreenX answers
// with, and that is the whole point of it being a function.
//
// This is the funnel: MouseEvent, UIEvent, Touch and EventStateManager all
// reach screen coordinates through here, so it is the one place where the
// relation a page checks is either true or false.
//
//     event.screenX - event.clientX === window.mozInnerScreenX
//
// It holds on every real Firefox because both sides come from this widget.
// When the window geometry became a declaration on 2026-08-09 the three DOM
// getters were moved and this line was not, so the two sides came from
// different places and the relation failed on 45 mouse events out of 45 on
// Windows and 20 of 20 on Linux, against 0 of 6 on stock. Humanised movement
// made it louder rather than quieter: dozens of events per action, each one a
// fresh sample of the same contradiction.
//
// Adding the declared origin HERE rather than teaching the widget to lie is
// deliberate. The widget works in device pixels and cannot see the CSS scale
// - layout.css.devPixelsPerPx is applied above it, in the device context - so
// a declaration down there would need a conversion factor the widget does not
// have. Up here the units are known and the arithmetic is the same one the
// getter does.
// Written as the relation itself - screen = client + content origin - and not
// as "the widget-relative point plus something", because the widget-relative
// point sits inside the REAL window and carries the real chrome height with
// it. That was the first attempt and it moved the declared amount while
// leaving the same gap: with chrome_h at 85 the offset was 800, and with
// chrome_h at 300 it was still 800, because both sides had moved together.
// The client point is the one the page already receives, so adding the origin
// to it makes the relation true by construction rather than by arithmetic
// that happens to line up.
//
// Arithmetic only, and that is not an optimisation, it is a correctness
// requirement. THIS FUNCTION IS THE HOT PATH: EventStateManager::PreHandleEvent
// calls it for every trusted real mouse event and every wheel event, to store
// sLastScreenPoint, and then calls GetClientCoords right after. A first
// version of this branch derived the client point here with
// nsLayoutUtils::GetEventCoordinatesRelativeTo, which put a SECOND layout
// query on every mouse event - and humanised movement emits dozens per action.
// Measured: the full Windows e2e went from 2 mouse failures to 7, plus 4
// detector tests that had never failed, all of them timeouts, and all of them
// passing again when the same tests ran alone. A correctness fix that makes
// the browser too slow to click is not a fix.
//
// The identity used instead needs no layout at all:
//
//     screen_real     = topLevelRelative + topLevelWidgetOrigin
//     client          = screen_real - realContentOrigin
//     screen_declared = client + declaredContentOrigin
//                     = screen_real - realContentOrigin + declaredOrigin
//
// realContentOrigin is the root widget's screen offset - the very quantity
// mozInnerScreenX reported before it became a declaration - so the relation
// the gate checks holds by construction, and both terms are widget calls the
// original expression already made.
// Lo scarto lo calcola UNA funzione, la stessa che chiamano mozInnerScreenX e
// mozInnerScreenY. Quando l'aritmetica stava soltanto qui, la sostituzione
// secca in nsGlobalWindowOuter ha ricreato dentro gli iframe la contraddizione
// che questo blocco toglie al primo livello - misurato il 2026-08-10.
inline nsPoint StealthScreenOriginShift(nsPresContext* aPresContext) {
  // Nothing() (engine off, no PresContext, no root widget, a parent process
  // popup) is the zero shift: the sum then equals stock Firefox's.
  return nsGlobalWindowOuter::StealthContentOriginShift(aPresContext)
      .valueOr(nsPoint());
}

}  // namespace mozilla::dom

#endif  // mozilla_dom_StealthScreenCoords_h
