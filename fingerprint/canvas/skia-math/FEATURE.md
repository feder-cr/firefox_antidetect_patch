# fingerprint/canvas/skia-math

Category: spoof
Prefs: none read directly
Test: TODO

## What it does

TODO: one paragraph, from the notes below.

## Hooks in Mozilla files

- `gfx/skia/skia/src/opts/SkRasterPipeline_opts.h` (after the system includes; around the SSE2 mad/nmad/rcp_precise and the rcp_fast/rsqrt dispatch, already-hook): Five added lines (one include of the wrappers of the reciprocal intrinsics, two Begin/End pairs that rename the stock functions by macro): with the engine on the highp raster pipeline uses an exact FMA and the RCPPS/RSQRTPS model of the declared CPU vendor, with it off the stock functions; no line of Skia is rewritten.

## Our own files

- `gfx/skia/skia/src/opts/StealthSkiaMadBegin.h`
- `gfx/skia/skia/src/opts/StealthSkiaMadEnd.h`
- `gfx/skia/skia/src/opts/StealthSkiaMath.cpp`
- `gfx/skia/skia/src/opts/StealthSkiaMath.h`
- `gfx/skia/skia/src/opts/StealthSkiaMathTables.inc`
- `gfx/skia/skia/src/opts/StealthSkiaRcpFastBegin.h`
- `gfx/skia/skia/src/opts/StealthSkiaRcpFastEnd.h`
- `gfx/skia/skia/src/opts/StealthSkiaRcpHook.h`
- `gfx/skia/skia/src/opts/gen_stealth_skia_math.py`
- `gfx/skia/skia/src/opts/stealth_skia_math/model_amd.txt`
- `gfx/skia/skia/src/opts/stealth_skia_math/model_intel.txt`
- `gfx/tests/gtest/TestStealthSkiaMath.cpp`
- `gfx/tests/gtest/TestStealthSkiaMathCases.h`
