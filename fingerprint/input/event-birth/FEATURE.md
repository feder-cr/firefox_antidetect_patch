# fingerprint/input/event-birth

Category: spoof
Prefs: none read directly
Test: TODO

## What it does

TODO: one paragraph, from the notes below.

## Hooks in Mozilla files

- `dom/base/nsContentUtils.cpp` (nsContentUtils::InputEventBirth (new); nsContentUtils::SynthesizeMouseEvent, movable): Two concerns. (1) spoof, input-event-birth: InputEventBirth moves a synthetic event's timeStamp back by the caller's latency and keeps births monotonic through the function-static sLastBorn (no lock; not verified that SynthesizeMouseEvent always runs on the main thread). The function is about 6 logic lines and can move to our file, leaving only the declaration in nsContentUtils.h. (2) juggler, jug
- `dom/base/nsContentUtils.h` (nsContentUtils (static declarations), already-hook): Declaration plus doc comment. If the function moves to our own file, this declaration goes away and the Mozilla header has no change for this feature.
