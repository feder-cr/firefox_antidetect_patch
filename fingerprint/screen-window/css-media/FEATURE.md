# fingerprint/screen-window/css-media

Category: spoof
Prefs: zoom.stealth.pointer.all, zoom.stealth.pointer.primary, zoom.stealth.screen.height, zoom.stealth.screen.width
Test: TODO

## What it does

TODO: one paragraph, from the notes below.

## Hooks in Mozilla files

- `layout/style/nsMediaFeatures.cpp` (GetDeviceSize (device-size block) and GetPointerCapabilities (stealth branch), movable): Two declared sub-features under one file. Device size: returns the declared width/height when both prefs are above 0, with no StealthEngineActive gate (the pointer branch has one: inconsistent, CONFIRMED from the diff); the same ungated pattern is used in dom/base/nsScreen.cpp:93-96, so it is the house pattern for these two prefs rather than a one-off. Pointer capabilities: declared primary and un
