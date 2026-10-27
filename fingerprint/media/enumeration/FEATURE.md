# fingerprint/media/enumeration

Category: spoof
Prefs: zoom.stealth.media.enumeration_ms
Test: TODO

## What it does

TODO: one paragraph, from the notes below.

## Hooks in Mozilla files

- `dom/media/MediaManager.cpp` (MediaManager::EnumerationParams::RealDeviceRequested; HostSpeakersMayBeAsked (new); EnumerationParams::HostSpeakersRequested (new); MediaManager::MaybeRequestPermissionAndEnumerateRawDevices; MediaManager::EnumerateRawDevices; MediaManager::GetPhysicalDevices, movable): B300: with zoom.stealth.media_enumeration_ms set, speakers are not enumerated from the host, and GetPhysicalDevices resolves after the declared delay (the resolve step is wrapped in a lambda, hunk 6, the largest block). Risk: the nsresult from NS_DelayedDispatchToCurrentThread is ignored; if the dispatch fails, the enumeration promise never resolves. RealDeviceRequested no longer counts EnumerateA
- `dom/media/MediaManager.h` (MediaManager::EnumerationParams (declarations), already-hook): Declares the one decision point shared by RealDeviceRequested, the permission path and EnumerateRawDevices.
