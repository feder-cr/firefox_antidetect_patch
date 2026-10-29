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

## startupcache/StartupCache.h: StartupCache::WritesMayLowerIOPriority

Whether a write may run with lowered I/O priority (nsAutoLowPriorityIO).

It may not. The write holds mTableLock from its first entry to its last,
and the main thread takes the same lock in GetBuffer() for every module it
loads, so a starved writer is a starved main thread. On Windows the lowered
priority is THREAD_MODE_BACKGROUND_BEGIN, which lowers the thread's CPU
priority as well as its I/O, and the write compresses every entry with
LZ4 HC. Upstream saw it at shutdown (bug 2037923) and exempted only that
write; the first regular write, about 30 s after a launch, is the same
inversion. Measured on 2026-10-28 with eleven browsers at once on Windows:
the parent's main thread stood in GetBuffer() for 7-27 s with its CPU idle,
behind a pool thread in WriteToDisk(), in 10 runs of 11, and nothing it
owed the pages or the automation pipe went out meanwhile. With background
mode refused from outside, the same runs never waited more than 4.5 s and
the writes took 0.7-2 s. 151 had no lowered priority here.

## startupcache/test/TestStartupCache.cpp: TEST(StartupCache, WritesNeverLowerIOPriority)

A write holds mTableLock throughout and the main thread's GetBuffer() waits
on it, so no write may lower its own priority: see
StartupCache::WritesMayLowerIOPriority for what that cost under load.
