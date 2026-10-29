# fingerprint/hardware/concurrency

Category: spoof
Prefs: zoom.stealth.hw_concurrency
Test: TODO

## What it does

TODO: one paragraph, from the notes below.

## Hooks in Mozilla files

- `dom/workers/WorkerNavigator.cpp` (WorkerNavigator::HardwareConcurrency, already-hook): Returns the pref when > 0, before the RuntimeService value. [verified: CONFIRMED, the main-thread counterpart Navigator::HardwareConcurrency reads the same pref at dom/base/Navigator.cpp:702.] The include is added out of alphabetical order (cosmetic).

## dom/workers/WorkerNavigator.cpp: WorkerNavigator::HardwareConcurrency

Stealth: return spoofed core count if pref is set (zoom.stealth.hw_concurrency, read through StaticPrefs; when > 0 it is returned before the RuntimeService value).
