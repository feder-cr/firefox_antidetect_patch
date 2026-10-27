# automation/input

Category: juggler
Prefs: none read directly
Test: TODO

## What it does

TODO: one paragraph, from the notes below.

## Hooks in Mozilla files

- `dom/base/nsDOMWindowUtils.cpp` (nsDOMWindowUtils::JugglerSendMouseEvent (new); nsDOMWindowUtils::JugglerSendDragEvent (new); nsDOMWindowUtils::SendWheelEvent; CanvasToDataSourceSurface, movable): Four pieces in one file. (1) juggler: JugglerSendMouseEvent (new, about 45 lines) with a plain static counter sJugglerEventId. (2) juggler: JugglerSendDragEvent (new, about 60 lines) dispatches WidgetDragEvent directly and sets the drag session action from the modifier keys (B212). (3) spoof, input-event-birth: SendWheelEvent gets aLatency and sets mTimeStamp (same hunk as the signature change). (
- `dom/events/EventStateManager.cpp` (EventStateManager::BeginTrackingDragGesture; EventStateManager::GenerateDragGesture, already-hook): B288: a drag gesture started by a Juggler press continues only with Juggler moves, so native moves during a Juggler press no longer start a native drag. The early return skips only the gesture update; the move is still dispatched (per the comment). Relies on WidgetMouseEvent::mJugglerEventId, defined outside this group (MouseEvents.h, not checked). VERIFIED: WidgetMouseEvent::mJugglerEventId is de
- `dom/events/EventStateManager.h` (EventStateManager (members), already-hook): One member with a comment. It is the Mozilla-side state of the feature and cannot move out of the class.
- `dom/interfaces/base/nsIDOMWindowUtils.idl` (nsIDOMWindowUtils: sendWheelEvent, jugglerSendMouseEvent (new), jugglerSendDragEvent (new), already-hook): Declarations only; parameter lines only, so inline logic is 0. Adds an optional aLatency to sendWheelEvent, a new jugglerSendMouseEvent (14 inputs, returns the event id) and a new jugglerSendDragEvent. The jugglerSendMouseEvent doc comment names 'Playwright' and an issue number; the repo rule forbids third-party or test names in comments, so reword it before commit. The comment also says 'restored
- `dom/ipc/CoalescedMouseData.cpp` (CoalescedMouseData::Coalesce, already-hook): Carries the last folded event's Juggler id onto the coalesced event so a caller waiting for its move sees it land (B217).
- `layout/base/PresShell.cpp` (PresShell::HandleEvent (scope-exit lambda ackJugglerEvent), movable): After the event is handled, notifies juggler-mouse-event-hit-renderer with 'type id' (mousemove, mousedown, mouseup, contextmenu) and clears mJugglerEventId so it fires once. The ~32-line lambda is movable behind one line here. Open questions CHECKED: WidgetMouseEvent::mJugglerEventId exists at widget/MouseEvents.h:249 (that file is in the modified list); MakeScopeExit is already available via #in
- `widget/MouseEvents.h` (class WidgetMouseEventBase: member declaration (hunk 1) and its Assign copy (hunk 2), already-hook): New field in a core event struct that crosses IPC; nsGUIEventIPC.h serializes it (see that entry). The comment refers to a lost ack bug (B217). Both sides are built together, so no version skew handling is needed.
- `widget/nsGUIEventIPC.h` (struct ParamTraits<mozilla::WidgetMouseEventBase>: the field is written after mInputSource (hunk 1) and read in ReadForDerivedClass (hunk 2), already-hook): IPC serialization of mJugglerEventId, read in the same order it is written. Wire format change, so the reader and writer must stay in step; both sides are built together.
- `xpcom/reflect/xptinfo/xptinfo.h` (PARAM_BUFFER_COUNT macro, already-hook): Not a Firefox version fix: the diff comment says the count moves to 15 for jugglerSendMouseEvent, which takes 15 arguments. It is tied to that Juggler method's signature; if the method's arity changes, this value must follow. Check of the 15-argument claim: nsIDOMWindowUtils.idl:465-478 declares 14 in-parameters plus an unsigned long return value, i.e. 15 XPT params including the retval; the diff 
