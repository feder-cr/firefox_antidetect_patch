# fingerprint/webgl/multisample

Category: spoof
Prefs: webgl.force-enabled
Test: gfx/gl/gtest/TestStealthGLMultisample.cpp (StealthGLMultisample.OnlyTheBasicRenderDriverWorkaroundIsUndone, StealthGLMultisample.BasicRenderDriverBlendsIntoAMultisampledTarget, Windows only); the WebGL surface of the host gate (webgl1/webgl2 attributes.antialias, SAMPLES) on the Windows runner, which has no GPU.

## What it does

Mozilla takes multisampling away from a GL context whose renderer is the
Microsoft Basic Render Driver, the software D3D device (bug 978966: on Windows 8
it ignored the blend function in multisampled draws). Every Windows machine
without a GPU renders there, so `antialias: false` and `SAMPLES 0` became a fact
about the host that no machine with the declared GPU shows. Today's device blends
correctly into a 4x target (the gtest draws one and reads the resolved pixel on
the Windows CI host). With WebGL forced on regardless of the host's driver
(`webgl.force-enabled`, which the core sets on every host, next to
`gfx.blacklist.webgl.angle`), the feature stays as the driver reports it. Every
other workaround and every other renderer stay Mozilla's.

## Hooks in Mozilla files

- `gfx/gl/GLContext.cpp` (after #include "GLContext.h", after InitFeatures() in GLContext::InitImpl, and after the closing brace of its driver-workaround block, already-hook): The multisample feature as the driver reports it is kept before the workarounds; on the Basic Render Driver, with WebGL forced on, it is put back after them (StealthKeepsMultisample).
- `gfx/gl/gtest/moz.build` (after the UNIFIED_SOURCES list (TestColorspaces.cpp), already-hook): Builds TestStealthGLMultisample.cpp.
- `gfx/gl/moz.build` (after the SOURCES list (GLScreenBuffer.cpp), already-hook): Builds StealthGLMultisample.cpp.

## Our own files

- `gfx/gl/StealthGLMultisample.cpp`
- `gfx/gl/StealthGLMultisample.h`
- `gfx/gl/gtest/TestStealthGLMultisample.cpp`
