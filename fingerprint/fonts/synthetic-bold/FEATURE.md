# fingerprint/fonts/synthetic-bold

Category: spoof
Prefs: none read directly
Test: TODO

## What it does

TODO: one paragraph, from the notes below.

## Hooks in Mozilla files

- `gfx/2d/ScaledFontFontconfig.h` (the InstanceData flags enum, already-hook): A flag of the font instance that says Gecko does the bold with several strikes, as Windows does for a webfont, so that neither FreeType nor idwr embolden the same face.

## Our own files

- `gfx/2d/StealthMultistrikeBold.h`
- `gfx/thebes/StealthSyntheticBold.cpp`
- `gfx/thebes/StealthSyntheticBold.h`
- `gfx/thebes/StealthSyntheticBold.mozbuild`
