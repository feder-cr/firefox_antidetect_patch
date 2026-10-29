# fingerprint/audio/context-values

Category: spoof
Prefs: zoom.stealth.audio.max_channel_count, zoom.stealth.audio.output_latency_ms
Test: TODO

## What it does

TODO: one paragraph, from the notes below.

## Hooks in Mozilla files

- `dom/media/webaudio/AudioContext.cpp` (AudioContext::OutputLatency and AudioContext::MaxChannelCount, already-hook): Two values under one feature name: outputLatency and maxChannelCount. Each returns the pref when it is > 0, before the resistFingerprinting branch. The include hunk adds two stray blank lines after StaticPrefs_zoom.h (cosmetic).

## dom/media/webaudio/AudioContext.cpp: AudioContext::OutputLatency

Stealth: outputLatency from pref set by Python at launch

The block is the first statement after the mIsShutDown check, marked
`// STEALTH-HOOK(fingerprint/audio/context-values)`: it reads
`mozilla::StaticPrefs::zoom_stealth_audio_output_latency_ms()` into
`latency_ms` and, when it is > 0, returns `latency_ms / 1000.0`, before
Mozilla's reduceFingerprinting branch.

## dom/media/webaudio/AudioContext.cpp: AudioContext::MaxChannelCount

Stealth: maxChannelCount from pref set by Python at launch

The block is the first statement of the function, marked
`// STEALTH-HOOK(fingerprint/audio/context-values)`: it reads
`mozilla::StaticPrefs::zoom_stealth_audio_max_channel_count()` into `ch`
and, when it is > 0, returns `(uint32_t)ch`, before Mozilla's
`mShouldResistFingerprinting` branch (which returns 2).

## dom/media/webaudio/AudioContext.cpp: includes

`#include "mozilla/StaticPrefs_zoom.h"` is added after StaticPrefs_media.h
for the two prefs above. The patch had also added two stray blank lines after
it; they are gone.
