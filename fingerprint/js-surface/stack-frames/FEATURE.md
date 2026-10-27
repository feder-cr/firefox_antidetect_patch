# fingerprint/js-surface/stack-frames

Category: spoof
Prefs: none read directly
Test: TODO

## What it does

TODO: one paragraph, from the notes below.

## Hooks in Mozilla files

- `js/src/vm/SavedStacks.cpp` (BuildStackString, movable): Page-observable: frames whose source starts with the Juggler resource path are dropped from every stack string built for content. The filter is a URL prefix, so it breaks silently if Juggler's resource path changes; juggler/moz.build is the owner of that path. The frame-skip (frame = nextFrame, skippedAsync, continue) must stay inside Mozilla's walk loop; only the source-prefix test (about 15 line
