# fingerprint/webgl/parameters

Category: spoof
Prefs: webgl.sanitize-unmasked-renderer, zoom.stealth.webgl.extensions, zoom.stealth.webgl.float_params, zoom.stealth.webgl.int2_params, zoom.stealth.webgl.int_params, zoom.stealth.webgl.renderer, zoom.stealth.webgl.shader_precisions, zoom.stealth.webgl.vendor, zoom.stealth.webgl2.extensions
Test: TODO

## What it does

TODO: one paragraph, from the notes below.

## Hooks in Mozilla files

- `dom/canvas/ClientWebGLContext.cpp` (ClientWebGLContext::GetParameter, GetShaderPrecisionFormat, ReadPixels (both forms), DoReadPixels, DoReadPixelsAsync, IsSupported, GetSupportedExtensions, BufferParameter; static LookupStealthParamEntry and GetStealthExtensionWhitelist; GetImageBuffer and GetInputStream (toDataURL/toBlob projection); comment-only hunk in GetSurfaceSnapshot, movable): This one file carries at least five features, and the feature field above only names the main one: webgl-parameters (int, float and int2 overrides in GetParameter), webgl-renderer-vendor (renderer and vendor overrides; when the pref is set the ShouldResistFingerprinting branch is skipped), webgl-shader-precision (override in GetShaderPrecisionFormat), webgl-extension-whitelist (IsSupported and Get
