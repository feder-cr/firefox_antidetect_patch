# platform/startup-cache

Category: other
Prefs: none read directly
Test: TODO

## What it does

TODO: one paragraph, from the notes below.

## Hooks in Mozilla files

- `startupcache/StartupCache.cpp` (StartupCache::MaybeWriteOffMainThread, already-hook): Fix of a 157 priority inversion (commit 044fdc985f): StartupCache writes no longer take nsAutoLowPriorityIO, whose THREAD_MODE_BACKGROUND_BEGIN starved the writer while it held mTableLock and so the main thread in GetBuffer() (7-27 s stalls with eleven browsers). 151 had no lowered priority here.
- `startupcache/StartupCache.h` (StartupCache::WritesMayLowerIOPriority (new), already-hook): Fix of a 157 priority inversion (commit 044fdc985f): StartupCache writes no longer take nsAutoLowPriorityIO, whose THREAD_MODE_BACKGROUND_BEGIN starved the writer while it held mTableLock and so the main thread in GetBuffer() (7-27 s stalls with eleven browsers). 151 had no lowered priority here.
- `startupcache/test/TestStartupCache.cpp` (TEST(StartupCache, WritesNeverLowerIOPriority) (new), already-hook): Fix of a 157 priority inversion (commit 044fdc985f): StartupCache writes no longer take nsAutoLowPriorityIO, whose THREAD_MODE_BACKGROUND_BEGIN starved the writer while it held mTableLock and so the main thread in GetBuffer() (7-27 s stalls with eleven browsers). 151 had no lowered priority here.
