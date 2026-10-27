# fingerprint/js-surface/debugger

Category: other, spoof
Prefs: zoom.stealth.debugger.force_detach
Test: TODO

## What it does

TODO: one paragraph, from the notes below.

## Hooks in Mozilla files

- `devtools/server/actors/thread.js` (ThreadActor.attach(options), already-hook): With the pref true, attach() returns early and the thread actor never attaches through this path. The pref name says force_detach but the code blocks attach: name and behaviour disagree (CONFIRMED from the diff: it is the same early return as alreadyAttached). The pref has no entry in StaticPrefList.yaml (CONFIRMED against that file's diff) and is read in JS with default false; no other reader fou
- `js/src/debugger/Debugger.cpp` (Debugger::addDebuggeeGlobal; Debugger::removeDebuggeeGlobal, hard): Observable by page code: with stealthMode the realm reports no debuggers (hasDebuggers() false) and does not enter debug mode, so Ion and the slow native-call path stay as on a non-debugged realm. stealthMode is a JS property of the Debugger object that Juggler sets, not a pref. The read of that property must happen before the no-GC region (it can run JS). The removal path runs during GC sweep, so
- `js/src/debugger/Debugger.h` (class Debugger (members, next to allocationsLog), already-hook): Plain member read by removeDebuggeeGlobal during GC sweep, so it must stay a plain bool with no JS calls behind it. Default is false, which keeps the normal path unchanged.
