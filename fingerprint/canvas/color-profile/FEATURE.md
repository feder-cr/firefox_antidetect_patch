# fingerprint/canvas/color-profile

Category: spoof
Prefs: none read directly
Test: TODO

## What it does

TODO: one paragraph, from the notes below.

## Hooks in Mozilla files

- `gfx/thebes/gfxWindowsPlatform.cpp` (the include block and the first line of gfxWindowsPlatform::GetPlatformCMSOutputProfileData_Impl, before the pref and GetICMProfileW, already-hook): With the engine on, the output colour profile is the one the engine declares (Windows' default display profile, built in code), and the monitor's profile (GetICMProfileW) is never read.

## Our own files

- `gfx/tests/gtest/TestStealthColorProfile.cpp`
- `gfx/thebes/StealthColorProfile.cpp`
- `gfx/thebes/StealthColorProfile.h`
- `gfx/thebes/StealthColorProfile.mozbuild`
- `gfx/thebes/StealthColorProfileCurve.inc`
- `gfx/thebes/gen_stealth_color_profile.py`
