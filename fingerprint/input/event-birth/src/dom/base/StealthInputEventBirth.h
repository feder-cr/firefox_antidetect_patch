/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_StealthInputEventBirth_h
#define mozilla_StealthInputEventBirth_h

#include "mozilla/Assertions.h"
#include "mozilla/BasicEvents.h"  // WidgetEventTime
#include "mozilla/TimeStamp.h"
#include "nsError.h"

namespace mozilla {

/**
 * Stealthfox [B299] [B301]: the birth of a synthesized input event that
 * reaches the page aLatencyMs after it was born, as a system's does; never
 * before the previous such birth, mouse or wheel.
 *
 * An inline function: the C++17 rule gives its function-local static ONE
 * instance in the whole binary, so the mouse path (nsContentUtils) and the
 * wheel path (nsDOMWindowUtils) share the same previous birth. No lock, as
 * before.
 */
inline TimeStamp StealthInputEventBirth(double aLatencyMs) {
  // Stealthfox [B299] [B301]: an input event is born in the system a few
  // milliseconds before a page's handler runs, and a page can tell: its
  // `timeStamp` is below `performance.now()` there. A synthesized one was born
  // at its dispatch, so a tracker that keeps only events from the past dropped
  // them. The caller's latency is taken off the birth, and never past the
  // birth of the previous one, mouse or wheel alike, so the times stay in
  // order as a system's do.
  static TimeStamp sLastBorn;
  TimeStamp born =
      TimeStamp::Now() - TimeDuration::FromMilliseconds(aLatencyMs);
  if (!sLastBorn.IsNull() && born < sLastBorn) {
    born = sLastBorn;
  }
  sLastBorn = born;
  return born;
}

/**
 * Stealthfox [B299] [B301]: born before the page sees it. Stamp aEvent with
 * the birth for aLatencyMs when the caller declared a latency. Works for the
 * mouse and the wheel event alike (the wheel as the mouse): mTimeStamp lives
 * on their common base. A latency of 0 (or less, or NaN) leaves the stamp the
 * event got at its construction.
 */
inline void StealthSetInputEventBirth(WidgetEventTime& aEvent,
                                      double aLatencyMs) {
  if (aLatencyMs > 0) {
    aEvent.mTimeStamp = StealthInputEventBirth(aLatencyMs);
  }
}

/**
 * Stealthfox [B299]: the birth of a key event the caller sent with a latency,
 * for nsITextInputProcessor (chrome only; nothing is added to the web-exposed
 * KeyboardEventInit). A null stamp when no latency was declared (0, less, or
 * NaN): the event then keeps the stamp it got at its construction.
 *
 * The cap of an hour is not a latency a system has: past it the number is a
 * mistake of the caller, not a birth. It also keeps the conversion of the
 * milliseconds to the ticks of a TimeDuration (a 64-bit count) far from an
 * overflow, which an infinite or huge value would reach.
 */
inline TimeStamp StealthInputEventBirthOrNull(double aLatencyMs) {
  return aLatencyMs > 0 && aLatencyMs < 3600000
             ? StealthInputEventBirth(aLatencyMs)
             : TimeStamp();
}

/**
 * Stealthfox [B299]: the birth of the key that nsITextInputProcessor is
 * delivering. The caller declares the latency of the NEXT key with
 * setNextKeyLatency() (stored in the processor, mStealthNextKeyLatencyMs);
 * keydown(), keyup() and commitCompositionWith() each open this scope, which
 * takes the declared latency (one use: it is cleared) and holds the birth for
 * the duration of the call. KeydownInternal() and KeyupInternal() stamp their
 * copy of the key event through Stamp(); TextEventDispatcher copies the stamp
 * with AssignKeyEventData, and the keypress events are copies of the keydown,
 * so every key event of the call carries the birth. With no latency declared the
 * scope holds no birth and Stamp() does nothing: the event keeps the stamp of
 * its construction. The scopes nest (a restore on exit); the processor lives
 * on the main thread only, as the births of the mouse and the wheel do.
 *
 * Known limit: on commitCompositionWith() the keydown and keyup carry the
 * birth, but the composition events and the input event that follow keep the
 * time of the delivery, as they did with the old timer. They are created by
 * TextEventDispatcher::CommitComposition(), whose call in
 * TextInputProcessor.cpp is a line of Mozilla's and takes no stamp from here.
 */
class StealthKeyBirthScope {
 public:
  explicit StealthKeyBirthScope(double& aNextLatencyMs) : mPrevious(Current()) {
    Current() = StealthInputEventBirthOrNull(aNextLatencyMs);
    aNextLatencyMs = 0;
  }
  ~StealthKeyBirthScope() { Current() = mPrevious; }
  StealthKeyBirthScope(const StealthKeyBirthScope&) = delete;
  StealthKeyBirthScope& operator=(const StealthKeyBirthScope&) = delete;

  static void Stamp(WidgetEventTime& aEvent) {
    if (!Current().IsNull()) {
      aEvent.mTimeStamp = Current();
    }
  }

 private:
  static TimeStamp& Current() {
    static TimeStamp sBorn;
    return sBorn;
  }
  TimeStamp mPrevious;
};

/**
 * Stealthfox [B299]: nsITextInputProcessor.setNextKeyLatency(). Chrome only
 * (as the rest of the interface); stores the latency of the next key in the
 * processor's field, to be taken by the scope above.
 */
inline nsresult StealthSetNextKeyLatency(double& aField, double aLatencyMs,
                                         bool aIsCallerChrome) {
  MOZ_RELEASE_ASSERT(aIsCallerChrome);
  aField = aLatencyMs;
  return NS_OK;
}

}  // namespace mozilla

#endif  // mozilla_StealthInputEventBirth_h
