# fingerprint/webgl/own-gl

Category: spoof
Prefs: none read directly (on only with the engine on, `zoom.stealth.fpp.hw_seed`)
Test: the build proves the GL before keeping it (`scripts/ci/lavapipe/proof/gl_proof.c`: loads from its directory alone, a surfaceless GLES 3 context, a clear read back, no GL library of the host mapped); `scripts/ci/test_ci.py` (the `webgl:` cases); in the wrapper, `tests/test_webgl_draws_on_the_engine_s_own_gl_e2e.py`, run by the gate's guards under Xvfb (Linux, under X11: the process that draws maps the engine's libEGL and libgallium and no libEGL of the host's, and every declared extension answers as on Windows).

## What it does

On Linux WebGL drew on whatever GL the host had, GLX or EGL from the system's
libraries: the host's Mesa version, or its GPU driver, decided which declared
extensions could be served and every pixel a page read back. The GL now ships
with the engine, in `llvmpipe/` next to the executable: Mesa's llvmpipe behind
EGL (`libEGL.so.1`, `libGLESv2.so.2`, `libgallium-<mesa>.so`), built by
`scripts/ci/lavapipe_build.py` from WebGPU's lavapipe tree, pins and patches,
surfaceless only, with no GLX and no glvnd. Its Mesa options are compiled in and
the environment is never read, and it has one CPU profile, so the same page
renders the same bytes on every host. With the engine on, the hooks fail
closed: EGL is ours or nothing (`GLLibraryEGL::Init`, never the host's
libEGL), the display is EGL's surfaceless platform, asked for with no native
display (`CreateDisplayLocked`, `StealthOwnGLSurfacelessDisplay`; never the
host's X11, Wayland or render nodes, and not Mozilla's `EGL_DEFAULT_DISPLAY`,
which under X11 is the X11 display and which Mesa refuses there), and a
headless context is always EGL (`GLContextProviderLinux::CreateHeadless`, never GLX). Without the
files WebGL has no context. With the engine off, Firefox is unchanged.

## Hooks in Mozilla files

- `gfx/gl/GLLibraryEGL.cpp` (after #include "GLLibraryEGL.h", after the non-Windows library comment in GLLibraryEGL::Init, and after #ifdef MOZ_WIDGET_GTK in CreateDisplayLocked, already-hook): EGL is the engine's own llvmpipe or nothing, and its display is the surfaceless one.
- `gfx/gl/GLContextProviderLinux.cpp` (after #include "GLContextProvider.h", and at the top of GLContextProviderLinux::CreateHeadless, already-hook): a headless context is always EGL, never GLX.

## Our own files

- `gfx/gl/StealthOwnGL.h`
