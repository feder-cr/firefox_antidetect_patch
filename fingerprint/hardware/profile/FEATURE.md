# fingerprint/hardware/profile

Category: spoof
Prefs: zoom.stealth.hw_concurrency, zoom.stealth.max_touch_points
Test: TODO

## What it does

TODO: one paragraph, from the notes below.

## Hooks in Mozilla files

- `dom/base/Navigator.cpp` (Navigator::HardwareConcurrency; Navigator::MaxTouchPoints, already-hook): hardwareConcurrency returns the pref when it is > 0, with no engine gate. maxTouchPoints returns the pref only when the engine is active and the caller is not System, after StealthAssertDeclarationsComplete(). Both read StaticPrefs directly from Mozilla code; each could become a one-line call into our file. The two gate differently (one checks the engine, the other does not).
