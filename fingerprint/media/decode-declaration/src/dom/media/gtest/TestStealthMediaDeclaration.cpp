/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// The declared decode support (dom/media/StealthMediaDeclaration.*,
// zoom.stealth.media.decode_support): what a page learns about decoding comes
// from the declaration, not from the machine. PDMFactory has two query roads,
// Supports (sync) and SupportsAsync, and both must answer from it. Since 159
// the WebRTC branch of mediaCapabilities.decodingInfo asks SupportsAsync, which
// did not consult the declaration and leaked the host's real hardware-decode
// bit.
//
// The mime types below are made up on purpose: no platform module decodes
// them, so without the declaration every host answers the empty set, and a
// declared set can never be mistaken for what the host really reports.
//
// The WebRTC half (zoom.stealth.media.encode_support, the H.264 gate and the
// core count of encodingInfo's "smooth") is further down.

#include "MediaInfo.h"
#include "PDMFactory.h"
#include "PlatformDecoderModule.h"
#include "StealthMediaDeclaration.h"
#include "VideoUtils.h"
#include "gtest/gtest.h"
#include "mozilla/Preferences.h"
#include "mozilla/TaskQueue.h"
#include "mozilla/dom/MediaCapabilitiesBinding.h"
#include "mozilla/gtest/WaitFor.h"
#include "nsThreadUtils.h"

#include <iterator>

#ifdef MOZ_WEBRTC
#  include "EncoderConfig.h"
#  include "MediaCapabilities.h"
#  include "MediaMIMETypes.h"
#  include "WebrtcVideoCodecFactory.h"
#  include "js/Promise.h"
#  include "js/PropertyAndElement.h"
#  include "jsapi/DefaultCodecPreferences.h"
#  include "mozilla/ErrorResult.h"
#  include "mozilla/SpinEventLoopUntil.h"
#  include "mozilla/dom/MediaCapabilitiesBinding.h"
#  include "mozilla/dom/Promise.h"
#  include "mozilla/dom/ScriptSettings.h"
#  include "mozilla/gfx/gfxVars.h"
#  include "mozilla/media/webrtc/CodecInfo.h"
#  include "xpcpublic.h"
#endif

using namespace mozilla;
using media::DecodeSupport;
using media::DecodeSupportSet;

namespace {

constexpr char kDeclarationPref[] = "zoom.stealth.media.decode_support";
constexpr char kEncodeDeclarationPref[] = "zoom.stealth.media.encode_support";
constexpr char kCoresPref[] = "zoom.stealth.hw_concurrency";
constexpr char kDecodingInfoPref[] = "zoom.stealth.media.decoding_info";

// Newline separated "mime|flags" entries, see the pref's comment.
constexpr char kDeclaration[] =
    "video/x-stealth-sw|sw\n"
    "video/x-stealth-hw|hw\n"
    "video/x-stealth-swhw|swhw\n"
    "video/x-stealth-no|no";

// Sets a string pref, puts it back at the end of the test.
class DeclarationPref {
 public:
  DeclarationPref(const char* aName, const char* aSpec) : mName(aName) {
    Preferences::GetCString(mName, mOld);
    Preferences::SetCString(mName, nsDependentCString(aSpec));
  }
  ~DeclarationPref() { Preferences::SetCString(mName, mOld); }

 private:
  const char* mName;
  nsAutoCString mOld;
};

class DecodeSupportPref : public DeclarationPref {
 public:
  explicit DecodeSupportPref(const char* aSpec)
      : DeclarationPref(kDeclarationPref, aSpec) {}
};

// Sets an int pref, clears it at the end of the test.
class ScopedIntPref {
 public:
  ScopedIntPref(const char* aName, int32_t aValue) : mName(aName) {
    Preferences::SetInt(mName, aValue);
  }
  ~ScopedIntPref() { Preferences::ClearUser(mName); }

 private:
  const char* mName;
};

DecodeSupportSet SyncSupport(const char* aMime) {
  VideoInfo info;
  info.mMimeType = nsDependentCString(aMime);
  RefPtr<PDMFactory> factory = MakeRefPtr<PDMFactory>();
  return factory->Supports(SupportDecoderParams(info), nullptr);
}

// Off the main thread, as SupportsAsync requires.
DecodeSupportSet AsyncSupport(const char* aMime) {
  VideoInfo info;
  info.mMimeType = nsDependentCString(aMime);
  RefPtr<TaskQueue> queue = TaskQueue::Create(
      GetMediaThreadPool(MediaThreadType::SUPERVISOR), "TestStealthDecodeAsync");
  RefPtr<PDMFactory> factory = MakeRefPtr<PDMFactory>();
  DecodeSupportSet result = WaitForResolve(
      InvokeAsync(queue, __func__, [&info, factory]() {
        return factory->SupportsAsync(SupportDecoderParams(info));
      }));
  queue->BeginShutdown();
  queue->AwaitShutdownAndIdle();
  return result;
}

}  // namespace

TEST(StealthMediaDeclaration, SupportsAsyncAnswersFromTheDeclaration)
{
  DecodeSupportPref pref(kDeclaration);
  EXPECT_EQ(AsyncSupport("video/x-stealth-sw"),
            DecodeSupportSet{DecodeSupport::SoftwareDecode});
  EXPECT_EQ(AsyncSupport("video/x-stealth-hw"),
            DecodeSupportSet{DecodeSupport::HardwareDecode});
  EXPECT_EQ(AsyncSupport("video/x-stealth-swhw"),
            DecodeSupportSet(DecodeSupport::SoftwareDecode,
                             DecodeSupport::HardwareDecode));
  EXPECT_EQ(AsyncSupport("video/x-stealth-no"), DecodeSupportSet{});
}

// The two roads must never diverge: a page can reach either one.
TEST(StealthMediaDeclaration, SupportsAndSupportsAsyncAgree)
{
  DecodeSupportPref pref(kDeclaration);
  for (const char* mime :
       {"video/x-stealth-sw", "video/x-stealth-hw", "video/x-stealth-swhw",
        "video/x-stealth-no", "video/x-stealth-undeclared"}) {
    EXPECT_EQ(SyncSupport(mime), AsyncSupport(mime)) << mime;
  }
}

// An undeclared type falls through to the real answer, on both roads.
TEST(StealthMediaDeclaration, UndeclaredTypeFallsThrough)
{
  DecodeSupportPref pref(kDeclaration);
  EXPECT_EQ(AsyncSupport("video/x-stealth-undeclared"), DecodeSupportSet{});
}

// --- powerEfficient: the retail rule over the declared hardware bit -------

// Retail (MediaCapabilities.cpp, CreateVideoDecodingInfo): powerEfficient =
// hardware decode || width * height <= 640 * 480, for ANY codec, and smooth is
// always true for a supported file. The declared table carries the hardware
// half; the size half is StealthPowerEfficientFor.
namespace {

struct DecodingSize {
  uint32_t mWidth;
  uint32_t mHeight;
};

// The sizes of the grid: at most 640x480 (the boundary itself, a pixel count
// that is below although a side is above 640, a pixel past the boundary), then
// 480p, 720p, 1080p.
constexpr DecodingSize kSizes[] = {
    {320, 240}, {640, 480}, {720, 360}, {641, 480},
    {854, 480}, {1280, 720}, {1920, 1080},
};

bool IsAtMost640x480(DecodingSize aSize) {
  return uint64_t(aSize.mWidth) * aSize.mHeight <= 307200;
}

// Newline separated "container|family|supported smooth hardware", the rows of
// the table invisible_core declares (prefs.py _WIN_DECODING_INFO).
constexpr char kCoreTable[] =
    "video/mp4|avc1|111\n"
    "video/mp4|avc3|111\n"
    "video/mp4|hev1|111\n"
    "video/mp4|hvc1|111\n"
    "video/mp4|av01|111\n"
    "video/mp4|vp09|111\n"
    "video/webm|av01|111\n"
    "video/webm|vp09|111\n"
    "video/webm|vp8|110\n"
    "video/webm|vp9|110";

// The types Gecko's own gate (CanHandleContainerType) lets through on any host
// when the track mime types are declared: the table below declares them.
constexpr char kTrackTypes[] =
    "video/avc|sw\n"
    "video/hevc|sw\n"
    "video/vp8|sw\n"
    "video/vp9|sw\n"
    "video/av1|sw";

struct DecodingRow {
  const char* mContentType;
  bool mHardware;
  // hev1/hvc1 also depend on Gecko's own HEVC switch, which the declaration of
  // the track types does not open on every build: a row that Gecko's gate turns
  // away (Nothing) is not an answer to check, for these two only.
  bool mGeckoMayRefuse;
};

constexpr DecodingRow kRows[] = {
    {"video/mp4; codecs=\"avc1.640028\"", true, false},
    {"video/mp4; codecs=\"avc3.640028\"", true, false},
    {"video/mp4; codecs=\"hev1.1.6.L93.B0\"", true, true},
    {"video/mp4; codecs=\"hvc1.1.6.L93.B0\"", true, true},
    {"video/mp4; codecs=\"av01.0.05M.08\"", true, false},
    {"video/mp4; codecs=\"vp09.00.10.08\"", true, false},
    {"video/webm; codecs=\"av01.0.05M.08\"", true, false},
    {"video/webm; codecs=\"vp09.00.10.08\"", true, false},
    {"video/webm; codecs=\"vp8\"", false, false},
    {"video/webm; codecs=\"vp9\"", false, false},
};

class ScopedBoolPref {
 public:
  ScopedBoolPref(const char* aName, bool aValue) : mName(aName) {
    Preferences::SetBool(mName, aValue);
  }
  ~ScopedBoolPref() { Preferences::ClearUser(mName); }

 private:
  const char* mName;
};

// decodingInfo({type: "file", video}) as MediaCapabilities hands it to the
// declaration.
dom::MediaDecodingConfiguration FileConfig(const char* aContentType,
                                           DecodingSize aSize) {
  dom::MediaDecodingConfiguration config;
  config.mType = dom::MediaDecodingType::File;
  dom::VideoConfiguration& video = config.mVideo.Construct();
  video.mContentType = NS_ConvertUTF8toUTF16(nsDependentCString(aContentType));
  video.mWidth = aSize.mWidth;
  video.mHeight = aSize.mHeight;
  video.mBitrate = 1'000'000;
  video.mFramerate = 30.0;
  return config;
}

}  // namespace

// The rule itself, with no pref and no host: the declared hardware bit wins at
// any size; without it a picture is power efficient only up to 640x480 pixels
// in total. 65536x65536 overflows 32 bits, which retail does not count as low
// resolution, and a side above INT32_MAX is clamped first.
TEST(StealthMediaDeclaration, PowerEfficientIsHardwareOrAtMost640x480)
{
  for (const DecodingSize& size : kSizes) {
    EXPECT_TRUE(StealthPowerEfficientFor(true, size.mWidth, size.mHeight))
        << size.mWidth << "x" << size.mHeight << " with hardware";
    EXPECT_EQ(StealthPowerEfficientFor(false, size.mWidth, size.mHeight),
              IsAtMost640x480(size))
        << size.mWidth << "x" << size.mHeight << " without hardware";
  }
  EXPECT_TRUE(StealthPowerEfficientFor(false, 640, 480));
  EXPECT_FALSE(StealthPowerEfficientFor(false, 641, 480));
  // The threshold itself, from both sides and in the shapes a side can take:
  // 307200 pixels is low, 307201 and 640x481 (307840) are not.
  EXPECT_FALSE(StealthPowerEfficientFor(false, 640, 481));
  EXPECT_FALSE(StealthPowerEfficientFor(false, 1, 307201));
  EXPECT_FALSE(StealthPowerEfficientFor(false, 307201, 1));
  EXPECT_TRUE(StealthPowerEfficientFor(false, 1, 307200));
  EXPECT_TRUE(StealthPowerEfficientFor(false, 307200, 1));
  // A zero side is zero pixels, as in retail: low resolution.
  EXPECT_TRUE(StealthPowerEfficientFor(false, 0, 0));
  EXPECT_TRUE(StealthPowerEfficientFor(false, 0, 1080));
  EXPECT_TRUE(StealthPowerEfficientFor(false, 1920, 0));
  EXPECT_TRUE(StealthPowerEfficientFor(false, 720, 360)) << "259200 pixels";
  EXPECT_FALSE(StealthPowerEfficientFor(false, 65536, 65536)) << "overflow";
  EXPECT_FALSE(StealthPowerEfficientFor(false, UINT32_MAX, UINT32_MAX));
  EXPECT_FALSE(StealthPowerEfficientFor(false, UINT32_MAX, 1));
  EXPECT_TRUE(StealthPowerEfficientFor(true, 65536, 65536));
}

// Through the declaration the page reads: every codec of the table at every
// size of the grid answers supported, smooth, and the retail powerEfficient.
// Before the size entered the answer, a vp8 file in webm read the same
// powerEfficient at 320x240 and at 1080p.
TEST(StealthMediaDeclaration, DecodingInfoPowerEfficientFollowsTheRetailRule)
{
  DeclarationPref table(kDecodingInfoPref, kCoreTable);
  DecodeSupportPref tracks(kTrackTypes);
  ScopedBoolPref hevc("media.hevc.enabled", true);
  size_t checked = 0;
  size_t refusedRows = 0;
  for (const DecodingRow& row : kRows) {
    size_t refusedSizes = 0;
    for (const DecodingSize& size : kSizes) {
      const dom::MediaDecodingConfiguration config =
          FileConfig(row.mContentType, size);
      const auto info = dom::StealthDeclaredDecodingInfoFor(config);
      if (!info && row.mGeckoMayRefuse) {
        ++refusedSizes;
        continue;
      }
      ASSERT_TRUE(info) << row.mContentType << " " << size.mWidth << "x"
                        << size.mHeight << " is declared";
      EXPECT_TRUE(info->mSupported) << row.mContentType;
      EXPECT_TRUE(info->mSmooth) << row.mContentType;
      EXPECT_EQ(info->mPowerEfficient, row.mHardware || IsAtMost640x480(size))
          << row.mContentType << " at " << size.mWidth << "x" << size.mHeight;
      ++checked;
    }
    // Gecko's gate looks at the type, never at the size: a row is refused at
    // every size or at none.
    EXPECT_TRUE(refusedSizes == 0 || refusedSizes == std::size(kSizes))
        << row.mContentType << " refused at " << refusedSizes << " sizes";
    if (refusedSizes == std::size(kSizes)) {
      ++refusedRows;
    }
  }
  // Every row Gecko did not refuse is checked at every size, and the log says
  // how many rows (only hev1 and hvc1 can be) were left out.
  GTEST_LOG_(INFO) << "powerEfficient grid: " << checked << " answers checked, "
                   << refusedRows << " HEVC row(s) refused by Gecko's own gate";
  EXPECT_LE(refusedRows, size_t(2));
  EXPECT_EQ(checked, (std::size(kRows) - refusedRows) * std::size(kSizes));
}

// A type declared unsupported is false, false, false at every size: the size
// rule is retail's CreateVideoDecodingInfo, which a rejected file never reaches.
TEST(StealthMediaDeclaration, AnUnsupportedTypeIsNotPowerEfficientAtAnySize)
{
  DeclarationPref table(kDecodingInfoPref, "video/webm|vp8|000");
  DecodeSupportPref tracks(kTrackTypes);
  for (const DecodingSize& size : kSizes) {
    const auto info = dom::StealthDeclaredDecodingInfoFor(
        FileConfig("video/webm; codecs=\"vp8\"", size));
    ASSERT_TRUE(info);
    EXPECT_FALSE(info->mSupported);
    EXPECT_FALSE(info->mSmooth);
    EXPECT_FALSE(info->mPowerEfficient)
        << size.mWidth << "x" << size.mHeight;
  }
}

// --- The WebRTC half -------------------------------------------------------

// The helper the IsSWEncodeSmooth hook binds: the declared count, never the
// host's, and the real count only when nothing is declared. One value is offset
// from the host's count so a host answer can never pass for the declared one.
// That the hook actually routes encodingInfo through it is
// WebrtcAv1EncodeSmoothFollowsTheDeclaredCores, below.
TEST(StealthMediaDeclaration, DeclaredNumberOfProcessors)
{
  const size_t host = GetNumberOfProcessors();
  {
    ScopedIntPref cores(kCoresPref, 8);
    EXPECT_EQ(StealthDeclaredNumberOfProcessors(), size_t(8));
  }
  {
    ScopedIntPref cores(kCoresPref, int32_t(host) + 8);
    EXPECT_EQ(StealthDeclaredNumberOfProcessors(), host + 8);
  }
  {
    ScopedIntPref cores(kCoresPref, 2);
    EXPECT_EQ(StealthDeclaredNumberOfProcessors(), size_t(2));
  }
  // Undeclared (stock Firefox): the real count.
  {
    ScopedIntPref cores(kCoresPref, -1);
    EXPECT_EQ(StealthDeclaredNumberOfProcessors(), host);
  }
}

#ifdef MOZ_WEBRTC

namespace {

// VP8, VP9 and AV1 only: libwebrtc encodes them itself, so without the
// declaration the answer is a constant {SoftwareEncode} on every host.
EncoderConfig StealthEncodeConfig(CodecType aCodec) {
  EncoderConfig::CodecSpecific specific(void_t{});
  return EncoderConfig(
      aCodec, gfx::IntSize(1280, 720), Usage::Realtime,
      EncoderConfig::SampleFormat(dom::ImageBitmapFormat::YUV420P),
      /* fps */ 30u, /* kf interval */ 0, /* bitrate */ 0, /* br min */ 0,
      /* br max */ 0, BitrateMode::Variable, HardwarePreference::None,
      ScalabilityMode::None, specific);
}

media::EncodeSupportSet StealthEncodeSupport(CodecType aCodec) {
  return WaitFor(WebrtcVideoEncoderFactory::SupportsCodec(
                     StealthEncodeConfig(aCodec)))
      .unwrap();
}

// libwebrtc's own encoder first, as on desktop. Android defaults to the
// hardware strategy, which would mix the platform's answer into the
// fall-through cases below.
class EncoderStrategyPref {
 public:
  EncoderStrategyPref() {
    Preferences::SetInt("media.webrtc.encoder_creation_strategy", 0);
  }
  ~EncoderStrategyPref() {
    Preferences::ClearUser("media.webrtc.encoder_creation_strategy");
  }
};

}  // namespace

// Without the declaration VP8 is a constant {SoftwareEncode}: only the
// declaration can turn it into {HardwareEncode}.
TEST(StealthMediaDeclaration, EncodeSupportAnswersFromTheDeclaration)
{
  EncoderStrategyPref strategy;
  DeclarationPref pref(kEncodeDeclarationPref, "video/vp8|hw");
  EXPECT_EQ(StealthEncodeSupport(CodecType::VP8),
            media::EncodeSupportSet{media::EncodeSupport::HardwareEncode});
}

TEST(StealthMediaDeclaration, EncodeSupportDeclaredNoIsEmpty)
{
  EncoderStrategyPref strategy;
  DeclarationPref pref(kEncodeDeclarationPref, "video/vp8|no\nvideo/vp9|swhw");
  EXPECT_EQ(StealthEncodeSupport(CodecType::VP8), media::EncodeSupportSet{});
  EXPECT_EQ(StealthEncodeSupport(CodecType::VP9),
            media::EncodeSupportSet(media::EncodeSupport::SoftwareEncode,
                                    media::EncodeSupport::HardwareEncode));
}

// An undeclared codec and an unrecognised token both keep Mozilla's answer
// (VP8 and VP9: {SoftwareEncode}); the declared AV1 row next to them still
// applies.
TEST(StealthMediaDeclaration, EncodeSupportUndeclaredAndMalformedFallThrough)
{
  EncoderStrategyPref strategy;
  DeclarationPref pref(kEncodeDeclarationPref,
                       "video/vp8|fast\nvideo/vp9\nvideo/av1|hw");
  EXPECT_EQ(StealthEncodeSupport(CodecType::VP8),
            media::EncodeSupportSet{media::EncodeSupport::SoftwareEncode});
  EXPECT_EQ(StealthEncodeSupport(CodecType::VP9),
            media::EncodeSupportSet{media::EncodeSupport::SoftwareEncode});
  EXPECT_EQ(StealthEncodeSupport(CodecType::AV1),
            media::EncodeSupportSet{media::EncodeSupport::HardwareEncode});
}

// The H.264 gate (DefaultCodecPreferences, which WebrtcCodecInfo and so
// decodingInfo/encodingInfo ask first) follows the declaration, not the
// OpenH264 plugin and not the GPU blocklist. The hardware path is switched off
// so only the software gate can open it. The gtest profile HAS the fake
// plugin, so the declared "no" case is the one that proves the plugin is no
// longer asked: before the hook it answered true.
TEST(StealthMediaDeclaration, DeclaredH264GateIgnoresThePlugin)
{
  if (!gfx::gfxVars::IsInitialized()) {
    gfx::gfxVars::Initialize();
  }
  Preferences::SetBool("media.webrtc.hw.h264.enabled", false);
  const Maybe<MediaExtendedMIMEType> h264 =
      MakeMediaExtendedMIMEType("video/h264");
  ASSERT_TRUE(h264);
  {
    DeclarationPref decode(kDeclarationPref, "video/avc|sw");
    DeclarationPref encode(kEncodeDeclarationPref, "video/avc|sw");
    EXPECT_EQ(StealthDeclaredSoftwareH264(), Some(true));
    DefaultCodecPreferences prefs;
    EXPECT_TRUE(prefs.SoftwareH264Enabled());
    EXPECT_FALSE(prefs.HardwareH264Enabled());
    EXPECT_TRUE(prefs.H264Enabled());
    EXPECT_TRUE(prefs.SendingH264PacketizationModeZeroSupported());
    const auto info = WebrtcCodecInfo::Create();
    EXPECT_TRUE(info->CheckDecodeType(*h264));
    EXPECT_TRUE(info->CheckEncodeType(*h264));
  }
  {
    DeclarationPref decode(kDeclarationPref, "video/avc|no");
    DeclarationPref encode(kEncodeDeclarationPref, "video/avc|no");
    EXPECT_EQ(StealthDeclaredSoftwareH264(), Some(false));
    DefaultCodecPreferences prefs;
    EXPECT_FALSE(prefs.SoftwareH264Enabled());
    EXPECT_FALSE(prefs.H264Enabled());
    EXPECT_FALSE(prefs.SendingH264PacketizationModeZeroSupported());
    const auto info = WebrtcCodecInfo::Create();
    EXPECT_FALSE(info->CheckDecodeType(*h264));
    EXPECT_FALSE(info->CheckEncodeType(*h264));
  }
  // Declared in one table only, or with an unrecognised token: not declared,
  // and Mozilla's plugin check decides.
  {
    DeclarationPref decode(kDeclarationPref, "video/avc|sw");
    DeclarationPref encode(kEncodeDeclarationPref, "video/vp8|sw");
    EXPECT_EQ(StealthDeclaredSoftwareH264(), Nothing());
  }
  {
    DeclarationPref decode(kDeclarationPref, "video/avc|sw");
    DeclarationPref encode(kEncodeDeclarationPref, "video/avc|yes");
    EXPECT_EQ(StealthDeclaredSoftwareH264(), Nothing());
  }
  Preferences::ClearUser("media.webrtc.hw.h264.enabled");
}

#  ifndef MOZ_WIDGET_ANDROID

namespace {

struct EncodingAnswer {
  bool mSupported = false;
  bool mSmooth = false;
};

// navigator.mediaCapabilities.encodingInfo({type: "webrtc", video}) through
// MediaCapabilities::EncodingInfo itself, on a sandbox global: the same road a
// page takes, down to IsSWEncodeSmooth, which is file-static and has no other
// way in.
EncodingAnswer WebrtcEncodingInfo(const nsAString& aContentType,
                                  uint32_t aWidth, uint32_t aHeight,
                                  double aFramerate) {
  EncodingAnswer answer;
  nsCOMPtr<nsIGlobalObject> global =
      xpc::NativeGlobal(xpc::PrivilegedJunkScope());
  RefPtr<dom::MediaCapabilities> caps = new dom::MediaCapabilities(global);

  dom::MediaEncodingConfiguration config;
  config.mType = dom::MediaEncodingType::Webrtc;
  dom::VideoConfiguration& video = config.mVideo.Construct();
  video.mContentType = aContentType;
  video.mWidth = aWidth;
  video.mHeight = aHeight;
  video.mBitrate = 2'000'000;
  video.mFramerate = aFramerate;

  IgnoredErrorResult rv;
  RefPtr<dom::Promise> promise = caps->EncodingInfo(config, rv);
  EXPECT_FALSE(rv.Failed());
  if (rv.Failed() || !promise) {
    return answer;
  }
  MOZ_ALWAYS_TRUE(SpinEventLoopUntil(
      "TestStealthMediaDeclaration::WebrtcEncodingInfo"_ns, [&] {
        return promise->State() != dom::Promise::PromiseState::Pending;
      }));
  EXPECT_EQ(promise->State(), dom::Promise::PromiseState::Resolved);
  if (promise->State() != dom::Promise::PromiseState::Resolved) {
    return answer;
  }

  dom::AutoJSAPI jsapi;
  MOZ_ALWAYS_TRUE(jsapi.Init(xpc::PrivilegedJunkScope()));
  JSContext* cx = jsapi.cx();
  JS::Rooted<JSObject*> promiseObj(cx, promise->PromiseObj());
  JS::Rooted<JS::Value> result(cx, JS::GetPromiseResult(promiseObj));
  EXPECT_TRUE(result.isObject());
  if (!result.isObject()) {
    return answer;
  }
  JS::Rooted<JSObject*> info(cx, &result.toObject());
  JS::Rooted<JS::Value> field(cx);
  MOZ_ALWAYS_TRUE(JS_GetProperty(cx, info, "supported", &field));
  answer.mSupported = field.isTrue();
  MOZ_ALWAYS_TRUE(JS_GetProperty(cx, info, "smooth", &field));
  answer.mSmooth = field.isTrue();
  return answer;
}

}  // namespace

// encodingInfo's "smooth" for a software encoder sizes libwebrtc's encoder
// threads from the core count (IsSWEncodeSmooth). AV1 at 1280x720 and 30 fps
// flips there: more than 4 cores give 4 threads, 0.98 x (60/30) x (4/4) = 1.96,
// smooth; 2 cores give 1 thread, 0.49, not smooth. The page must see the
// DECLARED count's answer on every host. Without the hook in IsSWEncodeSmooth
// one of the two cases fails on any machine: a host with more than 4 cores
// answers smooth for the declared 2, one with 4 or fewer answers not smooth
// for the declared 8.
TEST(StealthMediaDeclaration, WebrtcAv1EncodeSmoothFollowsTheDeclaredCores)
{
  if (!gfx::gfxVars::IsInitialized()) {
    gfx::gfxVars::Initialize();
  }
  EncoderStrategyPref strategy;
  // Software only, so "smooth" is the estimate and never the hardware shortcut.
  DeclarationPref encode(kEncodeDeclarationPref, "video/av1|sw");
  {
    ScopedIntPref cores(kCoresPref, 8);
    const EncodingAnswer answer =
        WebrtcEncodingInfo(u"video/AV1"_ns, 1280, 720, 30.0);
    EXPECT_TRUE(answer.mSupported);
    EXPECT_TRUE(answer.mSmooth);
  }
  {
    ScopedIntPref cores(kCoresPref, 2);
    const EncodingAnswer answer =
        WebrtcEncodingInfo(u"video/AV1"_ns, 1280, 720, 30.0);
    EXPECT_TRUE(answer.mSupported);
    EXPECT_FALSE(answer.mSmooth);
  }
}

#  endif  // !MOZ_WIDGET_ANDROID

#endif  // MOZ_WEBRTC
