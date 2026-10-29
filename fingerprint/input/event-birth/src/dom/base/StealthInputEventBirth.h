/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_StealthInputEventBirth_h
#define mozilla_StealthInputEventBirth_h

#include "mozilla/BasicEvents.h"  // WidgetEventTime
#include "mozilla/TimeStamp.h"

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

}  // namespace mozilla

#endif  // mozilla_StealthInputEventBirth_h
