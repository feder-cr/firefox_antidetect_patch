# fingerprint/webgl/parameters

Category: spoof
Prefs: webgl.sanitize-unmasked-renderer, zoom.stealth.webgl.extensions, zoom.stealth.webgl.float_params, zoom.stealth.webgl.int2_params, zoom.stealth.webgl.int_params, zoom.stealth.webgl.renderer, zoom.stealth.webgl.shader_precisions, zoom.stealth.webgl.vendor, zoom.stealth.webgl2.extensions
Test: invisible_playwright `tests/test_a_declared_webgl_extension_survives_its_use_e2e.py` (the six entry points, both context kinds, then a question to the page)

## What it does

TODO: one paragraph, from the notes below.

## Hooks in Mozilla files

- `dom/canvas/ClientWebGLContext.cpp` (ClientWebGLContext::GetParameter, GetShaderPrecisionFormat, ReadPixels (both forms), DoReadPixels, DoReadPixelsAsync, IsSupported, GetSupportedExtensions, BufferParameter; static LookupStealthParamEntry and GetStealthExtensionWhitelist; GetImageBuffer and GetInputStream (toDataURL/toBlob projection); comment-only hunk in GetSurfaceSnapshot, movable): This one file carries at least five features, and the feature field above only names the main one: webgl-parameters (int, float and int2 overrides in GetParameter), webgl-renderer-vendor (renderer and vendor overrides; when the pref is set the ShouldResistFingerprinting branch is skipped), webgl-shader-precision (override in GetShaderPrecisionFormat), webgl-extension-whitelist (IsSupported and Get
- `dom/canvas/WebGLContextExtensions.cpp` (WebGLContext::IsExtensionSupported, case WEBGL_provoking_vertex): the host serves an extension the persona declares wherever its GL has it, ahead of Mozilla's per-platform policy (ANGLE or macOS only). The page is told the persona's list on the client side; the host decided on its own, and a declared extension it refused was a trap (2026-11-07, read with symbols on Linux: one `provokingVertexWEBGL()` call, MOZ_RELEASE_ASSERT, main process gone, no GPU process there).
- `dom/canvas/WebGLContextGL.cpp` (BlendEquationSeparate and BlendFuncSeparate with an index, PolygonOffsetClampEXT, ProvokingVertex, ClipControlEXT) and `dom/canvas/WebGLContextFramebufferOperations.cpp` (ColorMask with an index): an entry point whose extension is not enabled returns before Mozilla's assert, which stays. The safety net under the hook above, for a GL that lacks the feature.
