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
#include "mozilla/TextEvents.h"
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

// The born-in-the-past time of a key ([B299]): nsITextInputProcessor stamps its copy of the key event with
// StealthInputEventBirthOrNull(latency) before TextEventDispatcher copies it again with AssignKeyEventData. The
// processor itself needs a window and a widget, which a gtest has none of; what these tests hold is the two pieces
// it is made of: the birth the latency gives, and a stamp that the copies of a WidgetKeyboardEvent keep.
TEST(StealthInputEventBirth, KeyNoLatencyIsNoBirth)
{
  EXPECT_TRUE(StealthInputEventBirthOrNull(0).IsNull());
  EXPECT_TRUE(StealthInputEventBirthOrNull(-5).IsNull());
  EXPECT_TRUE(StealthInputEventBirthOrNull(std::numeric_limits<double>::quiet_NaN()).IsNull());
  // a latency no system has (past an hour, infinite) is no birth either, and does not reach the tick conversion
  EXPECT_TRUE(StealthInputEventBirthOrNull(3600000).IsNull());
  EXPECT_TRUE(StealthInputEventBirthOrNull(std::numeric_limits<double>::infinity()).IsNull());
}

TEST(StealthInputEventBirth, KeyLatencyIsBornInThePast)
{
  const TimeStamp before = TimeStamp::Now();
  const TimeStamp born = StealthInputEventBirthOrNull(35);
  ASSERT_FALSE(born.IsNull());
  EXPECT_TRUE(born <= TimeStamp::Now());
  EXPECT_TRUE(born >= before - TimeDuration::FromMilliseconds(35));
}

TEST(StealthInputEventBirth, TheStampOfAKeyEventSurvivesTheCopiesOfTheProcessor)
{
  // KeydownInternal copies the event and stamps the copy; TextEventDispatcher builds the event that is dispatched with
  // AssignKeyEventData (and the keypress is a copy of the copy). The stamp must come through all of them.
  WidgetKeyboardEvent original(true, eKeyDown, nullptr);
  const TimeStamp born = original.mTimeStamp - TimeDuration::FromMilliseconds(50);

  WidgetKeyboardEvent copy(original);
  copy.mTimeStamp = born;

  WidgetKeyboardEvent dispatched(true, eKeyDown, nullptr);
  dispatched.AssignKeyEventData(copy, false);
  EXPECT_TRUE(dispatched.mTimeStamp == born);

  WidgetKeyboardEvent keypress(copy);
  keypress.mMessage = eKeyPress;
  EXPECT_TRUE(keypress.mTimeStamp == born);

  // the original was not touched: the processor never modifies the event it is given
  EXPECT_TRUE(original.mTimeStamp != born);
}

TEST(StealthInputEventBirth, KeysAndMouseShareTheOrderOfBirths)
{
  // a key born after a mouse event keeps the order of the system: never before the previous birth, of either kind
  WidgetEventTime mouse;
  StealthSetInputEventBirth(mouse, 5);
  const TimeStamp key = StealthInputEventBirthOrNull(4000);
  ASSERT_FALSE(key.IsNull());
  EXPECT_TRUE(key >= mouse.mTimeStamp);
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
  // char16ptr_t has no unambiguous == with const char16_t* on Windows (clang-cl): compare the plain pointers
  EXPECT_EQ(data, static_cast<const char16_t*>(buffer.get()));
}

// The scope that nsITextInputProcessor opens around keydown(), keyup() and commitCompositionWith() ([B299]): the latency
// declared with setNextKeyLatency() is taken once, and the key events stamped inside the scope are born in the past.
TEST(StealthKeyBirthScope, NothingDeclaredStampsNothing)
{
  double declared = 0;
  StealthKeyBirthScope scope(declared);
  WidgetKeyboardEvent event(true, eKeyDown, nullptr);
  const TimeStamp before = event.mTimeStamp;
  StealthKeyBirthScope::Stamp(event);
  EXPECT_TRUE(event.mTimeStamp == before);
}

TEST(StealthKeyBirthScope, OutsideAScopeStampsNothing)
{
  WidgetKeyboardEvent event(true, eKeyDown, nullptr);
  const TimeStamp before = event.mTimeStamp;
  StealthKeyBirthScope::Stamp(event);
  EXPECT_TRUE(event.mTimeStamp == before);
}

TEST(StealthKeyBirthScope, ADeclaredLatencyIsTakenOnceAndStampsTheEventsInside)
{
  double declared = 60;
  WidgetKeyboardEvent down(true, eKeyDown, nullptr);
  WidgetKeyboardEvent up(true, eKeyUp, nullptr);
  const TimeStamp now = down.mTimeStamp;
  {
    StealthKeyBirthScope scope(declared);
    EXPECT_EQ(declared, 0.0);  // used once: the next key declares its own
    StealthKeyBirthScope::Stamp(down);
    StealthKeyBirthScope::Stamp(up);
  }
  EXPECT_TRUE(down.mTimeStamp < now);
  EXPECT_TRUE(down.mTimeStamp >= now - TimeDuration::FromMilliseconds(61));
  // both events of the call carry the one birth
  EXPECT_TRUE(up.mTimeStamp == down.mTimeStamp);

  // the scope is closed: nothing is stamped any more, and a second scope with nothing declared stamps nothing
  WidgetKeyboardEvent after(true, eKeyDown, nullptr);
  const TimeStamp stamp = after.mTimeStamp;
  {
    StealthKeyBirthScope scope(declared);
    StealthKeyBirthScope::Stamp(after);
  }
  EXPECT_TRUE(after.mTimeStamp == stamp);
}

TEST(StealthKeyBirthScope, AScopeThatEndsRestoresTheOneAround)
{
  double outer = 80, inner = 0;
  StealthKeyBirthScope scope(outer);
  WidgetKeyboardEvent first(true, eKeyDown, nullptr);
  StealthKeyBirthScope::Stamp(first);
  {
    StealthKeyBirthScope nested(inner);  // nothing declared inside: no birth inside
    WidgetKeyboardEvent plain(true, eKeyDown, nullptr);
    const TimeStamp stamp = plain.mTimeStamp;
    StealthKeyBirthScope::Stamp(plain);
    EXPECT_TRUE(plain.mTimeStamp == stamp);
  }
  WidgetKeyboardEvent second(true, eKeyDown, nullptr);
  StealthKeyBirthScope::Stamp(second);
  EXPECT_TRUE(second.mTimeStamp == first.mTimeStamp);
}

TEST(StealthKeyBirthScope, ALatencyPastTheCapIsNoBirth)
{
  double declared = 3600000;
  StealthKeyBirthScope scope(declared);
  WidgetKeyboardEvent event(true, eKeyDown, nullptr);
  const TimeStamp before = event.mTimeStamp;
  StealthKeyBirthScope::Stamp(event);
  EXPECT_TRUE(event.mTimeStamp == before);
}

// A latency declared with setNextKeyLatency() and never followed by a key (the caller threw) is taken back by
// setNextKeyLatency(0) of its finally: the next key is not born in the past. The processor itself is not driven
// here (it needs a window and a widget), so this is the field and the scope it feeds.
TEST(StealthKeyBirthScope, ALatencyWithNoKeyThenZeroGivesNoBirthToTheNextKey)
{
  double field = 0;
  ASSERT_EQ(StealthSetNextKeyLatency(field, 60, true), NS_OK);
  EXPECT_EQ(field, 60.0);
  // no key call follows; the caller takes the latency back
  ASSERT_EQ(StealthSetNextKeyLatency(field, 0, true), NS_OK);
  EXPECT_EQ(field, 0.0);

  WidgetKeyboardEvent event(true, eKeyDown, nullptr);
  const TimeStamp before = event.mTimeStamp;
  {
    StealthKeyBirthScope scope(field);  // what keydown() opens
    StealthKeyBirthScope::Stamp(event);
  }
  EXPECT_TRUE(event.mTimeStamp == before);
}

TEST(StealthKeyBirthScope, TheScopeTakesTheLatencyEvenWhenTheCallEndsEarly)
{
  // the scope zeroes the field when it opens, before anything of the call can fail
  double field = 45;
  { StealthKeyBirthScope scope(field); }
  EXPECT_EQ(field, 0.0);
}
