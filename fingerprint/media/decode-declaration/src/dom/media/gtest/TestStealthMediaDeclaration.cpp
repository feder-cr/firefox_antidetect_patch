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
#include "mozilla/gtest/WaitFor.h"
#include "nsThreadUtils.h"

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
