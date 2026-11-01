/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "StealthMediaDeclaration.h"

#include "DecoderTraits.h"
#include "MediaContainerType.h"
#include "MediaMIMETypes.h"
#include "PlatformEncoderModule.h"
#include "mozilla/Logging.h"
#include "mozilla/StaticPrefs_zoom.h"
#include "mozilla/dom/MediaCapabilitiesBinding.h"
#include "mozilla/dom/MediaKeySystemAccess.h"
#include "nsCharSeparatedTokenizer.h"
#include "nsString.h"
#include "nsThreadUtils.h"

#include <type_traits>

namespace mozilla {

// Both tables are NEWLINE separated, and that is not a style choice. The
// separator used to be a comma, which is also what a multi-codec type carries
// inside its own value, so `codecs="avc1.42E01E, mp4a.40.2"` was split into two
// fragments and matched nothing. The format could not express the commonest
// shape in the wild. Found 2026-08-08 by measuring a 62-type corpus, where it
// was the last divergence left after the table was filled - and it survived
// being ADDED to the table, which is what pointed at the parser rather than the
// data. Nothing in these fields can contain a newline.

namespace {

// One parser for both tables, decode_support and encode_support: the same
// "trackmime|no|sw|hw|swhw" lines, so a token means the same thing in both.
bool ParseDeclaredSupport(const nsACString& aSpec,
                          const nsACString& aTrackMimeType, bool& aSoftware,
                          bool& aHardware) {
  if (aSpec.IsEmpty()) {
    return false;
  }
  for (const nsACString& entry : aSpec.Split('\n')) {
    const int32_t bar = nsCString(entry).FindChar('|');
    if (bar <= 0) {
      continue;
    }
    if (!aTrackMimeType.Equals(Substring(entry, 0, bar))) {
      continue;
    }
    const nsDependentCSubstring flags(Substring(entry, bar + 1));
    if (flags.EqualsLiteral("no")) {
      aSoftware = false;
      aHardware = false;
    } else if (flags.EqualsLiteral("sw")) {
      aSoftware = true;
      aHardware = false;
    } else if (flags.EqualsLiteral("hw")) {
      aSoftware = false;
      aHardware = true;
    } else if (flags.EqualsLiteral("swhw")) {
      aSoftware = true;
      aHardware = true;
    } else {
      // An UNRECOGNISED token falls through to the real logic; it does not mean
      // "no". A typo in the pref must never be able to tell every page that a
      // codec this build decodes perfectly well is unsupported: that inverts
      // the rule this table is written under, where the failure mode is "we
      // fail to disguise" and never "the page breaks".
      return false;
    }
    return true;
  }
  return false;
}

}  // namespace

bool StealthDeclaredDecodeSupport(const nsACString& aTrackMimeType,
                                  bool& aSoftware, bool& aHardware) {
  nsAutoCString spec;
  {
    auto lock = StaticPrefs::zoom_stealth_media_decode_support();
    spec = *lock;
  }
  return ParseDeclaredSupport(spec, aTrackMimeType, aSoftware, aHardware);
}

bool StealthDeclaredEncodeSupport(const nsACString& aTrackMimeType,
                                  bool& aSoftware, bool& aHardware) {
  nsAutoCString spec;
  {
    auto lock = StaticPrefs::zoom_stealth_media_encode_support();
    spec = *lock;
  }
  return ParseDeclaredSupport(spec, aTrackMimeType, aSoftware, aHardware);
}

Maybe<media::EncodeSupportSet> StealthDeclaredEncodeSupportSet(
    const nsACString& aTrackMimeType) {
  bool software = false;
  bool hardware = false;
  if (!StealthDeclaredEncodeSupport(aTrackMimeType, software, hardware)) {
    return Nothing();
  }
  media::EncodeSupportSet declared{};
  if (software) {
    declared += media::EncodeSupport::SoftwareEncode;
  }
  if (hardware) {
    declared += media::EncodeSupport::HardwareEncode;
  }
  return Some(declared);
}

static_assert(std::is_same_v<StealthEncodeSupportPromise,
                             PlatformEncoderModule::SupportsEncoderPromise>);

// The track mime types decode_support is keyed by, so one name means one codec
// in both tables.
static const char* StealthTrackMimeOf(CodecType aCodec) {
  switch (aCodec) {
    case CodecType::H264:
      return "video/avc";
    case CodecType::H265:
      return "video/hevc";
    case CodecType::VP8:
      return "video/vp8";
    case CodecType::VP9:
      return "video/vp9";
    case CodecType::AV1:
      return "video/av1";
    default:
      return nullptr;
  }
}

RefPtr<StealthEncodeSupportPromise> StealthDeclaredWebrtcEncodeSupport(
    const EncoderConfig& aConfig) {
  const char* mime = StealthTrackMimeOf(aConfig.mCodec);
  if (!mime) {
    return nullptr;
  }
  Maybe<media::EncodeSupportSet> declared =
      StealthDeclaredEncodeSupportSet(nsDependentCString(mime));
  if (!declared) {
    return nullptr;
  }
  return StealthEncodeSupportPromise::CreateAndResolve(*declared, __func__);
}

Maybe<bool> StealthDeclaredSoftwareH264() {
  bool decodeSw = false, decodeHw = false, encodeSw = false, encodeHw = false;
  if (!StealthDeclaredDecodeSupport("video/avc"_ns, decodeSw, decodeHw) ||
      !StealthDeclaredEncodeSupport("video/avc"_ns, encodeSw, encodeHw)) {
    return Nothing();
  }
  return Some(decodeSw && encodeSw);
}

size_t StealthDeclaredNumberOfProcessors() {
  const int32_t declared = StaticPrefs::zoom_stealth_hw_concurrency();
  return declared > 0 ? size_t(declared) : GetNumberOfProcessors();
}

Maybe<media::DecodeSupportSet> StealthDeclaredDecodeSupportSet(
    const nsACString& aTrackMimeType) {
  bool software = false;
  bool hardware = false;
  if (!StealthDeclaredDecodeSupport(aTrackMimeType, software, hardware)) {
    return Nothing();
  }
  media::DecodeSupportSet declared{};
  if (software) {
    declared += media::DecodeSupport::SoftwareDecode;
  }
  if (hardware) {
    declared += media::DecodeSupport::HardwareDecode;
  }
  return Some(declared);
}

bool StealthDeclaredDecodingInfo(const nsACString& aContainerMimeType,
                                 const nsACString& aCodec, bool& aSupported,
                                 bool& aSmooth, bool& aPowerEfficient) {
  nsAutoCString spec;
  {
    auto lock = StaticPrefs::zoom_stealth_media_decoding_info();
    spec = *lock;
  }
  if (spec.IsEmpty()) {
    return false;
  }
  // The family prefix, i.e. everything before the first '.': "avc1.640028" ->
  // "avc1", "vp8" -> "vp8". Splitting on the dot rather than declaring whole
  // codec strings is what keeps the key space finite - there are eight families
  // and an unbounded number of strings built from them.
  const int32_t dot = nsCString(aCodec).FindChar('.');
  const nsDependentCSubstring family =
      dot > 0 ? Substring(aCodec, 0, uint32_t(dot)) : Substring(aCodec, 0);

  for (const nsACString& entry : spec.Split('\n')) {
    const int32_t bar1 = nsCString(entry).FindChar('|');
    if (bar1 <= 0) {
      continue;
    }
    if (!aContainerMimeType.Equals(Substring(entry, 0, bar1))) {
      continue;
    }
    const nsDependentCSubstring rest(Substring(entry, bar1 + 1));
    const int32_t bar2 = nsCString(rest).FindChar('|');
    if (bar2 <= 0) {
      continue;
    }
    if (!family.Equals(Substring(rest, 0, bar2))) {
      continue;
    }
    const nsDependentCSubstring bits(Substring(rest, bar2 + 1));
    if (bits.Length() != 3) {
      return false;  // malformed: fall through, never a partial answer
    }
    for (uint32_t i = 0; i < 3; ++i) {
      if (bits[i] != '0' && bits[i] != '1') {
        return false;
      }
    }
    aSupported = bits[0] == '1';
    aSmooth = bits[1] == '1';
    aPowerEfficient = bits[2] == '1';
    return true;
  }
  return false;
}

}  // namespace mozilla

namespace mozilla::dom {

// The declared decodingInfo answer is logged under MediaCapabilities' own
// module, as the hook in MediaCapabilities::CreateNonWebRTCDecodingInfo used to.
static LazyLogModule sStealthMediaCapabilitiesLog("MediaCapabilities");

Maybe<MediaCapabilitiesDecodingInfo> StealthDeclaredDecodingInfoFor(
    const MediaDecodingConfiguration& aConfiguration) {
  if (!aConfiguration.mVideo.WasPassed()) {
    return Nothing();
  }
  Maybe<MediaContainerType> container =
      MakeMediaContainerType(aConfiguration.mVideo.Value().mContentType);
  if (!container) {
    return Nothing();
  }
  // Gecko's own gate first, and it is a precondition rather than a fallback:
  // this is the compiled, host-independent part - IsAllowedH264Codec's profile
  // set and its [1, 6.2] level window, the container/codec pairing rules - and
  // it answers the same on every platform.
  //
  // Without it the family key would be too coarse in exactly one place and
  // wrongly: avc1.424028 and avc1.6E4028 share the family "avc1", retail
  // answers supported/smooth/powerEfficient for the first and nothing for the
  // second, and a declaration keyed on the family alone would have claimed
  // high10 support that no Firefox has. Splitting the key finer would mean
  // declaring the profile space we just established is not ours to declare.
  if (DecoderTraits::CanHandleContainerType(*container, nullptr) == CANPLAY_NO) {
    return Nothing();
  }
  const MediaCodecs& codecs = container->ExtendedType().Codecs();
  if (codecs.IsEmpty()) {
    return Nothing();
  }
  bool supported = true, smooth = true, powerEfficient = true;
  bool sawOne = false;
  for (const auto& codec : codecs.Range()) {
    bool s = false, m = false, p = false;
    if (!StealthDeclaredDecodingInfo(container->Type().AsString(),
                                     NS_ConvertUTF16toUTF8(codec), s, m, p)) {
      return Nothing();
    }
    sawOne = true;
    supported &= s;
    smooth &= m;
    powerEfficient &= p;
  }
  if (!sawOne) {
    return Nothing();
  }
  MOZ_LOG(sStealthMediaCapabilitiesLog, LogLevel::Debug,
          ("[MediaCapabilities] %s -> DECLARED supported=%d smooth=%d "
           "powerEfficient=%d",
           NS_ConvertUTF16toUTF8(aConfiguration.mVideo.Value().mContentType)
               .get(),
           supported, smooth, powerEfficient));
  MediaCapabilitiesDecodingInfo info;
  info.mSupported = supported;
  info.mSmooth = smooth;
  info.mPowerEfficient = powerEfficient;
  return Some(std::move(info));
}

}  // namespace mozilla::dom
