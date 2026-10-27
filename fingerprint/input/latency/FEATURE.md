# fingerprint/input/latency

Category: spoof
Prefs: none read directly
Test: TODO

## What it does

TODO: one paragraph, from the notes below.

## Hooks in Mozilla files

- `dom/webidl/Window.webidl` (dictionary SynthesizeMouseEventData, already-hook): Two new members of a chrome-only dictionary. jugglerEventId is Juggler plumbing (consumed at nsDOMWindowUtils.cpp:762 and nsContentUtils.cpp:10517). latency (ms before dispatch) sets the event birth time: nsDOMWindowUtils.cpp:765 stores it in mLatency and :897-898 uses nsContentUtils::InputEventBirth(aLatency) for the timestamp, which a page reads as event.timeStamp. [verified: the dictionary is r
