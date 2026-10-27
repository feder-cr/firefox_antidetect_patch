# fingerprint/locale-time/session-declaration

Category: build
Prefs: none read directly
Test: TODO

## What it does

TODO: one paragraph, from the notes below.

## Hooks in Mozilla files

- `dom/base/moz.build` (EXPORTS.mozilla.dom and UNIFIED_SOURCES lists, already-hook): Registers two list entries (not logic, so inline count is 0). StealthSessionDeclaration.h and .cpp are new files that are not in the inventory list. StealthDeclarationGate.h is included by several changed files but is not registered in this hunk; its registration was not checked. VERIFIED: StealthDeclarationGate.h/.cpp are registered in gfx/thebes/moz.build:68 and :245, so nothing is missing here.

## Our own files

- `dom/base/StealthSessionDeclaration.cpp`
- `dom/base/StealthSessionDeclaration.h`
- `intl/locale/StealthSessionPrefs.cpp`
- `intl/locale/StealthSessionPrefs.h`
