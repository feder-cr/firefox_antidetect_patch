# automation/window-size

Category: juggler
Prefs: none read directly
Test: TODO

## What it does

TODO: one paragraph, from the notes below.

## Hooks in Mozilla files

- `toolkit/components/windowwatcher/nsWindowWatcher.cpp`: the popup branch of CalculateChromeFlagsForContent calls JugglerPopupChromeFlags (toolkit/components/windowwatcher/JugglerWindowSize.cpp).

## The explicit-size flag

Juggler: the window.open features that opened this popup give a size
(width, height, innerWidth or innerHeight). Juggler then leaves the
window at that size instead of applying the context's default viewport.

The flag is ours: `kJugglerWindowExplicitSize` in JugglerWindowSize.h, read as
`JUGGLER_WINDOW_EXPLICIT_SIZE` in juggler/TargetRegistry.js, bit 1 of the chrome
flags, which nsIWebBrowserChrome leaves free. Until 2026-10-29 it was declared
in nsIWebBrowserChrome.idl as 1 << 25, one of the bits the Mozilla comment
"Two bits are free here." counted; Firefox 159 gives that bit to
CHROME_MEDIA_PIP. juggler/tests/explicit_size_flag_is_free.test.mjs checks that
both sides agree and that no CHROME_* flag uses the bit.
