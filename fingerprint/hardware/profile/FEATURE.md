# fingerprint/hardware/profile

Category: spoof
Prefs: zoom.stealth.hw_concurrency, zoom.stealth.max_touch_points
Test: TODO

## What it does

TODO: one paragraph, from the notes below.

## Hooks in Mozilla files

- `dom/base/Navigator.cpp` (Navigator::HardwareConcurrency; Navigator::MaxTouchPoints, already-hook): hardwareConcurrency returns the pref when it is > 0, with no engine gate. maxTouchPoints returns the pref only when the engine is active and the caller is not System, after StealthAssertDeclarationsComplete(). Both read StaticPrefs directly from Mozilla code; each could become a one-line call into our file. The two gate differently (one checks the engine, the other does not).

## dom/base/Navigator.cpp: Navigator::HardwareConcurrency

Stealth: return spoofed core count if pref is set (zoom.stealth.hw_concurrency, when > 0;
the check sits before the worker-count lookup).

## dom/base/Navigator.cpp: Navigator::MaxTouchPoints

Stealth: the DECLARED touch point count. This used to be a literal 0,
which is the right value for the desktop GPU personas we ship but the
wrong shape: a constant buried in C++ is not pinnable, not inspectable and
not overridable through extra_prefs, so a touchscreen persona (a Surface,
a 2-in-1) would have needed a browser rebuild. It is now
HardwareProfile.max_touch_points, at the same level as every other surface
(rule 6). The pref defaults to -1, so a build launched without
invisible_core keeps exactly the previous behaviour.

Inside the hook's block (before StealthAssertDeclarationsComplete): No compiled floor
(engine rule 7, 2026-08-09). The "? ... : 0" that used to close the return line was a
second answer for a declared field, and at the call site a wrong one is indistinguishable
from a right one. A missing declaration refuses in StealthDeclarationGate rather than
resolving in MaxTouchPoints.
