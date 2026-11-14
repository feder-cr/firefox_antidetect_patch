# fingerprint/input/event-birth

Category: spoof
Prefs: none read directly
Test: TODO

## What it does

TODO: one paragraph, from the notes below.

## Hooks in Mozilla files

- `dom/base/TextInputProcessor.{h,cpp}`, `dom/interfaces/base/nsITextInputProcessor.idl`: a key is delivered at once and born
  the declared latency in the past. `setNextKeyLatency` (one idl line, chrome-only XPCOM) declares it for the next key; a
  `StealthKeyBirthScope` opened right after the IsCallerChrome assert of keydown, keyup and commitCompositionWith takes it and
  zeroes it in every case; KeydownInternal/KeyupInternal stamp the event's birth (StealthInputEventBirth.h). It replaces the
  chrome setTimeout of juggler/content/PageAgent.js, which under load moved the delivery itself (gaps and dwells the page saw
  were the timer's). Known limit: the composition and input events of commitCompositionWith keep their delivery time.
- `dom/base/nsContentUtils.cpp` (nsContentUtils::InputEventBirth (new); nsContentUtils::SynthesizeMouseEvent, movable): Two concerns. (1) spoof, input-event-birth: InputEventBirth moves a synthetic event's timeStamp back by the caller's latency and keeps births monotonic through the function-static sLastBorn (no lock; not verified that SynthesizeMouseEvent always runs on the main thread). The function is about 6 logic lines and can move to our file, leaving only the declaration in nsContentUtils.h. (2) juggler, jug
- `dom/base/nsContentUtils.h` (nsContentUtils (static declarations), already-hook): Declaration plus doc comment. If the function moves to our own file, this declaration goes away and the Mozilla header has no change for this feature.
