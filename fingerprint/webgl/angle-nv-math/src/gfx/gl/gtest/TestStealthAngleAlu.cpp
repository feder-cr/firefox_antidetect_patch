/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// The arithmetic class the persona declares reaches ANGLE (StealthAngleAlu.h):
// zoom.stealth.webgl.alu_class becomes the feature `stealthAluNvidia` in the
// feature overrides of the EGL display, and the translator then emits float
// dot() of two, three or four components as the NVIDIA fma chain
// (OutputHLSL.cpp, nv_dot2/3/4). Class 0 is the HLSL the build emitted before
// the feature: no character of it changes.

#include <string>

#include "StealthAngleAlu.h"
#include "gtest/gtest.h"
#include "mozilla/Preferences.h"
#include "mozilla/ScopeExit.h"

#ifdef XP_WIN
#  include "GLSLANG/ShaderLang.h"
#endif

namespace mozilla::gl {

constexpr char kFeature[] = "stealthAluNvidia";

TEST(StealthAngleAlu, EachClassNamesItsFeatureAndUnknownOnesNone)
{
  EXPECT_EQ(StealthAngleAluFeatureOf(0), nullptr);
  // 1 = NVIDIA Maxwell and Pascal, 2 = Volta and later: both are the feature
  ASSERT_NE(StealthAngleAluFeatureOf(1), nullptr);
  EXPECT_STREQ(StealthAngleAluFeatureOf(1), kFeature);
  ASSERT_NE(StealthAngleAluFeatureOf(2), nullptr);
  EXPECT_STREQ(StealthAngleAluFeatureOf(2), kFeature);
  EXPECT_EQ(StealthAngleAluFeatureOf(3), nullptr);
  EXPECT_EQ(StealthAngleAluFeatureOf(-1), nullptr);
}

// The caller's list is kept whole and the overrides come after it.
TEST(StealthAngleAlu, TheFeatureIsAppendedToTheCallersList)
{
  const EGLAttrib list[] = {LOCAL_EGL_PLATFORM_ANGLE_DEVICE_TYPE_ANGLE,
                            LOCAL_EGL_PLATFORM_ANGLE_DEVICE_TYPE_WARP_ANGLE,
                            LOCAL_EGL_NONE};
  const StealthAngleAluAttribs attribs(kFeature, LOCAL_EGL_PLATFORM_ANGLE_ANGLE,
                                       list);
  const EGLAttrib* got = attribs.Get();
  ASSERT_NE(got, list);
  EXPECT_EQ(got[0], LOCAL_EGL_PLATFORM_ANGLE_DEVICE_TYPE_ANGLE);
  EXPECT_EQ(got[1], LOCAL_EGL_PLATFORM_ANGLE_DEVICE_TYPE_WARP_ANGLE);
  ASSERT_EQ(got[2], LOCAL_EGL_FEATURE_OVERRIDES_ENABLED_ANGLE);
  const char* const* names = reinterpret_cast<const char* const*>(got[3]);
  ASSERT_NE(names, nullptr);
  EXPECT_STREQ(names[0], kFeature);
  EXPECT_EQ(names[1], nullptr);
  EXPECT_EQ(got[4], LOCAL_EGL_NONE);
}

TEST(StealthAngleAlu, NoListIsAListWithTheFeatureOnly)
{
  const StealthAngleAluAttribs attribs(kFeature, LOCAL_EGL_PLATFORM_ANGLE_ANGLE,
                                       nullptr);
  const EGLAttrib* got = attribs.Get();
  ASSERT_NE(got, nullptr);
  EXPECT_EQ(got[0], LOCAL_EGL_FEATURE_OVERRIDES_ENABLED_ANGLE);
  EXPECT_EQ(got[2], LOCAL_EGL_NONE);
}

// Class 0, another platform, or a list that already has overrides: the very
// pointer the caller passed, so the display is asked for exactly as before.
TEST(StealthAngleAlu, NothingToAddLeavesTheCallersListAlone)
{
  const EGLAttrib list[] = {LOCAL_EGL_PLATFORM_ANGLE_DEVICE_TYPE_ANGLE,
                            LOCAL_EGL_PLATFORM_ANGLE_DEVICE_TYPE_WARP_ANGLE,
                            LOCAL_EGL_NONE};
  EXPECT_EQ(StealthAngleAluAttribs(nullptr, LOCAL_EGL_PLATFORM_ANGLE_ANGLE, list)
                .Get(),
            list);
  EXPECT_EQ(StealthAngleAluAttribs(nullptr, LOCAL_EGL_PLATFORM_ANGLE_ANGLE,
                                   nullptr)
                .Get(),
            nullptr);
  EXPECT_EQ(StealthAngleAluAttribs(kFeature, LOCAL_EGL_PLATFORM_DEVICE_EXT,
                                   list)
                .Get(),
            list);

  const char* other[] = {"allowES3OnFL10_0", nullptr};
  const EGLAttrib withOverrides[] = {LOCAL_EGL_FEATURE_OVERRIDES_ENABLED_ANGLE,
                                     reinterpret_cast<EGLAttrib>(other),
                                     LOCAL_EGL_NONE};
  EXPECT_EQ(StealthAngleAluAttribs(kFeature, LOCAL_EGL_PLATFORM_ANGLE_ANGLE,
                                   withOverrides)
                .Get(),
            withOverrides);
}

// The declaration is the pref, and only with the engine on. Both NVIDIA classes
// (1 = Maxwell and Pascal, 2 = Volta and later) declare the feature.
TEST(StealthAngleAlu, TheDeclaredClassIsThePrefWithTheEngineOn)
{
  auto restore = MakeScopeExit([] {
    Preferences::ClearUser("zoom.stealth.webgl.alu_class");
    Preferences::ClearUser("zoom.stealth.fpp.hw_seed");
  });
  for (int32_t aluClass : {1, 2}) {
    Preferences::SetInt("zoom.stealth.webgl.alu_class", aluClass);
    Preferences::SetInt("zoom.stealth.fpp.hw_seed", 0);
    EXPECT_EQ(StealthAngleAluDeclaredFeature(), nullptr);  // engine off
    EXPECT_FALSE(StealthAngleAluDeclared());
    Preferences::SetInt("zoom.stealth.fpp.hw_seed", 7);
#ifdef XP_WIN
    ASSERT_NE(StealthAngleAluDeclaredFeature(), nullptr) << aluClass;
    EXPECT_STREQ(StealthAngleAluDeclaredFeature(), kFeature);
    // what the wrappers in GLLibraryEGL.h call
    EXPECT_TRUE(StealthAngleAluDeclared());
    const EGLAttrib* got =
        StealthAngleAluPlatformAttribs(LOCAL_EGL_PLATFORM_ANGLE_ANGLE, nullptr);
    ASSERT_NE(got, nullptr);
    EXPECT_EQ(got[0], LOCAL_EGL_FEATURE_OVERRIDES_ENABLED_ANGLE);
#else
    EXPECT_EQ(StealthAngleAluDeclaredFeature(), nullptr);  // no D3D translation
#endif
  }
  Preferences::SetInt("zoom.stealth.webgl.alu_class", 0);
  EXPECT_EQ(StealthAngleAluDeclaredFeature(), nullptr);
  EXPECT_FALSE(StealthAngleAluDeclared());
  const EGLAttrib list[] = {LOCAL_EGL_NONE};
  EXPECT_EQ(StealthAngleAluPlatformAttribs(LOCAL_EGL_PLATFORM_ANGLE_ANGLE, list),
            list);
  // a class this build does not know declares nothing
  Preferences::SetInt("zoom.stealth.webgl.alu_class", 3);
  EXPECT_EQ(StealthAngleAluDeclaredFeature(), nullptr);
}

#ifdef XP_WIN

// The same GLSL translated to HLSL with the option off and on.
static std::string TranslateToHLSL(const char* aSource, bool aStealthAlu) {
  EXPECT_TRUE(sh::Initialize());
  ShBuiltInResources resources;
  sh::InitBuiltInResources(&resources);
  const ShHandle handle = sh::ConstructCompiler(
      0x8B30 /* GL_FRAGMENT_SHADER */, SH_WEBGL2_SPEC, SH_HLSL_4_1_OUTPUT,
      &resources);
  EXPECT_NE(handle, nullptr);
  if (!handle) return {};
  ShCompileOptions options;
  options.objectCode = 1;
  options.initOutputVariables = 1;
  options.clampIndirectArrayBounds = 1;
  options.validateAST = 0;
  options.stealthAluNvidia = aStealthAlu ? 1 : 0;
  const char* parts[1] = {aSource};
  std::string out;
  EXPECT_TRUE(sh::Compile(handle, parts, 1, options)) << sh::GetInfoLog(handle);
  out = sh::GetObjectCode(handle);
  sh::Destruct(handle);
  return out;
}

constexpr char kDotShader[] = R"GLSL(#version 300 es
precision highp float;
uniform vec4 a, b;
uniform vec3 c;
out vec4 o;
void main() {
  o = vec4(dot(a, b), dot(a.xyz, c), dot(a.xy, b.zw), dot(a.x, b.y));
})GLSL";

TEST(StealthAngleAlu, ClassZeroHLSLHasNoNvidiaHelper)
{
  const std::string off = TranslateToHLSL(kDotShader, false);
  EXPECT_NE(off.find("dot("), std::string::npos);
  EXPECT_EQ(off.find("nv_"), std::string::npos);
  EXPECT_EQ(off.find("precise"), std::string::npos);
}

TEST(StealthAngleAlu, TheNvidiaClassEmitsTheFmaChainForTwoThreeAndFour)
{
  const std::string on = TranslateToHLSL(kDotShader, true);
  EXPECT_NE(on.find("nv_dot4("), std::string::npos);
  EXPECT_NE(on.find("nv_dot3("), std::string::npos);
  EXPECT_NE(on.find("nv_dot2("), std::string::npos);
  // the helpers are in the shader, defined before they are used
  const size_t def = on.find("precise float nv_dot4(");
  ASSERT_NE(def, std::string::npos);
  EXPECT_GT(on.rfind("nv_dot4("), def + 20);
  // dot of two scalars is a product: left as written
  EXPECT_NE(on.find("dot(_a.x, _b.y)"), std::string::npos);
  // not the same shader as class 0
  EXPECT_NE(on, TranslateToHLSL(kDotShader, false));
}

// A shader with no dot of two to four components: the option changes nothing.
TEST(StealthAngleAlu, ANeutralShaderIsTheSameInBothClasses)
{
  const char kNeutral[] = R"GLSL(#version 300 es
precision highp float;
uniform vec4 a;
out vec4 o;
void main() { o = a * 2.0 + vec4(length(a)); })GLSL";
  EXPECT_EQ(TranslateToHLSL(kNeutral, false), TranslateToHLSL(kNeutral, true));
}

#endif  // XP_WIN

}  // namespace mozilla::gl
