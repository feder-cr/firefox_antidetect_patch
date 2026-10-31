# fingerprint/locale-time/session-declaration

Category: build
Prefs: none read directly
Test: TODO

## What it does

TODO: one paragraph, from the notes below.

## Hooks in Mozilla files

- `dom/base/moz.build` (EXPORTS.mozilla.dom and UNIFIED_SOURCES lists, already-hook): Registers two list entries (not logic, so inline count is 0). StealthSessionDeclaration.h and .cpp are new files that are not in the inventory list. StealthDeclarationGate.h is included by several changed files but is not registered in this hunk; its registration was not checked. VERIFIED: StealthDeclarationGate.h/.cpp are registered in gfx/thebes/moz.build:68 and :245, so nothing is missing here.
- `dom/base/nsGlobalWindowOuter.cpp` (owned by fingerprint/screen-window/geometry, which has the rest of the file): the include of StealthSessionDeclaration.h and, in CreateNativeGlobalForInner, the realm's language and zone from GetDeclaredRealmLanguage/GetDeclaredRealmTimezone instead of Top()->GetLanguageOverride()/GetTimezoneOverride(). Both hooks are tagged with this folder.

## Our own files

- `dom/base/StealthSessionDeclaration.cpp`
- `dom/base/StealthSessionDeclaration.h`
- `intl/locale/StealthSessionPrefs.cpp`
- `intl/locale/StealthSessionPrefs.h`
