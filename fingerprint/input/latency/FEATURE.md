# fingerprint/input/latency

Category: spoof
Prefs: none read directly
Test: TODO

## What it does

TODO: one paragraph, from the notes below.

## Hooks in Mozilla files

- `dom/webidl/Window.webidl` (dictionary SynthesizeMouseEventData, already-hook): Two new members of a chrome-only dictionary. jugglerEventId is Juggler plumbing (consumed at nsDOMWindowUtils.cpp:762 and nsContentUtils.cpp:10517). latency (ms before dispatch) sets the event birth time: nsDOMWindowUtils.cpp:765 stores it in mLatency and :897-898 uses nsContentUtils::InputEventBirth(aLatency) for the timestamp, which a page reads as event.timeStamp. [verified: the dictionary is r

## dom/webidl/Window.webidl: dictionary SynthesizeMouseEventData (jugglerEventId, latency)

Two members added at the end of the chrome-only dictionary SynthesizeMouseEventData.

jugglerEventId (unsigned long, default 0): Juggler event identifier for tracking events across processes.

latency (double, default 0): Stealthfox [B299]: how many milliseconds before its dispatch the event was
born, as an input event is born in the system before a page's handler
runs. 0 is born now.
