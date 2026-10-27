# automation/ignore-https-errors

Category: juggler
Prefs: none read directly
Test: TODO

## What it does

TODO: one paragraph, from the notes below.

## Hooks in Mozilla files

- `security/manager/ssl/nsCertOverrideService.cpp` (IsDebugger (static, nsCertOverrideService.cpp), movable): IsDebugger() returns true when a Juggler pipe is connected, so Browser.setIgnoreHTTPSErrors can flip the per-context switch (a page cannot see this). Hunk 2 has 7 logic lines: move the check into one function in our own file and keep a one-line hook. The pipe is looked up by contract id string '@mozilla.org/juggler/remotedebuggingpipe;1'. The include of nsIRemoteDebuggingPipe.h sits under ENABLE_W
