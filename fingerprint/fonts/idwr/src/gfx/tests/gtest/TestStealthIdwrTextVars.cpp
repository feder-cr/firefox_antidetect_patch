/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// The declared text prefs into the gfxVars Skia's idwr text reads, on Linux
// (gfx/thebes/StealthIdwrTextVars.cpp), and the scaled font hooks that follow the engine
// (gfx/2d/StealthSkiaIdwrFontconfig.cpp).

#include <unistd.h>

#include "gtest/gtest.h"

#include "ScaledFontBase.h"
#include "StealthIdwrTextVars.h"
#include "StealthSkiaIdwr.h"
#include "gfxPlatform.h"
#include "mozilla/Preferences.h"
#include "mozilla/gfx/2D.h"
#include "mozilla/gfx/gfxVars.h"
#include "mozilla/gtest/MozHelpers.h"
#include "mozilla/webrender/WebRenderTypes.h"

using namespace mozilla;
using namespace mozilla::gfx;

namespace {

// Copied verbatim from gfx/thebes/gtest/TestStealthGfxFeatureStatus.cpp (kUhd620, retail
// Firefox 159 on Windows 11, Intel UHD 620): any complete declaration serves, the death
// tests below only need the gate to accept one.
const char kGfxFeatureStatus[] =
    "DIRECT3D_9_LAYERS|1,DIRECT3D_10_LAYERS|1,DIRECT3D_10_1_LAYERS|1,OPENGL"
    "_LAYERS|5|FEATURE_FAILURE_INTEL_OGL_DIS,WEBGL_OPENGL|5|WEBGL_NATIVE_GL"
    "_OLD_INTEL,WEBGL_ANGLE|1,WEBRTC_HW_ACCELERATION_H264|1,DIRECT3D_11_LAY"
    "ERS|1,HARDWARE_VIDEO_DECODING|1,HARDWARE_VIDEO_DECODING_VULKAN|1,DIREC"
    "T3D_11_ANGLE|1,WEBRTC_HW_ACCELERATION_ENCODE|1,WEBRTC_HW_ACCELERATION_"
    "DECODE|1,CANVAS2D_ACCELERATION|1,VP8_HW_DECODE|1,VP9_HW_DECODE|1,DX_IN"
    "TEROP2|1,GPU_PROCESS|1,WEBGL2|1,D3D11_KEYED_MUTEX|1,WEBRENDER|1,DX_NV1"
    "2|1,DX_P010|1,DX_P016|1,GL_SWIZZLE|1,WEBRENDER_COMPOSITOR|1,WEBRENDER_"
    "SCISSORED_CACHE_CLEARS|4|FEATURE_FAILURE_BUG_1603515,THREADSAFE_GL|1,W"
    "EBRENDER_OPTIMIZED_SHADERS|1,X11_EGL|1,DMABUF|1,WEBRENDER_SHADER_CACHE"
    "|1,WEBRENDER_PARTIAL_PRESENT|1,WEBGPU|1,VIDEO_OVERLAY|1,HW_DECODED_VID"
    "EO_ZERO_COPY|3|FEATURE_FAILURE_BUG_1802357,DMABUF_SURFACE_EXPORT|1,REU"
    "SE_DECODER_DEVICE|1,BACKDROP_FILTER|1,ACCELERATED_CANVAS2D|1,H264_HW_D"
    "ECODE|1,AV1_HW_DECODE|1,HEVC_HW_DECODE|1,VIDEO_SOFTWARE_OVERLAY|1,WEBG"
    "L_USE_HARDWARE|1,OVERLAY_VP_AUTO_HDR|1,OVERLAY_VP_SUPER_RESOLUTION|1,D"
    "MABUF_WEBGL|1,WEBRENDER_DCOMP|1,VP8_HW_ENCODE|1,VP9_HW_ENCODE|1,H264_H"
    "W_ENCODE|1,AV1_HW_ENCODE|1,HEVC_HW_ENCODE|1,WMF_HW_DRM|1,GL_NORM16_TEX"
    "TURES|1,HARDWARE_VIDEO_ENCODING|1,WEBGPU_EXTERNAL_TEXTURE|1,MESA_THREA"
    "DING|1,WEBGL|1,VIDEO_HDR|1,WEBGL_ANGLE_METAL|1,WEBRENDER_ANGLE_METAL|1";

// What a launcher declares for the text: the engine on, a quality, rendering mode 0.
void DeclareText(int32_t aQuality) {
  // The engine on needs the graphics declaration too (StealthGfxFeatureStatus refuses
  // an empty one at the first feature query).
  Preferences::SetCString("zoom.stealth.gfx.feature_status", kGfxFeatureStatus);
  Preferences::SetInt("zoom.stealth.text.system_quality", aQuality);
  Preferences::SetInt("gfx.font_rendering.cleartype_params.rendering_mode", 0);
  // The engine on, last: with it on a change of the text prefs updates the text vars.
  Preferences::SetInt("zoom.stealth.fpp.hw_seed", 7);
}

// The body of the child of the death tests below: a process where gfxPlatform is not
// initialised, the declaration made, then the platform built. gfxPlatformGtk's
// constructor runs StealthIdwrUpdateTextVars while gPlatform is still null (it is
// assigned after the constructor returns) and gEverInitialized is already true.
[[noreturn]] void ConstructPlatformWithQuality(int32_t aQuality) {
  mozilla::gtest::DisableCrashReporter();
  if (gfxPlatform::Initialized()) {
    // The test's own assumption, not the code under test: the child must be a fresh
    // process, or it does not replay the constructor.
    _exit(77);
  }
  DeclareText(aQuality);
  gfxPlatform::GetPlatform();
  _exit(gfxVars::SystemTextQuality() == aQuality ? 0 : 3);
}

}  // namespace

// Before the fix, a declared quality that is not 5 (the gfxVar's default) made the first
// SetSystemTextQuality, inside the constructor, call the listener: FlushTextCaches,
// gfxPlatform::PurgeSkiaFontCache, GetPlatform(), Init() again, and
// MOZ_RELEASE_ASSERT(!gEverInitialized) stopped the parent at launch.
#if defined(GTEST_HAS_DEATH_TEST) && !defined(XP_DARWIN)
TEST(StealthIdwrTextVars, PlatformConstructorWithAntialiasedQualityStarts)
{
  GTEST_FLAG_SET(death_test_style, "threadsafe");
  EXPECT_EXIT(ConstructPlatformWithQuality(4), ::testing::ExitedWithCode(0), "");
}

TEST(StealthIdwrTextVars, PlatformConstructorWithDefaultQualityStarts)
{
  GTEST_FLAG_SET(death_test_style, "threadsafe");
  EXPECT_EXIT(ConstructPlatformWithQuality(0), ::testing::ExitedWithCode(0), "");
}
#endif

namespace {

// A one glyph bitmap font in BDF: FreeType reads it, idwr (sfnt only) refuses it.
constexpr char kBdf[] =
    "STARTFONT 2.1\n"
    "FONT -misc-test-medium-r-normal--8-80-75-75-c-80-iso10646-1\n"
    "SIZE 8 75 75\n"
    "FONTBOUNDINGBOX 8 8 0 0\n"
    "STARTPROPERTIES 2\n"
    "FONT_ASCENT 8\n"
    "FONT_DESCENT 0\n"
    "ENDPROPERTIES\n"
    "CHARS 1\n"
    "STARTCHAR A\n"
    "ENCODING 65\n"
    "SWIDTH 1000 0\n"
    "DWIDTH 8 0\n"
    "BBX 8 8 0 0\n"
    "BITMAP\n"
    "18\n24\n42\n42\n7E\n42\n42\n00\n"
    "ENDCHAR\n"
    "ENDFONT\n";

struct ClearEngine {
  ~ClearEngine() {
    for (const char* name : {"zoom.stealth.fpp.hw_seed", "zoom.stealth.text.system_quality",
                             "gfx.font_rendering.cleartype_params.rendering_mode"}) {
      Preferences::ClearUser(name);
    }
  }
};

}  // namespace

// With the engine off Skia's text is stock Firefox's; with it on it is idwr's.
TEST(StealthIdwrFontconfig, TheEngineDecidesWhetherSkiaTextIsIdwrs)
{
  gfxPlatform::GetPlatform();
  ClearEngine clear;
  Preferences::ClearUser("zoom.stealth.fpp.hw_seed");
  EXPECT_FALSE(StealthIdwrAAModeDeclared());
  Preferences::SetInt("zoom.stealth.fpp.hw_seed", 7);
  EXPECT_TRUE(StealthIdwrAAModeDeclared());
}

// idwr cannot take the font: there is no typeface. Before, ScaledFontFontconfig went on to
// SkCreateTypefaceFromCairoFTFont and the host's FreeType drew the glyphs.
TEST(StealthIdwrFontconfig, AFontIdwrRefusesHasNoTypeface)
{
  gfxPlatform::GetPlatform();
  ClearEngine clear;
  DeclareText(5);

  RefPtr<NativeFontResource> resource = Factory::CreateNativeFontResource(
      reinterpret_cast<const uint8_t*>(kBdf), sizeof(kBdf) - 1, FontType::FONTCONFIG);
  if (!resource) {
    GTEST_SKIP() << "no native font resource for the BDF";
  }
  RefPtr<UnscaledFont> unscaled = resource->CreateUnscaledFont(0, nullptr, 0);
  if (!unscaled) {
    GTEST_SKIP() << "FreeType does not read the BDF";
  }
  wr::FontInstanceOptions options = {};
  wr::FontInstancePlatformOptions platform = {};
  RefPtr<ScaledFont> font = unscaled->CreateScaledFontFromWRFont(
      8.0f, &options, &platform, nullptr, 0);
  ASSERT_TRUE(font);
  EXPECT_FALSE(static_cast<ScaledFontBase*>(font.get())->GetSkTypeface());
}

// A declaration without the rendering mode is refused, as one without a quality is. Windows
// forces GDI classic for small text when the mode is not declared, and the Linux text does
// not reproduce that; the core always declares the mode.
TEST(StealthIdwrTextVars, ADeclarationWithoutTheRenderingModeIsRefused)
{
  StealthIdwrTextPrefs prefs;
  prefs.mQuality = 5;
  StealthIdwrTextVars missing = StealthIdwrTextVarsFromPrefs(prefs);
  EXPECT_TRUE(missing.mRefusal);

  prefs.mMode = 0;
  EXPECT_FALSE(StealthIdwrTextVarsFromPrefs(prefs).mRefusal);
  prefs.mMode = 3;
  StealthIdwrTextVars declared = StealthIdwrTextVarsFromPrefs(prefs);
  EXPECT_FALSE(declared.mRefusal);
  EXPECT_EQ(3, declared.mRenderingMode);

  // Out of range, as gfxDWriteFont::UpdateClearTypeVars takes it: the default mode.
  prefs.mMode = 9;
  StealthIdwrTextVars outOfRange = StealthIdwrTextVarsFromPrefs(prefs);
  EXPECT_FALSE(outOfRange.mRefusal);
  EXPECT_EQ(0, outOfRange.mRenderingMode);

  // The quality refusal stays what it was.
  prefs.mMode = 0;
  prefs.mQuality = -1;
  EXPECT_TRUE(StealthIdwrTextVarsFromPrefs(prefs).mRefusal);
}
