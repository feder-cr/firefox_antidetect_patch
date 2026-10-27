# fingerprint/screen-window/popup-position

Category: spoof
Prefs: none read directly
Test: TODO

## What it does

TODO: one paragraph, from the notes below.

## Hooks in Mozilla files

- `toolkit/components/windowwatcher/nsWindowWatcher.cpp` (nsWindowWatcher::CalculateChromeFlagsForContent (hunk 2); static SizeOpenedWindow (hunks 3 and 4); include at top (hunk 1), movable): File mixes two features: this spoof part (popup default position, which a page reads as screenX/screenY, and the chrome flags) and the Juggler flag JUGGLER_WINDOW_EXPLICIT_SIZE (feature juggler-popup-explicit-size, see nsIWebBrowserChrome.idl). Split them in the series. Verified: StealthDeclarationGate.h is gfx/thebes/StealthDeclarationGate.h (struct StealthWindowFrame line 124, StealthDeclaredWin
