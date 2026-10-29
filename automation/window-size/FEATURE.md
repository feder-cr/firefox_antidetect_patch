# automation/window-size

Category: juggler
Prefs: none read directly
Test: TODO

## What it does

TODO: one paragraph, from the notes below.

## Hooks in Mozilla files

- `toolkit/components/browser/nsIWebBrowserChrome.idl` (interface nsIWebBrowserChrome, window flag constants (hunk header line 94), already-hook): Takes bit 25 from the free bits and says so in the comment. The flag is set in nsWindowWatcher.cpp (CalculateChromeFlagsForContent), which mixes it with a spoof change; split when building the series. Juggler reads the flag from the chrome flags.

## toolkit/components/browser/nsIWebBrowserChrome.idl: JUGGLER_WINDOW_EXPLICIT_SIZE

Juggler: the window.open features that opened this popup give a size
(width, height, innerWidth or innerHeight). Juggler then leaves the
window at that size instead of applying the context's default viewport.

The constant takes bit 25, one of the two bits that the Mozilla comment
"Two bits are free here." counted as free; that comment now reads
"One bit is free here."
