# fingerprint/audio/sample-rate

Category: spoof
Prefs: zoom.stealth.audio.sample_rate
Test: TODO

## What it does

TODO: one paragraph, from the notes below.

## Hooks in Mozilla files

- `dom/media/CubebUtils.cpp` (PreferredSampleRate, already-hook): Declared sample rate checked after the sCubebForcedSampleRate test override and before the fingerprint-resistance 44100. Reads StaticPrefs directly; it could be one call into our file.

## dom/media/CubebUtils.cpp: PreferredSampleRate

Comment that stood above the `zoom_stealth_audio_sample_rate()` read, moved out of the Mozilla file verbatim:

> Stealth: sampleRate from pref set by Python at launch
