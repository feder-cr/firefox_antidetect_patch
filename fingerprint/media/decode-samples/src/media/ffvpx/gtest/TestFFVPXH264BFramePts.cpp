/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// ff_h264_decoder (h264dec.c) does not set FF_CODEC_CAP_SETS_FRAME_PROPS: for
// such decoders the generic ffmpeg layer (media/ffvpx/libavcodec/decode.c)
// stamps every frame coming out of avcodec_receive_frame() with the
// pts/pkt_dts of the MOST RECENTLY SENT packet, not the packet that actually
// produced that image (REA on media/ffvpx/libavcodec/decode.c, confirmed with
// a real build via MOZ_LOG="FFmpegVideo:5" on this clip, and confirmed
// identical on Firefox 159.0 retail Linux with the distro's own
// libavcodec.so.59: this is a Mozilla-wrapper-level gap, not specific to our
// ffvpx). With B-frames the image can come out several packets after it was
// sent, so VideoFrame.timestamp (readable by any page using WebCodecs) comes
// out reordered relative to retail, even though the decoded image content
// itself is already in the right order.
//
// h264_8bit_1920x1080.h264: 96-frame Annex-B elementary stream, has_b_frames
// 2, vendored from invisible_WebCodecs
// (captures/h264-1080p-bframes/h264_8bit_1920x1080.h264, sha256
// 04da8f4dcbb8bc0f72749c0170d0684591aa10b9f8d04486a199dff84e0d51be). We feed
// it exactly like the browser's WebCodecs probe does: one EncodedVideoChunk
// per access unit in bitstream order, timestamp = index * 41667, duration =
// 41667. The expected output order is retail's own measured sequence
// (captures/h264-1080p-bframes/retail-win/result.json, Firefox 159.0 retail
// Windows): 96 frames, every duration 41667, timestamps strictly the dense
// sequence 0, 41667, 83334, ..., 95*41667 -- i.e. output index k must carry
// timestamp k*41667.

#include <cstdint>
#include <cstdio>
#include <utility>

#include "AnnexB.h"
#include "H264.h"
#include "ImageContainer.h"
#include "PDMFactory.h"
#include "VideoUtils.h"
#include "gtest/gtest.h"
#include "mozilla/Maybe.h"
#include "mozilla/ScopeExit.h"
#include "mozilla/Span.h"
#include "mozilla/gtest/ScopedPrefSetter.h"
#include "mozilla/gtest/WaitFor.h"

using namespace mozilla;

namespace {

constexpr int64_t kFrameUs = 41667;
constexpr size_t kExpectedFrames = 96;
constexpr const char* kClipFile = "h264_8bit_1920x1080.h264";

bool ReadWholeFile(const char* aFileName, nsTArray<uint8_t>& aOut) {
  FILE* f = fopen(aFileName, "rb");
  if (!f) {
    return false;
  }
  uint8_t buf[65536];
  size_t n;
  while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
    aOut.AppendElements(buf, n);
  }
  fclose(f);
  return !aOut.IsEmpty();
}

// Groups the NAL units of an Annex-B elementary stream into access units:
// a new AU starts at an AUD, or at a VCL NAL with first_mb_in_slice == 0
// when the current AU already holds a VCL NAL. Non-VCL NALs (SEI, SPS,
// PPS) that directly precede that VCL go to the new AU. Same rule as the
// browser-side probe (invisible_WebCodecs/scripts/probe.html
// splitAccessUnits), ported from NAL byte-offsets to NAL indices.
void SplitAccessUnits(const nsTArray<uint8_t>& aData,
                      const nsTArray<AnnexB::NALEntry>& aNals,
                      nsTArray<std::pair<size_t, size_t>>& aAUs /* [start,end) NAL index ranges */) {
  nsTArray<size_t> boundaries;
  bool curHasVcl = false;
  bool prevVcl = false;
  Maybe<size_t> runStart;
  for (size_t i = 0; i < aNals.Length(); i++) {
    const size_t offset = size_t(aNals[i].mOffset);
    const uint8_t nalType = aData[offset] & 0x1f;
    const bool isAud = nalType == H264_NAL_AUD;
    const bool isVcl = nalType >= H264_NAL_SLICE && nalType <= H264_NAL_IDR_SLICE;
    if (isAud) {
      if (curHasVcl) {
        boundaries.AppendElement(i);
      }
      curHasVcl = false;
      prevVcl = false;
      runStart = Nothing();
    } else if (isVcl) {
      const bool firstSlice =
          aNals[i].mSize >= 2 && (aData[offset + 1] & 0x80) != 0;
      if (firstSlice && curHasVcl) {
        boundaries.AppendElement(runStart ? *runStart : i);
      }
      curHasVcl = true;
      prevVcl = true;
      runStart = Nothing();
    } else if (prevVcl && runStart.isNothing()) {
      runStart = Some(i);
    }
  }
  size_t start = 0;
  for (size_t b : boundaries) {
    aAUs.AppendElement(std::make_pair(start, b));
    start = b;
  }
  if (start < aNals.Length()) {
    aAUs.AppendElement(std::make_pair(start, aNals.Length()));
  }
}

bool AuIsKey(const nsTArray<uint8_t>& aData,
            const nsTArray<AnnexB::NALEntry>& aNals, size_t aStart,
            size_t aEnd) {
  for (size_t i = aStart; i < aEnd; i++) {
    if ((aData[size_t(aNals[i].mOffset)] & 0x1f) == H264_NAL_IDR_SLICE) {
      return true;
    }
  }
  return false;
}

// Builds a clean Annex-B buffer (our own 4-byte start codes) for the NALs
// in [aStart, aEnd).
void BuildAnnexB(const nsTArray<uint8_t>& aData,
                 const nsTArray<AnnexB::NALEntry>& aNals, size_t aStart,
                 size_t aEnd, nsTArray<uint8_t>& aOut) {
  static const uint8_t kStartCode[] = {0, 0, 0, 1};
  for (size_t i = aStart; i < aEnd; i++) {
    aOut.AppendElements(kStartCode, sizeof(kStartCode));
    aOut.AppendElements(aData.Elements() + size_t(aNals[i].mOffset),
                        size_t(aNals[i].mSize));
  }
}

// Decodes every access unit of kClipFile with the bundled ffvpx H.264
// decoder (software path only, same as CreateImage()), feeding it exactly
// like the browser's WebCodecs probe does, and returns the (mTime, mDuration)
// of every VideoData produced, in output order.
bool DecodeAllFrames(nsTArray<std::pair<int64_t, int64_t>>& aOut) {
  nsTArray<uint8_t> file;
  if (!ReadWholeFile(kClipFile, file)) {
    ADD_FAILURE() << "Failed to read " << kClipFile;
    return false;
  }

  nsTArray<AnnexB::NALEntry> nals;
  AnnexB::ParseNALEntries(Span(file.Elements(), file.Length()), nals);
  if (nals.IsEmpty()) {
    ADD_FAILURE() << "No NAL units found in " << kClipFile;
    return false;
  }

  RefPtr<MediaByteBuffer> avcc =
      AnnexB::ExtractExtraDataForAVCC(Span(file.Elements(), file.Length()));
  if (!avcc || avcc->IsEmpty()) {
    ADD_FAILURE() << "Failed to extract SPS/PPS from " << kClipFile;
    return false;
  }

  nsTArray<std::pair<size_t, size_t>> aus;
  SplitAccessUnits(file, nals, aus);
  if (aus.Length() != kExpectedFrames) {
    ADD_FAILURE() << kClipFile << ": expected " << kExpectedFrames
                  << " access units, got " << aus.Length();
    return false;
  }

  VideoInfo info(1920, 1080);
  info.mMimeType = "video/avc"_ns;
  info.mExtraData = avcc;

  ScopedPrefSetter decoderPrefs({
      {"media.gpu-process-decoder", false},
      {"media.rdd-process.enabled", false},
      {"media.utility-process.enabled", false},
      {"media.prefer-non-ffvpx", false},
  });
  RefPtr<PDMFactory> factory = MakeRefPtr<PDMFactory>();
  RefPtr<layers::ImageContainer> imageContainer =
      MakeRefPtr<layers::ImageContainer>(
          layers::ImageUsageType::VideoFrameContainer,
          layers::ImageContainer::ASYNCHRONOUS);
  // No wrapper: the raw ffvpx decoder expects AVCC samples (NeedsConversion()
  // == kNeedAVCC), which is what we hand it below.
  auto created = WaitFor(factory->CreateDecoder(CreateDecoderParams{
      info, imageContainer.get(),
      CreateDecoderParams::WrapperSet({/* no wrapper */})}));
  if (created.isErr()) {
    ADD_FAILURE() << "Failed to create a H.264 decoder for " << kClipFile;
    return false;
  }
  RefPtr<MediaDataDecoder> decoder = created.unwrap();
  auto shutdownDecoder =
      MakeScopeExit([&] { WaitForResolve(decoder->Shutdown()); });
  if (!decoder->GetDescriptionName().EqualsLiteral("ffvpx video decoder")) {
    ADD_FAILURE() << kClipFile << " went to "
                  << decoder->GetDescriptionName().get()
                  << ", not to the bundled FFmpeg (software path)";
    return false;
  }
  if (WaitFor(decoder->Init()).isErr()) {
    ADD_FAILURE() << "Failed to initialize the ffvpx H.264 decoder";
    return false;
  }

  auto append = [&](const MediaDataDecoder::DecodedData& aData) {
    for (const auto& d : aData) {
      const VideoData* v = d->As<VideoData>();
      aOut.AppendElement(std::make_pair(v->mTime.ToMicroseconds(),
                                        v->mDuration.ToMicroseconds()));
    }
  };

  for (size_t i = 0; i < aus.Length(); i++) {
    nsTArray<uint8_t> annexB;
    BuildAnnexB(file, nals, aus[i].first, aus[i].second, annexB);
    RefPtr<MediaRawData> sample =
        new MediaRawData(annexB.Elements(), annexB.Length());
    if (!AnnexB::ConvertSampleToAVCC(sample, avcc)) {
      ADD_FAILURE() << "Failed to convert AU " << i << " to AVCC";
      return false;
    }
    sample->mTime = media::TimeUnit::FromMicroseconds(int64_t(i) * kFrameUs);
    sample->mDuration = media::TimeUnit::FromMicroseconds(kFrameUs);
    sample->mTimecode = sample->mTime;
    sample->mKeyframe = AuIsKey(file, nals, aus[i].first, aus[i].second);

    auto decoded = WaitFor(decoder->Decode(sample));
    if (decoded.isErr()) {
      ADD_FAILURE() << "H.264 decode error on AU " << i;
      return false;
    }
    append(decoded.inspect());
  }
  for (;;) {
    auto drained = WaitFor(decoder->Drain());
    if (drained.isErr() || drained.inspect().IsEmpty()) {
      break;
    }
    append(drained.inspect());
  }
  return true;
}

}  // namespace

// Without the reorder buffer in FFmpegVideoDecoder::DoDecode(), this fails:
// the output sequence is retail's own measured broken order (0, 83334,
// 125001, 41667, 208335, ...), not the dense retail-win sequence. With the
// fix, output index k must carry timestamp k*41667, duration 41667, matching
// captures/h264-1080p-bframes/retail-win/result.json frame-by-frame.
TEST(FFVPXH264, BFramePtsMatchesRetailOrder)
{
  nsTArray<std::pair<int64_t, int64_t>> frames;
  ASSERT_TRUE(DecodeAllFrames(frames));
  ASSERT_EQ(frames.Length(), kExpectedFrames);
  for (size_t k = 0; k < frames.Length(); k++) {
    EXPECT_EQ(frames[k].first, int64_t(k) * kFrameUs)
        << "output frame " << k << ": timestamp";
    EXPECT_EQ(frames[k].second, kFrameUs) << "output frame " << k << ": duration";
  }
}
