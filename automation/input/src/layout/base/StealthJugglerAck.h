/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_StealthJugglerAck_h
#define mozilla_StealthJugglerAck_h

// Header-only on purpose: it is included by PresShell.cpp alone, so it needs
// no line in layout/base/moz.build.

#include <stdint.h>

#include "mozilla/Attributes.h"
#include "mozilla/MouseEvents.h"
#include "mozilla/Services.h"
#include "nsCOMPtr.h"
#include "nsIObserverService.h"
#include "nsString.h"

namespace mozilla {

/**
 * Stealthfox [B217]: the renderer acks a juggler mouse event AFTER handling
 * it, once. Declared on the stack in PresShell::HandleEvent right after
 * RecordModifiers, so every return path of HandleEvent acks.
 *
 * The id is what `jugglerSendMouseEvent` handed back in the parent; it rode the
 * event here (and through coalescing, which keeps the last id folded in).
 * Content-side Juggler observes this to answer "has my input reached the page"
 * - the question a driver cannot answer any other way, and the one whose
 * answer was lost in the FF150 port. Zeroing the id after notifying is what
 * makes it once: a nested HandleEvent on a child shell sees 0 and stays quiet.
 * Not before the dispatch: the observer runs JavaScript synchronously, and a
 * promise resumed from it would read the landing before the listeners below
 * had recorded it.
 */
class MOZ_RAII StealthJugglerMouseAck final {
 public:
  // Reads the id the event carries now (0 when it is not a mouse event).
  explicit StealthJugglerMouseAck(WidgetGUIEvent* aGUIEvent)
      : mGUIEvent(aGUIEvent),
        mJugglerEventId(aGUIEvent->AsMouseEvent()
                            ? aGUIEvent->AsMouseEvent()->mJugglerEventId
                            : 0) {}

  // Does the ack, if the event still carries the id read in the constructor.
  ~StealthJugglerMouseAck() {
    if (!mJugglerEventId || !mGUIEvent->AsMouseEvent() ||
        mGUIEvent->AsMouseEvent()->mJugglerEventId != mJugglerEventId) {
      return;
    }
    const char* type = nullptr;
    switch (mGUIEvent->mMessage) {
      case eMouseMove:
        type = "mousemove";
        break;
      case eMouseDown:
        type = "mousedown";
        break;
      case eMouseUp:
        type = "mouseup";
        break;
      // [B230]: a right press sends `mousedown` and then `contextmenu`, and
      // `Page.dispatchMouseEvent` answers once the LAST event it sent is
      // handled. Without this case that last event was never acked, and every
      // right click waited out its bound.
      case eContextMenu:
        type = "contextmenu";
        break;
      default:
        return;
    }
    mGUIEvent->AsMouseEvent()->mJugglerEventId = 0;
    if (nsCOMPtr<nsIObserverService> os =
            mozilla::services::GetObserverService()) {
      nsAutoString info;
      info.AppendASCII(type);
      info.Append(' ');
      info.AppendInt(mJugglerEventId);
      os->NotifyObservers(nullptr, "juggler-mouse-event-hit-renderer",
                          info.get());
    }
  }

  StealthJugglerMouseAck(const StealthJugglerMouseAck&) = delete;
  StealthJugglerMouseAck& operator=(const StealthJugglerMouseAck&) = delete;

 private:
  WidgetGUIEvent* const mGUIEvent;
  const uint32_t mJugglerEventId;
};

}  // namespace mozilla

#endif  // mozilla_StealthJugglerAck_h
