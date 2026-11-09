/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// The output colour profile the engine declares (gfx/thebes/StealthColorProfile.cpp).
// Retail Firefox on Windows converts an image tagged iCCP, cICP, gAMA+cHRM, sRGB or a JPEG
// ICC profile to the system's sRGB profile ("sRGB Color Space Profile.icm", which qcms
// does not take for its own sRGB), where the engine used to skip the conversion with
// gfx.color_management.force_srgb and so differed from retail by 1 on 27 of 72 probes.
// And the parent process read the host's profile (Windows GetICMProfileW, Linux's
// _ICC_PROFILE and EDID atoms) and sent it to the content processes.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "StealthColorProfile.h"
#include "gfxPlatform.h"
#include "gtest/gtest.h"
#include "mozilla/Preferences.h"
#include "mozilla/gtest/MozHelpers.h"
#include "nsCOMPtr.h"
#include "nsDirectoryServiceDefs.h"
#include "nsDirectoryServiceUtils.h"
#include "nsIFile.h"
#include "qcms.h"
#if defined(XP_WIN)
#  include "gfxWindowsPlatform.h"
#endif

using namespace mozilla;
using namespace mozilla::gfx;

namespace {

// SHA-256 (FIPS 180-4), enough to pin the bytes without a crypto dependency.
class Sha256 {
 public:
  void Update(const uint8_t* aData, size_t aLen) {
    mLength += aLen;
    while (aLen > 0) {
      size_t n = 64 - mFill;
      if (n > aLen) n = aLen;
      std::memcpy(mBlock + mFill, aData, n);
      mFill += n;
      aData += n;
      aLen -= n;
      if (mFill == 64) {
        Block();
        mFill = 0;
      }
    }
  }
  std::string Hex() {
    uint64_t bits = mLength * 8;
    uint8_t pad = 0x80;
    Update(&pad, 1);
    uint8_t zero = 0;
    while (mFill != 56) Update(&zero, 1);
    uint8_t len[8];
    for (int i = 0; i < 8; i++) len[i] = uint8_t(bits >> (56 - 8 * i));
    Update(len, 8);
    static const char kHex[] = "0123456789abcdef";
    std::string out;
    for (uint32_t h : mState) {
      for (int i = 28; i >= 0; i -= 4) out += kHex[(h >> i) & 0xf];
    }
    return out;
  }

 private:
  static uint32_t Rotr(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }
  void Block() {
    static const uint32_t kK[64] = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1,
        0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
        0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
        0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
        0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
        0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
        0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
        0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
        0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
        0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
        0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
    uint32_t w[64];
    for (int i = 0; i < 16; i++) {
      w[i] = (uint32_t(mBlock[4 * i]) << 24) | (uint32_t(mBlock[4 * i + 1]) << 16) |
             (uint32_t(mBlock[4 * i + 2]) << 8) | uint32_t(mBlock[4 * i + 3]);
    }
    for (int i = 16; i < 64; i++) {
      uint32_t s0 = Rotr(w[i - 15], 7) ^ Rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
      uint32_t s1 = Rotr(w[i - 2], 17) ^ Rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
      w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = mState[0], b = mState[1], c = mState[2], d = mState[3];
    uint32_t e = mState[4], f = mState[5], g = mState[6], h = mState[7];
    for (int i = 0; i < 64; i++) {
      uint32_t s1 = Rotr(e, 6) ^ Rotr(e, 11) ^ Rotr(e, 25);
      uint32_t ch = (e & f) ^ (~e & g);
      uint32_t t1 = h + s1 + ch + kK[i] + w[i];
      uint32_t s0 = Rotr(a, 2) ^ Rotr(a, 13) ^ Rotr(a, 22);
      uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
      uint32_t t2 = s0 + maj;
      h = g;
      g = f;
      f = e;
      e = d + t1;
      d = c;
      c = b;
      b = a;
      a = t1 + t2;
    }
    mState[0] += a;
    mState[1] += b;
    mState[2] += c;
    mState[3] += d;
    mState[4] += e;
    mState[5] += f;
    mState[6] += g;
    mState[7] += h;
  }

  uint32_t mState[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                        0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
  uint8_t mBlock[64];
  size_t mFill = 0;
  uint64_t mLength = 0;
};

std::string Sha256Hex(const uint8_t* aData, size_t aLen) {
  Sha256 sha;
  sha.Update(aData, aLen);
  return sha.Hex();
}

// The whole profile, as gen_stealth_color_profile.py --sha256 assembles it, and its 1024
// point curve (the 2048 bytes after the 12 byte `curv` header). The source of the curve is
// the Windows 11 build 26100/26300 file "sRGB Color Space Profile.icm" (sha256
// 2b3aa1645779a9e634744faf9b01e9102b0c9b88fd6deced7934df86b949af7e): 1024 of 1024 points
// equal in each channel.
constexpr char kProfileSha256[] =
    "7ee2860cb53a66709190faf0b96ec8deed05438b298c0adf0046539f146c2e09";
constexpr char kCurveSha256[] =
    "e14a37e71455047989aa09e8956a794affe1b914a4d00a151949bcbb29ea8eba";
constexpr uint32_t kCurveDataOffset = 296 + 12;

uint32_t Read32(const nsTArray<uint8_t>& aData, uint32_t aOffset) {
  return (uint32_t(aData[aOffset]) << 24) | (uint32_t(aData[aOffset + 1]) << 16) |
         (uint32_t(aData[aOffset + 2]) << 8) | uint32_t(aData[aOffset + 3]);
}

void Write32(nsTArray<uint8_t>& aData, uint32_t aOffset, int32_t aValue) {
  aData[aOffset] = uint8_t(uint32_t(aValue) >> 24);
  aData[aOffset + 1] = uint8_t(uint32_t(aValue) >> 16);
  aData[aOffset + 2] = uint8_t(uint32_t(aValue) >> 8);
  aData[aOffset + 3] = uint8_t(uint32_t(aValue));
}

// A wide gamut display profile, made by hand: the declared profile with the colorants of
// Display P3 (adapted to D50), which is what a monitor profile of a wide gamut screen has.
nsTArray<uint8_t> WideGamutProfile() {
  nsTArray<uint8_t> data = StealthSystemColorProfileData();
  struct Colorant {
    const char* mSig;
    int32_t mX, mY, mZ;
  };
  const Colorant kP3[] = {{"rXYZ", 33758, 15807, -72},
                          {"gXYZ", 19137, 45363, 2746},
                          {"bXYZ", 10296, 4365, 51386}};
  uint32_t count = Read32(data, 128);
  for (const Colorant& c : kP3) {
    for (uint32_t i = 0; i < count; i++) {
      uint32_t entry = 132 + 12 * i;
      if (std::memcmp(&data[entry], c.mSig, 4) == 0) {
        uint32_t at = Read32(data, entry + 4);
        Write32(data, at + 8, c.mX);
        Write32(data, at + 12, c.mY);
        Write32(data, at + 16, c.mZ);
      }
    }
  }
  return data;
}

struct Rgb {
  uint8_t r, g, b;
};

// One pixel from a profile to another, as image decoders do it: the output profile has its
// transform precached first (gfxPlatform::InitializeCMS).
Rgb Convert(qcms_profile* aIn, qcms_profile* aOut, Rgb aColor) {
  qcms_profile_precache_output_transform(aOut);
  qcms_transform* t = qcms_transform_create(aIn, QCMS_DATA_RGB_8, aOut,
                                            QCMS_DATA_RGB_8, QCMS_INTENT_PERCEPTUAL);
  Rgb out = {0, 0, 0};
  if (t) {
    qcms_transform_data(t, &aColor, &out, 1);
    qcms_transform_release(t);
  }
  return out;
}

bool Near(Rgb aA, Rgb aB, int aTolerance) {
  auto d = [](uint8_t x, uint8_t y) { return x > y ? x - y : y - x; };
  return d(aA.r, aB.r) <= aTolerance && d(aA.g, aB.g) <= aTolerance &&
         d(aA.b, aB.b) <= aTolerance;
}

bool Same(Rgb aA, Rgb aB) { return Near(aA, aB, 0); }

}  // namespace

TEST(StealthColorProfile, TheBytesAreFixed)
{
  nsTArray<uint8_t> data = StealthSystemColorProfileData();
  ASSERT_EQ(kStealthColorProfileSize, data.Length());
  EXPECT_EQ(kProfileSha256, Sha256Hex(data.Elements(), data.Length()));
  EXPECT_EQ(kCurveSha256, Sha256Hex(data.Elements() + kCurveDataOffset,
                                    2 * kStealthColorProfileCurvePoints));
  // The same bytes every time: nothing in them depends on the machine or on the call.
  EXPECT_EQ(data, StealthSystemColorProfileData());
}

TEST(StealthColorProfile, TheHeaderIsAnIccV2MonitorProfile)
{
  nsTArray<uint8_t> data = StealthSystemColorProfileData();
  ASSERT_EQ(kStealthColorProfileSize, data.Length());
  EXPECT_EQ(data.Length(), Read32(data, 0));
  EXPECT_EQ(0x02100000u, Read32(data, 8));
  EXPECT_EQ(0x6d6e7472u, Read32(data, 12));  // mntr
  EXPECT_EQ(0x52474220u, Read32(data, 16));  // RGB
  EXPECT_EQ(0x58595a20u, Read32(data, 20));  // XYZ
  EXPECT_EQ(0x61637370u, Read32(data, 36));  // acsp
  EXPECT_EQ(0u, Read32(data, 64));           // perceptual
  EXPECT_EQ(0x0000f6d6u, Read32(data, 68));
  EXPECT_EQ(0x00010000u, Read32(data, 72));
  EXPECT_EQ(0x0000d32du, Read32(data, 76));
}

TEST(StealthColorProfile, QcmsReadsItAsARealProfileNotItsOwnSRGB)
{
  nsTArray<uint8_t> data = StealthSystemColorProfileData();
  // What gfxPlatform::InitializeCMS does with the platform's data.
  qcms_profile* profile =
      qcms_profile_from_memory_curves_only(data.Elements(), data.Length());
  ASSERT_TRUE(profile);
  EXPECT_FALSE(qcms_profile_is_bogus(profile));
  // Retail Firefox's output profile on Windows is a profile qcms read from memory, so
  // qcms_profile_is_sRGB is false and image decoders convert to it.
  EXPECT_FALSE(qcms_profile_is_sRGB(profile));
  EXPECT_EQ(QCMS_INTENT_PERCEPTUAL, qcms_profile_get_rendering_intent(profile));
  EXPECT_EQ(icSigRgbData, qcms_profile_get_color_space(profile));
  qcms_profile_release(profile);

  qcms_profile* full = qcms_profile_from_memory(data.Elements(), data.Length());
  ASSERT_TRUE(full);
  EXPECT_FALSE(qcms_profile_is_sRGB(full));
  qcms_profile_release(full);
}

// Values qcms gives through the profile, measured with the qcms of this tree on x86-64
// (SSE2 and AVX agree; the scalar path of qcms can differ by 1 elsewhere).
TEST(StealthColorProfile, TheTransformIsTheOneOfTheWindowsProfile)
{
  nsTArray<uint8_t> data = StealthSystemColorProfileData();
  qcms_profile* out =
      qcms_profile_from_memory_curves_only(data.Elements(), data.Length());
  ASSERT_TRUE(out);
  qcms_profile* srgb = qcms_profile_sRGB();
  qcms_profile* p3 = qcms_profile_displayP3();
  ASSERT_TRUE(srgb);
  ASSERT_TRUE(p3);

  // A pure green is where converting sRGB to this profile is not the identity (it becomes
  // 1,255,0): the footprint that a skipped conversion, which is what force_srgb did, lacks.
  Rgb green = Convert(srgb, out, {0, 255, 0});
  EXPECT_EQ(1, green.r);
  EXPECT_EQ(255, green.g);
  EXPECT_EQ(0, green.b);
  EXPECT_TRUE(Same(Convert(srgb, out, {255, 255, 255}), {255, 255, 255}));
  EXPECT_TRUE(Same(Convert(srgb, out, {0, 0, 0}), {0, 0, 0}));
  EXPECT_TRUE(Near(Convert(srgb, out, {200, 30, 60}), {200, 30, 60}, 1));

  // A tagged wide gamut image: Display P3 pixels converted to the profile.
  struct Case {
    Rgb mIn, mOut;
  };
  const Case kP3[] = {{{200, 30, 60}, {218, 0, 54}},
                      {{12, 200, 255}, {0, 204, 255}},
                      {{100, 150, 200}, {83, 152, 205}},
                      {{180, 120, 90}, {190, 117, 84}},
                      {{64, 64, 200}, {64, 64, 208}},
                      {{220, 200, 40}, {224, 199, 0}},
                      {{128, 128, 128}, {128, 128, 128}}};
  for (const Case& c : kP3) {
    Rgb got = Convert(p3, out, c.mIn);
    EXPECT_TRUE(Near(got, c.mOut, 1))
        << int(c.mIn.r) << "," << int(c.mIn.g) << "," << int(c.mIn.b) << " -> "
        << int(got.r) << "," << int(got.g) << "," << int(got.b);
  }
  qcms_profile_release(p3);
  qcms_profile_release(srgb);
  qcms_profile_release(out);
}

TEST(StealthColorProfile, AWideGamutProfileIsNotThisOne)
{
  // The fixture of the tests below really is another profile.
  nsTArray<uint8_t> wide = WideGamutProfile();
  nsTArray<uint8_t> declared = StealthSystemColorProfileData();
  ASSERT_EQ(declared.Length(), wide.Length());
  EXPECT_NE(declared, wide);
  qcms_profile* a =
      qcms_profile_from_memory_curves_only(wide.Elements(), wide.Length());
  qcms_profile* b = qcms_profile_from_memory_curves_only(declared.Elements(),
                                                         declared.Length());
  ASSERT_TRUE(a);
  ASSERT_TRUE(b);
  EXPECT_FALSE(qcms_profile_is_bogus(a));
  qcms_profile* srgb = qcms_profile_sRGB();
  EXPECT_FALSE(
      Same(Convert(srgb, a, {0, 255, 0}), Convert(srgb, b, {0, 255, 0})));
  qcms_profile_release(srgb);
  qcms_profile_release(a);
  qcms_profile_release(b);
}

#if defined(GTEST_HAS_DEATH_TEST) && !defined(XP_DARWIN)

namespace {

// The wide gamut profile as a file, in a temporary directory, for the pref
// gfx.color_management.display_profile. Returns the native path, empty on failure.
nsCString WriteWideGamutFile(nsCOMPtr<nsIFile>& aFile) {
  nsCString path;
  if (NS_FAILED(NS_GetSpecialDirectory(NS_OS_TEMP_DIR, getter_AddRefs(aFile))) ||
      NS_FAILED(aFile->AppendNative("stealth-color-profile-wide.icc"_ns)) ||
      NS_FAILED(aFile->CreateUnique(nsIFile::NORMAL_FILE_TYPE, 0600))) {
    return path;
  }
  FILE* fp = nullptr;
  if (NS_FAILED(aFile->OpenANSIFileDesc("wb", &fp)) || !fp) {
    return path;
  }
  nsTArray<uint8_t> wide = WideGamutProfile();
  bool wrote = fwrite(wide.Elements(), 1, wide.Length(), fp) == wide.Length();
  fclose(fp);
  if (wrote) {
    aFile->GetNativePath(path);
  }
  return path;
}

// Exit codes of the children.
constexpr int kOk = 0;
constexpr int kWrongTransform = 3;
constexpr int kNoFile = 78;

enum class Engine { Off, On };

#  if defined(MOZ_WIDGET_GTK)
constexpr int kNoOutputProfile = 1;
constexpr int kOutputIsInternalSRGB = 2;
constexpr int kNotFresh = 77;

// Copied verbatim from gfx/tests/gtest/TestStealthIdwrTextVars.cpp (kUhd620): any complete
// declaration serves, the children below only need the gate to accept one.
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

void DeclareEngine() {
  Preferences::SetCString("zoom.stealth.gfx.feature_status", kGfxFeatureStatus);
  Preferences::SetInt("zoom.stealth.text.system_quality", 5);
  Preferences::SetInt("gfx.font_rendering.cleartype_params.rendering_mode", 0);
  Preferences::SetInt("zoom.stealth.fpp.hw_seed", 7);
}

// A fresh process, the prefs a launcher and a user could have set, then the platform: what
// gfxPlatform::InitializeCMS made of the output profile. The expected conversion of sRGB
// green is the declared profile's (1,255,0) with the engine on, the wide gamut file's with
// it off and the pref set, and with force_srgb and the engine off, no conversion at all.
[[noreturn]] void ChildOutputProfile(Engine aEngine, bool aForceSRGB,
                                     bool aPointAtFile) {
  mozilla::gtest::DisableCrashReporter();
  if (gfxPlatform::Initialized()) {
    // The test's own assumption, not the code under test: the child must be a fresh
    // process.
    std::_Exit(kNotFresh);
  }
  nsCOMPtr<nsIFile> file;
  nsCString path = WriteWideGamutFile(file);
  if (path.IsEmpty()) {
    std::_Exit(kNoFile);
  }
  if (aPointAtFile) {
    Preferences::SetCString("gfx.color_management.display_profile", path);
  }
  if (aForceSRGB) {
    Preferences::SetBool("gfx.color_management.force_srgb", true);
  }
  if (aEngine == Engine::On) {
    DeclareEngine();
  }
  gfxPlatform::GetPlatform();
  qcms_profile* out = gfxPlatform::GetCMSOutputProfile();
  qcms_profile* srgb = qcms_profile_sRGB();
  int code = kOk;
  if (!out) {
    code = kNoOutputProfile;
  } else if (aEngine == Engine::On) {
    nsTArray<uint8_t> declared = StealthSystemColorProfileData();
    qcms_profile* expected = qcms_profile_from_memory_curves_only(
        declared.Elements(), declared.Length());
    if (qcms_profile_is_sRGB(out)) {
      code = kOutputIsInternalSRGB;
    } else {
      const Rgb kColors[] = {
          {0, 255, 0}, {200, 30, 60}, {12, 200, 255}, {0, 225, 0}};
      for (Rgb c : kColors) {
        if (!Same(Convert(srgb, out, c), Convert(srgb, expected, c))) {
          code = kWrongTransform;
        }
      }
      if (!Same(Convert(srgb, out, {0, 255, 0}), {1, 255, 0})) {
        code = kWrongTransform;
      }
    }
  } else if (aForceSRGB) {
    // Stock Firefox: force_srgb is the internal sRGB.
    if (!qcms_profile_is_sRGB(out)) code = kWrongTransform;
  } else {
    // Stock Firefox: the profile of the pref's file.
    nsTArray<uint8_t> wide = WideGamutProfile();
    qcms_profile* expected =
        qcms_profile_from_memory_curves_only(wide.Elements(), wide.Length());
    if (qcms_profile_is_sRGB(out) ||
        !Same(Convert(srgb, out, {0, 255, 0}),
              Convert(srgb, expected, {0, 255, 0}))) {
      code = kWrongTransform;
    }
  }
  file->Remove(false);
  std::_Exit(code);
}
#  endif  // MOZ_WIDGET_GTK

#  if defined(XP_WIN)
// The Windows platform function, without building the platform: it is static.
[[noreturn]] void ChildWindowsProfileData(Engine aEngine) {
  mozilla::gtest::DisableCrashReporter();
  nsCOMPtr<nsIFile> file;
  nsCString path = WriteWideGamutFile(file);
  if (path.IsEmpty()) {
    std::_Exit(kNoFile);
  }
  Preferences::SetCString("gfx.color_management.display_profile", path);
  if (aEngine == Engine::On) {
    Preferences::SetInt("zoom.stealth.fpp.hw_seed", 7);
  }
  nsTArray<uint8_t> got =
      gfxWindowsPlatform::GetPlatformCMSOutputProfileData_Impl();
  nsTArray<uint8_t> expected = aEngine == Engine::On
                                   ? StealthSystemColorProfileData()
                                   : WideGamutProfile();
  file->Remove(false);
  std::_Exit(got == expected ? kOk : kWrongTransform);
}
#  endif  // XP_WIN

}  // namespace

#  if defined(MOZ_WIDGET_GTK)
// Before: with the engine on and gfx.color_management.display_profile set, the pref's file
// was the output profile (a launcher that sets the pref, or a user, made the page's
// converted colours depend on a file).
TEST(StealthColorProfile, TheEngineIgnoresTheDisplayProfilePref)
{
  GTEST_FLAG_SET(death_test_style, "threadsafe");
  EXPECT_EXIT(ChildOutputProfile(Engine::On, false, true),
              ::testing::ExitedWithCode(kOk), "");
}

// Before: force_srgb (which the core set) made the output profile qcms' internal sRGB, and
// the conversion of tagged images was skipped, which retail Firefox on Windows does not.
TEST(StealthColorProfile, TheEngineDoesNotLetForceSRGBReplaceTheProfile)
{
  GTEST_FLAG_SET(death_test_style, "threadsafe");
  EXPECT_EXIT(ChildOutputProfile(Engine::On, true, false),
              ::testing::ExitedWithCode(kOk), "");
}

TEST(StealthColorProfile, TheEngineOnWithNothingSetDeclaresTheProfile)
{
  GTEST_FLAG_SET(death_test_style, "threadsafe");
  EXPECT_EXIT(ChildOutputProfile(Engine::On, false, false),
              ::testing::ExitedWithCode(kOk), "");
}

// The engine off is stock Firefox: the pref's file is the output profile, force_srgb is the
// internal sRGB.
TEST(StealthColorProfile, WithTheEngineOffThePrefStillWins)
{
  GTEST_FLAG_SET(death_test_style, "threadsafe");
  EXPECT_EXIT(ChildOutputProfile(Engine::Off, false, true),
              ::testing::ExitedWithCode(kOk), "");
}

TEST(StealthColorProfile, WithTheEngineOffForceSRGBStillWins)
{
  GTEST_FLAG_SET(death_test_style, "threadsafe");
  EXPECT_EXIT(ChildOutputProfile(Engine::Off, true, true),
              ::testing::ExitedWithCode(kOk), "");
}
#  endif  // MOZ_WIDGET_GTK

#  if defined(XP_WIN)
TEST(StealthColorProfile, TheWindowsPlatformDoesNotReadTheHostOrThePref)
{
  GTEST_FLAG_SET(death_test_style, "threadsafe");
  EXPECT_EXIT(ChildWindowsProfileData(Engine::On), ::testing::ExitedWithCode(kOk),
              "");
}

TEST(StealthColorProfile, WithTheEngineOffTheWindowsPrefStillWins)
{
  GTEST_FLAG_SET(death_test_style, "threadsafe");
  EXPECT_EXIT(ChildWindowsProfileData(Engine::Off), ::testing::ExitedWithCode(kOk),
              "");
}
#  endif  // XP_WIN

#endif  // GTEST_HAS_DEATH_TEST && !XP_DARWIN
