# automation/window-size

Category: juggler
Prefs: none read directly
Test: TODO

## What it does

TODO: one paragraph, from the notes below.

## Hooks in Mozilla files

- `toolkit/components/browser/nsIWebBrowserChrome.idl` (interface nsIWebBrowserChrome, window flag constants (hunk header line 94), already-hook): Takes bit 25 from the free bits and says so in the comment. The flag is set in nsWindowWatcher.cpp (CalculateChromeFlagsForContent), which mixes it with a spoof change; split when building the series. Juggler reads the flag from the chrome flags.
