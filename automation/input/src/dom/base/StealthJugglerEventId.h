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

}  // namespace mozilla

#endif  // mozilla_StealthJugglerEventId_h
