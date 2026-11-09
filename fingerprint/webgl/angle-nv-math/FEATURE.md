# fingerprint/webgl/angle-nv-math

Category: spoof
Prefs: zoom.stealth.webgl.alu_class
Test: gfx/gl/gtest/TestStealthAngleAlu.cpp (StealthAngleAlu.EachClassNamesItsFeatureAndUnknownOnesNone, StealthAngleAlu.TheFeatureIsAppendedToTheCallersList, StealthAngleAlu.NoListIsAListWithTheFeatureOnly, StealthAngleAlu.NothingToAddLeavesTheCallersListAlone, StealthAngleAlu.TheDeclaredClassIsThePrefWithTheEngineOn; Windows only: StealthAngleAlu.ClassZeroHLSLHasNoNvidiaHelper, StealthAngleAlu.TheNvidiaClassEmitsTheFmaChainForTwoThreeAndFour, StealthAngleAlu.ANeutralShaderIsTheSameInBothClasses); in the wrapper, `tests/test_webgl_dot_has_the_declared_gpu_s_bits_e2e.py` (Windows: the bits of dot() a page reads back, classes 1 and 2, against the NVIDIA fma chain computed in exact arithmetic, and class 0 as the control).

## What it does

The last bit of what a WebGL page reads back depends on which instructions the GPU runs: a float `dot()` is one chain of fused multiply-adds on NVIDIA hardware and another sequence on other adapters or on WARP, the software rasteriser Windows falls back to. The core declares the arithmetic class of the GPU the persona has (`zoom.stealth.webgl.alu_class`: 0 none, 1 NVIDIA Maxwell and Pascal, 2 NVIDIA Volta and later), and the HLSL ANGLE emits carries that arithmetic as IEEE-only `precise` helpers, so every adapter gives the same bits.

The declaration reaches ANGLE through the one channel its translator has, the feature overrides of the EGL display: `StealthAngleAlu.cpp` reads the pref (Windows, engine on) and the hooks in `GLLibraryEGL::fGetDisplay` and `fGetPlatformDisplay` add the ANGLE D3D feature `stealthAluNvidia` to every `EGL_PLATFORM_ANGLE_ANGLE` display, the one WebGL draws on. Every class of 1 or more is that feature, which is all `dot()` needs; the difference between 1 and 2 is for sin and cos. The feature sets the `ShCompileOptions` bit of the same name (`ShaderD3D::compile`, so it is in the shader cache key), and with it `OutputHLSL` emits float `dot()` of two, three or four components as `nv_dot2/3/4` and defines the helpers (`StealthNvMath.h`, the text of `reimpl/angle_nv/nv_math.hlsl` of the invisible_D3D11Core study) in the shader header, only in a shader that uses one. Class 0, the engine off and any host but Windows leave the display request and every character of the HLSL as Mozilla has them. The ANGLE side is `gfx/angle/patches/0021` (the patch ANGLE's vendoring re-applies) and the same lines in `third_party/angle`.

Gecko passes the override `allowES3OnFL10_0` (`GLLibraryEGL::CreateDisplay`, device displays) but the ANGLE feature is called `allowES3OnFL100`; the retail build has the same wrong name, so the override is ignored and we do as the retail does: it is left as it is.

## Hooks in Mozilla files

- `gfx/gl/GLLibraryEGL.h` (after struct ID3D11Device;, and at the top of fGetDisplay and fGetPlatformDisplay, already-hook): the declared ALU class's feature rides the feature overrides of every ANGLE display.
- `third_party/angle/include/GLSLANG/ShaderLang.h` (after expandFragmentOutputsToVec4 in ShCompileOptions, already-hook): the `stealthAluNvidia` option bit.
- `third_party/angle/include/platform/autogen/FeaturesD3D_autogen.h` (after the last FeatureInfo of FeaturesD3D, already-hook): the D3D feature `stealthAluNvidia`.
- `third_party/angle/src/libANGLE/renderer/d3d/ShaderD3D.cpp` (after allowTranslateUniformBlockToStructuredBuffer in ShaderD3D::compile, already-hook): the feature fills the option bit.
- `third_party/angle/src/compiler/translator/hlsl/OutputHLSL.cpp` and `OutputHLSL.h` (the include, the constructor, OutputHLSL::header, and the top of case EOpDot in visitAggregate, already-hook): float `dot()` as the helpers, which the header defines; Mozilla's own `dot(` line is untouched.
- `gfx/gl/moz.build` and `gfx/gl/gtest/moz.build` (one line each after the lists, and one more in the gtest's, already-hook): build StealthAngleAlu.cpp and TestStealthAngleAlu.cpp with ANGLE's include directory. Both files are owned by fingerprint/webgl/multisample, so the lines are in its hooks.patch, tagged with this folder.

## Our own files

- `gfx/gl/StealthAngleAlu.cpp`
- `gfx/gl/StealthAngleAlu.h`
- `gfx/gl/gtest/TestStealthAngleAlu.cpp`
- `gfx/angle/patches/0021-Add-the-stealthAluNvidia-feature-NVIDIA-fma-chain-fo.patch`
- `third_party/angle/src/compiler/translator/hlsl/StealthNvMath.h`

The pref `zoom.stealth.webgl.alu_class` is declared in `modules/libpref/init/StealthPrefList.yaml`, owned by platform/prefs.
