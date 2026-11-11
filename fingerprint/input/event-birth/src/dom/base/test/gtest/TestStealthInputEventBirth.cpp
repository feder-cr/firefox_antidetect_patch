/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// The properties of the synthesized input event birth ([B299] [B301]) and of
// the Juggler id that rides on a synthesized mouse event ([B217]), the flag a Juggler event does not carry on the
// asynchronous route (APZ) and the data of the notification of a crashed frame.

#include <limits>

#include "StealthFrameCrash.h"
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

// The asynchronous (APZ) route marks an event "async synthesized for tests", and nsBaseDragService then sends a drag
// started on it to the native drag loop. A Juggler event (it has an id) must come out of the route without the flag; every
// other event keeps what the route gave it.
TEST(StealthJugglerEventId, ADragStartedByAJugglerEventIsNotAsyncSynthesized)
{
  WidgetMouseEvent juggler(true, eMouseDown, nullptr, WidgetMouseEvent::eReal);
  StealthSetJugglerId(juggler, 7);
  juggler.mFlags.mIsSynthesizedForTests = true;
  juggler.mFlags.mIsAsyncSynthesizedForTests = true;
  StealthJugglerNotAsyncSynthesized(juggler);
  EXPECT_FALSE(juggler.mFlags.mIsAsyncSynthesizedForTests);
  // still a synthesized event: the drag session of the content process stays its own (no native loop)
  EXPECT_TRUE(juggler.mFlags.mIsSynthesizedForTests);
}

TEST(StealthJugglerEventId, AnEventWithNoIdKeepsTheFlagOfTheRoute)
{
  WidgetMouseEvent other(true, eMouseDown, nullptr, WidgetMouseEvent::eReal);
  EXPECT_EQ(other.mJugglerEventId, 0u);
  other.mFlags.mIsSynthesizedForTests = true;
  other.mFlags.mIsAsyncSynthesizedForTests = true;
  StealthJugglerNotAsyncSynthesized(other);
  EXPECT_TRUE(other.mFlags.mIsAsyncSynthesizedForTests);
  EXPECT_TRUE(other.mFlags.mIsSynthesizedForTests);
}

// The data of oop-frameloader-crashed is the id of the browsing context that crashed, as digits; none when the caller
// does not say which one (a frameloader that failed to build: the observer takes that for the tab).
TEST(StealthFrameCrash, NoContextIsNoData)
{
  nsAutoString buffer;
  EXPECT_EQ(StealthCrashedContextData(static_cast<dom::BrowsingContext*>(nullptr), buffer), nullptr);
  EXPECT_TRUE(buffer.IsEmpty());
}

TEST(StealthFrameCrash, TheIdIsTheDigits)
{
  nsAutoString buffer;
  // an id of the kind the engine makes: the process in the high word, the counter in the low one
  const char16_t* data = StealthCrashedContextData(uint64_t(15032385537), buffer);
  ASSERT_NE(data, nullptr);
  EXPECT_TRUE(nsDependentString(data).EqualsLiteral("15032385537"));
  // the buffer holds the digits for as long as the notification runs, and a second id does not append to the first
  data = StealthCrashedContextData(uint64_t(9), buffer);
  EXPECT_TRUE(nsDependentString(data).EqualsLiteral("9"));
  EXPECT_EQ(data, buffer.get());
}
