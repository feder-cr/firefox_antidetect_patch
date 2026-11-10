/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_StealthJugglerEventId_h
#define mozilla_StealthJugglerEventId_h

#include <stdint.h>

#include "mozilla/MouseEvents.h"  // WidgetMouseEventBase

namespace mozilla {

/**
 * Stealthfox [B217]: the id travels on the event, see widget/MouseEvents.h
 * (WidgetMouseEventBase::mJugglerEventId) and automation/input/FEATURE.md.
 * Copies the Juggler event id of a synthesized mouse event's data onto the
 * event (0 for every event that did not come from Juggler).
 */
inline void StealthSetJugglerId(WidgetMouseEventBase& aEvent, uint32_t aId) {
  aEvent.mJugglerEventId = aId;
}

/**
 * Stealthfox [F6]: a Juggler event (it carries an id) is not "async synthesized
 * for tests" when it is dispatched on the asynchronous (APZ) route, which sets
 * that flag for every synthesized event. Marionette's events keep it. Juggler
 * drives its own drags on a synthesized drag session (jugglerSendDragEvent),
 * and the flag would send the session to the native drag loop
 * (nsBaseDragService.cpp). The APZ route is wanted for the hit test alone.
 */
inline void StealthJugglerNotAsyncSynthesized(WidgetMouseEventBase& aEvent) {
  if (aEvent.mJugglerEventId) {
    aEvent.mFlags.mIsAsyncSynthesizedForTests = false;
  }
}

}  // namespace mozilla

#endif  // mozilla_StealthJugglerEventId_h
