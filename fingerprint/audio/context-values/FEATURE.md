# fingerprint/audio/context-values

Category: spoof
Prefs: zoom.stealth.audio.max_channel_count, zoom.stealth.audio.output_latency_ms
Test: TODO

## What it does

TODO: one paragraph, from the notes below.

## Hooks in Mozilla files

- `dom/media/webaudio/AudioContext.cpp` (AudioContext::OutputLatency and AudioContext::MaxChannelCount, already-hook): Two values under one feature name: outputLatency and maxChannelCount. Each returns the pref when it is > 0, before the resistFingerprinting branch. The include hunk adds two stray blank lines after StaticPrefs_zoom.h (cosmetic).
