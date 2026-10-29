/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// The properties of the synthesized input event birth ([B299] [B301]) and of
// the Juggler id that rides on a synthesized mouse event ([B217]).

#include <limits>

#include "StealthInputEventBirth.h"
#include "StealthJugglerEventId.h"
#include "gtest/gtest.h"
#include "mozilla/BasicEvents.h"
#include "mozilla/MouseEvents.h"
#include "mozilla/TimeStamp.h"

using namespace mozilla;

TEST(StealthInputEventBirth, NoLatencyKeepsTheConstructionStamp)
{
  WidgetEventTime event;
  const TimeStamp before = event.mTimeStamp;
  StealthSetInputEventBirth(event, 0);
  EXPECT_TRUE(event.mTimeStamp == before);
  StealthSetInputEventBirth(event, -5);
  EXPECT_TRUE(event.mTimeStamp == before);
  StealthSetInputEventBirth(event, std::numeric_limits<double>::quiet_NaN());
  EXPECT_TRUE(event.mTimeStamp == before);
}

TEST(StealthInputEventBirth, BirthIsInThePastAndNotBeforeTheLatency)
{
  const TimeStamp before = TimeStamp::Now();
  WidgetEventTime event;
  StealthSetInputEventBirth(event, 20);
  EXPECT_TRUE(event.mTimeStamp <= TimeStamp::Now());
  EXPECT_TRUE(event.mTimeStamp >= before - TimeDuration::FromMilliseconds(20));
}

TEST(StealthInputEventBirth, NeverPastThePreviousBirth)
{
  // A short latency, then a long one: the second would be born before the
  // first, so it takes the first's birth.
  WidgetEventTime first, second;
  StealthSetInputEventBirth(first, 10);
  StealthSetInputEventBirth(second, 5000);
  EXPECT_TRUE(second.mTimeStamp == first.mTimeStamp);

  // Whatever the latencies, the births never go back.
  TimeStamp last = second.mTimeStamp;
  for (double latency : {30.0, 2.0, 400.0, 1.0, 90.0, 7.0}) {
    WidgetEventTime event;
    StealthSetInputEventBirth(event, latency);
    EXPECT_TRUE(event.mTimeStamp >= last);
    last = event.mTimeStamp;
  }
}

TEST(StealthJugglerEventId, TravelsOnTheMouseEvent)
{
  WidgetMouseEvent event(true, eMouseDown, nullptr, WidgetMouseEvent::eReal);
  EXPECT_EQ(event.mJugglerEventId, 0u);
  StealthSetJugglerId(event, 7);
  EXPECT_EQ(event.mJugglerEventId, 7u);
}
