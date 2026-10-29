/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef StealthJugglerInput_h
#define StealthJugglerInput_h

// Members of nsDOMWindowUtils for the Juggler protocol: a mouse event with an
// event id, and a drag event. They are member definitions because they need
// the protected GetWidget and GetPresShell. This header is included by
// dom/base/nsDOMWindowUtils.cpp and by nothing else; a second includer would
// define the members twice and fail to link.

#include "Units.h"
#include "mozilla/EventForwards.h"
#include "mozilla/MouseEvents.h"
#include "mozilla/PresShell.h"
#include "mozilla/RefPtr.h"
#include "mozilla/Result.h"
#include "mozilla/dom/BindingDeclarations.h"
#include "mozilla/dom/FunctionBinding.h"
#include "mozilla/dom/MouseEventBinding.h"
#include "mozilla/dom/WindowBinding.h"
#include "nsCOMPtr.h"
#include "nsContentUtils.h"
#include "nsDOMWindowUtils.h"
#include "nsError.h"
// Stealthfox [B212]: jugglerSendDragEvent sets the session's drag action from
// the modifier state, the way nsNativeDragTarget::ProcessDrag does.
#include "nsIDragService.h"
#include "nsIDragSession.h"
#include "nsIWidget.h"
#include "nsPoint.h"
#include "nsServiceManagerUtils.h"
#include "nsString.h"

// Playwright: dispatch a synthetic mouse event and return a monotonically
// incrementing event identifier. Restored from the FF146 Juggler patch;
// dropped during the FF150 port and now re-added for issue #9.
//
// The id is stored on the event itself as WidgetMouseEventBase::mJugglerEventId
// (widget/MouseEvents.h, see automation/input/FEATURE.md).
NS_IMETHODIMP
nsDOMWindowUtils::JugglerSendMouseEvent(
    const nsAString& aType, float aX, float aY, int32_t aButton,
    int32_t aClickCount, int32_t aModifiers, bool aIgnoreRootScrollFrame,
    float aPressure, unsigned short aInputSourceArg,
    bool aIsDOMEventSynthesized, bool aIsWidgetEventSynthesized,
    int32_t aButtons, uint32_t aIdentifier, double aLatency,
    uint32_t* aJugglerEventId) {
  static uint32_t sJugglerEventId = 0;
  *aJugglerEventId = ++sJugglerEventId;

  // Stealthfox [B299]: aLatency took the place of aDisablePointerEvent, which
  // this method accepted for FF146's signature and ignored (FF150's
  // SynthesizeMouseEvent gates pointer dispatch on the event type). XPConnect
  // takes 15 arguments at most, and a 16th failed the build.

  RefPtr<mozilla::PresShell> presShell = GetPresShell();
  if (!presShell) {
    return NS_ERROR_FAILURE;
  }

  nsPoint offset;
  nsCOMPtr<nsIWidget> widget = GetWidget(&offset);
  if (!widget) {
    return NS_ERROR_FAILURE;
  }

  mozilla::LayoutDeviceIntPoint refPoint = nsContentUtils::ToWidgetPoint(
      mozilla::CSSPoint(aX, aY), offset, presShell->GetPresContext());

  mozilla::dom::SynthesizeMouseEventData mouseEventData;
  mouseEventData.mIdentifier = aIdentifier;
  mouseEventData.mButton = aButton;
  mouseEventData.mButtons.Construct(aButtons);
  mouseEventData.mClickCount.Construct(aClickCount);
  mouseEventData.mPressure.Construct(aPressure);
  mouseEventData.mInputSource = aInputSourceArg;
  mouseEventData.mModifiers = aModifiers;
  mouseEventData.mJugglerEventId = *aJugglerEventId;
  // Stealthfox [B299]: born that many milliseconds before its dispatch; see
  // nsContentUtils::SynthesizeMouseEvent.
  mouseEventData.mLatency = aLatency;

  mozilla::dom::SynthesizeMouseEventOptions options;
  options.mIgnoreRootScrollFrame = aIgnoreRootScrollFrame;
  options.mIsDOMEventSynthesized = aIsDOMEventSynthesized;
  options.mIsWidgetEventSynthesized = aIsWidgetEventSynthesized;
  options.mIsAsyncEnabled = false;

  auto result = nsContentUtils::SynthesizeMouseEvent(
      presShell, widget, aType, refPoint, mouseEventData, options,
      mozilla::dom::Optional<
          mozilla::OwningNonNull<mozilla::dom::VoidFunction>>{});
  return result.isOk() ? NS_OK : result.unwrapErr();
}

// Stealthfox [B212]: a drag event needs its own door.
//
// jugglerSendDragEvent dispatches a synthetic DRAG event the way the
// platform's own drop target does.
//
// This is deliberately NOT a mode of jugglerSendMouseEvent. A drag event is a
// WidgetDragEvent, not a WidgetMouseEvent, and routing it through the mouse
// synthesizer is what made it impossible: nsContentUtils::
// SynthesizeMouseEvent maps ten mouse type strings and returns
// NS_ERROR_FAILURE for everything else, so "dragover" could never be
// dispatched at all - that is [B212], where the drop never reached the
// target and drag_and_drop raised instead.
//
// The implementation mirrors nsNativeDragTarget::DispatchDragDropEvent, which
// is the path a real OS drag takes, rather than the mIsSynthesizedForTests
// path EventStateManager also offers: we want the engine on the same code path
// a retail Firefox runs, for the same reason inputSource moved from 0 to
// MOZ_SOURCE_MOUSE.
//
// It used to go through jugglerSendMouseEvent, and that could never work:
// nsContentUtils::SynthesizeMouseEvent maps ten mouse type strings and ends
// with `return Err(NS_ERROR_FAILURE)`, so "dragover" failed on EVERY call, on
// every page. The drop never reached the target and the whole gesture raised.
//
// This mirrors nsNativeDragTarget::DispatchDragDropEvent - the path a real OS
// drag takes on Windows - and NOT the mIsSynthesizedForTests path that
// EventStateManager also accepts. Both would deliver the event; only one puts
// the engine on the code path a retail Firefox runs, and that is the same
// reason inputSource moved from 0 to MOZ_SOURCE_MOUSE.
NS_IMETHODIMP
nsDOMWindowUtils::JugglerSendDragEvent(const nsAString& aType, float aX,
                                       float aY, int32_t aModifiers) {
  mozilla::EventMessage msg;
  if (aType.EqualsLiteral("dragover")) {
    msg = mozilla::eDragOver;
  } else if (aType.EqualsLiteral("drop")) {
    msg = mozilla::eDrop;
  } else if (aType.EqualsLiteral("dragenter")) {
    msg = mozilla::eDragEnter;
  } else if (aType.EqualsLiteral("dragexit")) {
    msg = mozilla::eDragExit;
  } else {
    // Named refusal: the caller asked for something this door does not open.
    // Returning NS_ERROR_FAILURE here would reproduce the very defect above,
    // where a wrong type was indistinguishable from a broken engine.
    return NS_ERROR_INVALID_ARG;
  }

  RefPtr<mozilla::PresShell> presShell = GetPresShell();
  if (!presShell) {
    return NS_ERROR_FAILURE;
  }

  nsPoint offset;
  nsCOMPtr<nsIWidget> widget = GetWidget(&offset);
  if (!widget) {
    return NS_ERROR_FAILURE;
  }

  mozilla::WidgetDragEvent event(true, msg, widget);
  event.mRefPoint = nsContentUtils::ToWidgetPoint(
      mozilla::CSSPoint(aX, aY), offset, presShell->GetPresContext());
  event.mModifiers = nsContentUtils::GetWidgetModifiers(aModifiers);
  // A real drag carries the input source of the session that started it; ours
  // is always a mouse. Leaving this at the default would reintroduce the tell
  // that P2.4b removed from the mouse path.
  event.mInputSource = mozilla::dom::MouseEvent_Binding::MOZ_SOURCE_MOUSE;

  // The real drop target sets the session's action from the modifier state
  // before dispatching (nsNativeDragTarget::ProcessDrag ->
  // GetGeckoDragAction): Ctrl+Shift LINK, Shift MOVE, Ctrl COPY. Without this
  // the session keeps whatever action it was left with, and the dropEffect the
  // page reads stops following the keys the user is holding.
  if (nsCOMPtr<nsIDragService> dragService =
          do_GetService("@mozilla.org/widget/dragservice;1")) {
    if (nsCOMPtr<nsIDragSession> session =
            dragService->GetCurrentSession(widget)) {
      uint32_t action = nsIDragService::DRAGDROP_ACTION_MOVE;
      if (event.IsControl()) {
        action = event.IsShift() ? nsIDragService::DRAGDROP_ACTION_LINK
                                 : nsIDragService::DRAGDROP_ACTION_COPY;
      }
      session->SetDragAction(action);
    }
  }

  // Whether the page called preventDefault is NOT reported back: the drop
  // decision is the page's, and the caller learns it by watching for the
  // event, which is what PageHandler already does. Returning it here would be
  // a second source for the same fact.
  widget->DispatchEvent(&event);
  return NS_OK;
}

#endif  // StealthJugglerInput_h
