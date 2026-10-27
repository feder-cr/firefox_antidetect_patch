# fingerprint/audio/noise

Category: spoof
Prefs: zoom.stealth.audio.fp_noise, zoom.stealth.fpp.hw_seed
Test: TODO

## What it does

TODO: one paragraph, from the notes below.

## Hooks in Mozilla files

- `dom/media/webaudio/AnalyserNode.cpp` (AnalyserNode::GetFloatFrequencyData and AnalyserNode::GetFloatTimeDomainData, movable): Two noise loops of about 8 logic lines each, using the same hash as AudioDestinationNode.cpp and DynamicsCompressorNode.h. Move to one helper in our file. Frequency-domain noise is 0.05 to 0.25 dB depending on seed; time-domain noise is 5e-5 to 2.5e-4 on [-1,1] samples. The time-domain path has no silence or length guard (the comment says so), unlike AudioDestinationNode.cpp. Both gates were chang
- `dom/media/webaudio/AudioDestinationNode.cpp` (OfflineDestinationNodeEngine render loop (class OfflineDestinationNodeEngine, hunk @@ -131), movable): Only OfflineAudioContext gets this noise; the realtime path is not in this diff. The gates (silent channel, length > 200, skip first 100 samples) are business logic and should move to our file with the loop. The comment at AudioDestinationNode.cpp:152 says scale ~5e-7; the code gives 5e-7 to 2.5e-6, so the comment states only the lower bound. [verified: REFUTED as a bug, minor comment imprecision 
- `dom/media/webaudio/DynamicsCompressorNode.h` (DynamicsCompressorNode::SetReduction (inline, in header), movable): Adds a StaticPrefs include to a public header, so every includer rebuilds when the pref header changes. Move the perturbation to a .cpp and keep one declared call in the header with no StaticPrefs include. Noise amplitude is about 1e-5 on each SetReduction call.
