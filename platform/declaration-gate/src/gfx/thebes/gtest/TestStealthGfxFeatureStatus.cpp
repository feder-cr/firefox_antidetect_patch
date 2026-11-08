/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// The graphics feature decisions from the declaration
// (gfx/thebes/StealthGfxFeatureStatus.cpp). The two declarations below are
// retail Firefox 159 on Windows 11 measured for a real device each: an Intel
// UHD 620 (driver 31.0.101.2145) and an NVIDIA RTX 3060 (driver 32.0.15.6210).

#include "GfxInfoBase.h"
#include "StealthGfxFeatureStatus.h"
#include "gtest/gtest.h"
#include "mozilla/Components.h"
#include "mozilla/Preferences.h"
#include "mozilla/ScopeExit.h"
#include "nsIGfxInfo.h"
#include "nsIObserver.h"

using namespace mozilla;
using namespace mozilla::gfx;

namespace {

const char kUhd620[] =
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

const char kRtx3060[] =
    "DIRECT3D_9_LAYERS|1,DIRECT3D_10_LAYERS|1,DIRECT3D_10_1_LAYERS|1,OPENGL"
    "_LAYERS|1,WEBGL_OPENGL|1,WEBGL_ANGLE|1,WEBRTC_HW_ACCELERATION_H264|1,D"
    "IRECT3D_11_LAYERS|1,HARDWARE_VIDEO_DECODING|1,HARDWARE_VIDEO_DECODING_"
    "VULKAN|1,DIRECT3D_11_ANGLE|1,WEBRTC_HW_ACCELERATION_ENCODE|1,WEBRTC_HW"
    "_ACCELERATION_DECODE|1,CANVAS2D_ACCELERATION|1,VP8_HW_DECODE|1,VP9_HW_"
    "DECODE|1,DX_INTEROP2|1,GPU_PROCESS|1,WEBGL2|1,D3D11_KEYED_MUTEX|1,WEBR"
    "ENDER|1,DX_NV12|1,DX_P010|1,DX_P016|1,GL_SWIZZLE|1,WEBRENDER_COMPOSITO"
    "R|1,WEBRENDER_SCISSORED_CACHE_CLEARS|1,THREADSAFE_GL|1,WEBRENDER_OPTIM"
    "IZED_SHADERS|1,X11_EGL|1,DMABUF|1,WEBRENDER_SHADER_CACHE|1,WEBRENDER_P"
    "ARTIAL_PRESENT|1,WEBGPU|1,VIDEO_OVERLAY|1,HW_DECODED_VIDEO_ZERO_COPY|9"
    "|FEATURE_ROLLOUT_ALL,DMABUF_SURFACE_EXPORT|1,REUSE_DECODER_DEVICE|1,BA"
    "CKDROP_FILTER|1,ACCELERATED_CANVAS2D|1,H264_HW_DECODE|1,AV1_HW_DECODE|"
    "1,HEVC_HW_DECODE|1,VIDEO_SOFTWARE_OVERLAY|1,WEBGL_USE_HARDWARE|1,OVERL"
    "AY_VP_AUTO_HDR|1,OVERLAY_VP_SUPER_RESOLUTION|1,DMABUF_WEBGL|1,WEBRENDE"
    "R_DCOMP|1,VP8_HW_ENCODE|1,VP9_HW_ENCODE|1,H264_HW_ENCODE|1,AV1_HW_ENCO"
    "DE|1,HEVC_HW_ENCODE|1,WMF_HW_DRM|1,GL_NORM16_TEXTURES|1,HARDWARE_VIDEO"
    "_ENCODING|1,WEBGPU_EXTERNAL_TEXTURE|1,MESA_THREADING|1,WEBGL|1,VIDEO_H"
    "DR|1,WEBGL_ANGLE_METAL|1,WEBRENDER_ANGLE_METAL|1";

constexpr char kSeedPref[] = "zoom.stealth.fpp.hw_seed";
constexpr char kDeclPref[] = "zoom.stealth.gfx.feature_status";

bool Parses(const nsACString& aDecl) {
  StealthGfxFeatureTable table;
  nsAutoCString error;
  return StealthParseGfxFeatureStatus(aDecl, table, error);
}

nsCString Replaced(const char* aFrom, const char* aTo) {
  nsCString decl(kUhd620);
  decl.ReplaceSubstring(aFrom, aTo);
  return decl;
}

// The engine on, declaring aDecl; everything back as it was at the end.
class Declared {
 public:
  explicit Declared(const char* aDecl) {
    Preferences::SetInt(kSeedPref, 7);
    Preferences::SetCString(kDeclPref, nsDependentCString(aDecl));
    StealthResetGfxFeatureStatusForTesting();
  }
  ~Declared() {
    Preferences::ClearUser(kSeedPref);
    Preferences::ClearUser(kDeclPref);
    StealthResetGfxFeatureStatusForTesting();
  }
};

bool LiveFeature(int32_t aId) {
  return !widget::GfxInfoBase::IsFeatureRetired(aId);
}

}  // namespace

TEST(StealthGfxFeatureStatus, TheMeasuredRetailDeclarationsParse)
{
  StealthGfxFeatureTable table;
  nsAutoCString error;
  ASSERT_TRUE(StealthParseGfxFeatureStatus(nsDependentCString(kUhd620), table,
                                           error))
  << error.get();
  ASSERT_TRUE(table[nsIGfxInfo::FEATURE_WEBGL_OPENGL]);
  EXPECT_EQ(table[nsIGfxInfo::FEATURE_WEBGL_OPENGL]->mStatus,
            int32_t(nsIGfxInfo::FEATURE_DISCOURAGED));
  EXPECT_TRUE(table[nsIGfxInfo::FEATURE_WEBGL_OPENGL]->mFailureId.EqualsLiteral(
      "WEBGL_NATIVE_GL_OLD_INTEL"));
  ASSERT_TRUE(StealthParseGfxFeatureStatus(nsDependentCString(kRtx3060),
                                           table, error))
  << error.get();
  // Zero-copy video is an allowlist feature: retail answers ALLOW_ALWAYS, and
  // its consumers need exactly that.
  ASSERT_TRUE(table[nsIGfxInfo::FEATURE_HW_DECODED_VIDEO_ZERO_COPY]);
  EXPECT_EQ(table[nsIGfxInfo::FEATURE_HW_DECODED_VIDEO_ZERO_COPY]->mStatus,
            int32_t(nsIGfxInfo::FEATURE_ALLOW_ALWAYS));
}

TEST(StealthGfxFeatureStatus, EveryBrokenRuleIsRefused)
{
  const nsCString broken[] = {
      ""_ns,
      // A live feature missing.
      Replaced(",WEBGL2|1,", ","),
      // Declared twice.
      Replaced(",WEBGL2|1,", ",WEBGL2|1,WEBGL2|1,"),
      // Unknown, retired, a blocklist wildcard.
      Replaced(",WEBGL2|1,", ",WEBGL2|1,NOT_A_FEATURE|1,"),
      Replaced(",WEBGL2|1,", ",WEBGL2|1,WEBGL_MSAA|1,"),
      Replaced(",WEBGL2|1,", ",WEBGL2|1,ALL|1,"),
      // Statuses retail never gives: INVALID, UNKNOWN, past the last, text.
      Replaced(",WEBGL2|1,", ",WEBGL2|0,"),
      Replaced(",WEBGL2|1,", ",WEBGL2|2,"),
      Replaced(",WEBGL2|1,", ",WEBGL2|12|FEATURE_FAILURE_X,"),
      Replaced(",WEBGL2|1,", ",WEBGL2|ok,"),
      // OK with a failure id, a block without one.
      Replaced(",WEBGL2|1,", ",WEBGL2|1|FEATURE_FAILURE_X,"),
      Replaced(",WEBGL2|1,", ",WEBGL2|4,"),
      // ALLOW_* on a feature not on an allowlist, OK on one that is.
      Replaced(",WEBGL2|1,", ",WEBGL2|9|FEATURE_ROLLOUT_ALL,"),
      Replaced("HW_DECODED_VIDEO_ZERO_COPY|3|FEATURE_FAILURE_BUG_1802357",
               "HW_DECODED_VIDEO_ZERO_COPY|1"),
      // A failure id outside [A-Za-z0-9_], a space, a fourth field.
      Replaced(",WEBGL2|1,", ",WEBGL2|4|BAD-ID,"),
      Replaced(",WEBGL2|1,", ", WEBGL2|1,"),
      Replaced(",WEBGL2|1,", ",WEBGL2|4|A|B,"),
  };
  for (const auto& decl : broken) {
    EXPECT_FALSE(Parses(decl)) << decl.get();
  }
}

// On GTK the only WebGL path is native GL, which WEBGL_OPENGL gates; on
// Windows it gates WGL, which retail never takes (ANGLE is the path). On GTK
// the answer is the declared WEBGL one and nothing the host has or lacks moves
// it: the GL that serves WebGL ships with the engine, so a host with no Mesa
// still has WebGL2 (before, the host's glxtest blocked it).
TEST(StealthGfxFeatureStatus, WebglOpenglGatesTheWebglPathOfThisPlatform)
{
  StealthGfxFeatureTable table;
  nsAutoCString error;
  ASSERT_TRUE(StealthParseGfxFeatureStatus(nsDependentCString(kUhd620), table,
                                           error));
  nsAutoCString failure;
  int32_t status = 0;
  ASSERT_TRUE(StealthAnswerGfxFeature(table, nsIGfxInfo::FEATURE_WEBGL_OPENGL,
                                      failure, &status));
#ifdef MOZ_WIDGET_GTK
  EXPECT_EQ(status, int32_t(nsIGfxInfo::FEATURE_STATUS_OK));
  EXPECT_TRUE(failure.IsEmpty());
  nsAutoCString webglFailure;
  int32_t webglStatus = 0;
  ASSERT_TRUE(StealthAnswerGfxFeature(table, nsIGfxInfo::FEATURE_WEBGL,
                                      webglFailure, &webglStatus));
  EXPECT_EQ(status, webglStatus);
  EXPECT_TRUE(failure.Equals(webglFailure));
#else
  EXPECT_EQ(status, int32_t(nsIGfxInfo::FEATURE_DISCOURAGED));
  EXPECT_TRUE(failure.EqualsLiteral("WEBGL_NATIVE_GL_OLD_INTEL"));
#endif
  // Outside the live set nothing is answered: Mozilla's path keeps it.
  EXPECT_FALSE(StealthAnswerGfxFeature(table, nsIGfxInfo::FEATURE_INVALID,
                                       failure, &status));
}

// Every live feature, through the real nsIGfxInfo, answers the declared
// status. A gfx.blacklist.* pref (a persisted override, or the downloadable
// list's) no longer outranks it.
TEST(StealthGfxFeatureStatus, EveryLiveFeatureAnswersFromTheDeclaration)
{
  Declared declared(kUhd620);
  Preferences::SetInt("gfx.blacklist.webgl2",
                      nsIGfxInfo::FEATURE_BLOCKED_DEVICE);
  auto restore =
      MakeScopeExit([] { Preferences::ClearUser("gfx.blacklist.webgl2"); });
  nsCOMPtr<nsIGfxInfo> gfxInfo = components::GfxInfo::Service();
  ASSERT_TRUE(gfxInfo);

  StealthGfxFeatureTable table;
  nsAutoCString error;
  ASSERT_TRUE(StealthParseGfxFeatureStatus(nsDependentCString(kUhd620), table,
                                           error));
  for (int32_t id = nsIGfxInfo::FEATURE_START; id < nsIGfxInfo::FEATURE_COUNT;
       ++id) {
    if (!LiveFeature(id)) {
      continue;
    }
    nsAutoCString failure;
    int32_t status = nsIGfxInfo::FEATURE_STATUS_INVALID;
    ASSERT_EQ(gfxInfo->GetFeatureStatus(id, failure, &status), NS_OK) << id;
    nsAutoCString wantFailure;
    int32_t want = 0;
    ASSERT_TRUE(StealthAnswerGfxFeature(table, id, wantFailure, &want));
    EXPECT_EQ(status, want) << id;
    EXPECT_TRUE(failure.Equals(wantFailure)) << id;
  }
  nsAutoCString failure;
  int32_t status = 0;
  ASSERT_EQ(gfxInfo->GetFeatureStatus(nsIGfxInfo::FEATURE_WEBGL2, failure,
                                      &status),
            NS_OK);
  EXPECT_EQ(status, int32_t(nsIGfxInfo::FEATURE_STATUS_OK));
}

// The downloadable blocklist is evaluated against the host's adapter and
// written into the profile as gfx.blacklist.* prefs. While declared it is not
// evaluated: the profile is left as it was.
TEST(StealthGfxFeatureStatus, TheDownloadedBlocklistLeavesTheProfileAlone)
{
  Declared declared(kUhd620);
  Preferences::SetInt("gfx.blacklist.webgl2", nsIGfxInfo::FEATURE_DISCOURAGED);
  auto restore =
      MakeScopeExit([] { Preferences::ClearUser("gfx.blacklist.webgl2"); });
  nsCOMPtr<nsIGfxInfo> gfxInfo = components::GfxInfo::Service();
  nsCOMPtr<nsIObserver> observer = do_QueryInterface(gfxInfo);
  ASSERT_TRUE(observer);
  // Before the hook the list was evaluated: this entry either matched (the
  // pref became BLOCKED_DEVICE) or did not (the pref was removed).
  observer->Observe(nullptr, "blocklist-data-gfxItems",
                    u"os:All\tfeature:WEBGL2\tfeatureStatus:BLOCKED_DEVICE"
                    u"\tblockID:stealth");
  EXPECT_EQ(Preferences::GetInt("gfx.blacklist.webgl2", -1),
            int32_t(nsIGfxInfo::FEATURE_DISCOURAGED));
}

TEST(StealthGfxFeatureStatus, NoSuggestedDriverVersionWhileDeclared)
{
  Declared declared(kRtx3060);
  Preferences::SetCString("gfx.blacklist.suggested-driver-version", "1.2.3"_ns);
  auto restore = MakeScopeExit([] {
    Preferences::ClearUser("gfx.blacklist.suggested-driver-version");
  });
  nsCOMPtr<nsIGfxInfo> gfxInfo = components::GfxInfo::Service();
  nsAutoString version;
  ASSERT_EQ(gfxInfo->GetFeatureSuggestedDriverVersion(
                nsIGfxInfo::FEATURE_WEBGL2, version),
            NS_OK);
  EXPECT_TRUE(version.IsEmpty());
}
