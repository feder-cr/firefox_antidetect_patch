# fingerprint/webgl/multisample

Category: spoof
Prefs: webgl.msaa-samples
Test: gfx/gl/gtest/TestStealthGLMultisample.cpp (StealthGLMultisample.TheDeclaredSampleCountDecides, StealthGLMultisample.BasicRenderDriverBlendsIntoAMultisampledTarget, Windows only); the WebGL surface of the host gate (webgl1/webgl2 attributes.antialias, SAMPLES) on the Windows runner, which has no GPU.

## What it does

Multisampling is part of the GPU the core declares (`webgl.msaa-samples`, 4 on
every identity, which every declared GPU pairs with: its declared MAX_SAMPLES is
4 or more). Mozilla's driver workarounds decide it from the host's renderer
instead: on the Microsoft Basic Render Driver, the software D3D device every
Windows machine without a GPU renders on, it is dropped (bug 978966, a Windows 8
blending bug), and a page read `antialias: false` and `SAMPLES 0`, a fact about
the host. The decision is taken back where it is born: when the core declares
samples and the driver can draw them, the context has them, whatever the
renderer. A declared 0 needs nothing here (Mozilla turns antialias off for every
context). Today's software device blends correctly into a 4x target: the gtest
draws one and reads the resolved pixel on the Windows CI host, which has no GPU.

## Hooks in Mozilla files

- `gfx/gl/GLContext.cpp` (after #include "GLContext.h", after InitFeatures() in GLContext::InitImpl, and after the closing brace of its driver-workaround block, already-hook): The multisample feature as the driver reports it is kept before the workarounds and put back after them when the core declares samples (webgl.msaa-samples > 0, StealthDeclaredMultisample): the declaration decides, not the host renderer.
- `gfx/gl/gtest/moz.build` (after the UNIFIED_SOURCES list (TestColorspaces.cpp), already-hook): Builds TestStealthGLMultisample.cpp.
- `gfx/gl/moz.build` (after the SOURCES list (GLScreenBuffer.cpp), already-hook): Builds StealthGLMultisample.cpp.

## Our own files

- `gfx/gl/StealthGLMultisample.cpp`
- `gfx/gl/StealthGLMultisample.h`
- `gfx/gl/gtest/TestStealthGLMultisample.cpp`
