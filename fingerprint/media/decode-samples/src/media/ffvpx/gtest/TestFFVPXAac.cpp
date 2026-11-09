/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// AAC-LC in ffvpx gives the samples of Windows (Media Foundation) on every
// CPU. The m4a decodeAudioData would be handed goes through the same MP4
// demuxer and ffvpx decoder, once with FFmpeg forced to plain C and once with
// every SIMD extension the host has (AVX included):
// - the SHA-256 of the two outputs must be the same;
// - the samples must be those Media Foundation gives on Windows 11
//   (msauddecmft.dll 10.0.26100.9549, the *.mf.wav files), max |diff| 0.
//   Media Foundation keeps the encoder delay that the MP4 edit list removes,
//   so the comparison starts 1024 frames into the WAV.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "FFVPXRuntimeLinker.h"
#include "MP4Demuxer.h"
#include "MockMediaResource.h"
#include "PDMFactory.h"
#include "VideoUtils.h"
#include "gtest/gtest.h"
#include "mozilla/Preferences.h"
#include "mozilla/ScopeExit.h"
#include "mozilla/SharedThreadPool.h"
#include "mozilla/gtest/ScopedPrefSetter.h"
#include "mozilla/gtest/WaitFor.h"
#include "nsComponentManagerUtils.h"
#include "nsICryptoHash.h"
#include "nsNetCID.h"
#include "prlink.h"

using namespace mozilla;

namespace {

// libavutil/cpu.h
constexpr int kAvCpuFlagAvx = 0x4000;

// Media Foundation does not drop the encoder delay.
constexpr size_t kMfPrimingFrames = 1024;

using ForceCpuFlags = void (*)(int);
using GetCpuFlags = int (*)();

// FFmpeg's CPU flags in the bundled libmozavutil.
struct AvCpu {
  ForceCpuFlags mForce = nullptr;
  GetCpuFlags mGet = nullptr;
  PRLibrary* mLib = nullptr;
  ~AvCpu() {
    if (mForce) {
      mForce(-1);  // Back to what the CPU has.
    }
    if (mLib) {
      PR_UnloadLibrary(mLib);
    }
  }
  bool Load() {
    if (!FFVPXRuntimeLinker::Init()) {
      return false;
    }
    mForce = reinterpret_cast<ForceCpuFlags>(
        PR_FindFunctionSymbolAndLibrary("av_force_cpu_flags", &mLib));
    if (mLib) {
      mGet = reinterpret_cast<GetCpuFlags>(
          PR_FindFunctionSymbol(mLib, "av_get_cpu_flags"));
    }
    return mForce && mGet;
  }
  int Host() {
    mForce(-1);
    return mGet();
  }
};

struct Decoded {
  nsTArray<AudioDataValue> mPcm;  // interleaved
  uint32_t mFrames = 0;
  uint32_t mChannels = 0;
};

nsCString HexSha256(const nsTArray<AudioDataValue>& aPcm) {
  nsCOMPtr<nsICryptoHash> hash = do_CreateInstance(NS_CRYPTO_HASH_CONTRACTID);
  if (!hash || NS_FAILED(hash->Init(nsICryptoHash::SHA256)) ||
      NS_FAILED(hash->Update(reinterpret_cast<const uint8_t*>(aPcm.Elements()),
                             aPcm.Length() * sizeof(AudioDataValue)))) {
    return "(hash failed)"_ns;
  }
  nsAutoCString raw;
  if (NS_FAILED(hash->Finish(false, raw))) {
    return "(hash failed)"_ns;
  }
  nsCString hex;
  for (uint32_t i = 0; i < raw.Length(); i++) {
    hex.AppendPrintf("%02x", uint8_t(raw[i]));
  }
  return hex;
}

// The float samples of an IEEE float WAV (WAVE_FORMAT_EXTENSIBLE or 3).
bool ReadFloatWav(const char* aFileName, uint32_t* aChannels,
                  nsTArray<float>& aSamples) {
  FILE* f = fopen(aFileName, "rb");
  if (!f) {
    return false;
  }
  nsTArray<uint8_t> b;
  uint8_t buf[65536];
  size_t n;
  while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
    b.AppendElements(buf, n);
  }
  fclose(f);
  auto u16 = [&](size_t o) { return uint32_t(b[o]) | uint32_t(b[o + 1]) << 8; };
  auto u32 = [&](size_t o) { return u16(o) | u16(o + 2) << 16; };
  if (b.Length() < 12 || memcmp(b.Elements(), "RIFF", 4) ||
      memcmp(b.Elements() + 8, "WAVE", 4)) {
    return false;
  }
  *aChannels = 0;
  for (size_t o = 12; o + 8 <= b.Length();) {
    const uint32_t size = u32(o + 4);
    if (o + 8 + size > b.Length()) {
      return false;
    }
    if (!memcmp(b.Elements() + o, "fmt ", 4) && size >= 16) {
      if (u16(o + 8 + 14) != 32) {  // bits per sample
        return false;
      }
      *aChannels = u16(o + 8 + 2);
    } else if (!memcmp(b.Elements() + o, "data", 4)) {
      aSamples.SetLength(size / sizeof(float));
      memcpy(aSamples.Elements(), b.Elements() + o + 8,
             aSamples.Length() * sizeof(float));
      return *aChannels != 0;
    }
    o += 8 + size + (size & 1);
  }
  return false;
}

// Demuxes aFileName and decodes all of its audio with the bundled FFmpeg.
bool DecodeWithFFVPX(const char* aFileName, Decoded& aOut) {
  RefPtr<MockMediaResource> resource = new MockMediaResource(aFileName);
  if (NS_FAILED(resource->Open())) {
    ADD_FAILURE() << "Failed to open " << aFileName;
    return false;
  }
  RefPtr<MP4Demuxer> demuxer = new MP4Demuxer(resource);
  RefPtr<TaskQueue> taskQueue = TaskQueue::Create(
      GetMediaThreadPool(MediaThreadType::SUPERVISOR), "TestFFVPXAac");
  auto shutdownTaskQueue = MakeScopeExit([&] {
    taskQueue->BeginShutdown();
    taskQueue->AwaitShutdownAndIdle();
  });

  WaitForResolve(
      InvokeAsync(taskQueue, __func__, [demuxer] { return demuxer->Init(); }));
  RefPtr<MediaTrackDemuxer> track =
      demuxer->GetTrackDemuxer(TrackInfo::kAudioTrack, 0);
  UniquePtr<TrackInfo> trackInfo = track ? track->GetInfo() : nullptr;
  if (!trackInfo || !trackInfo->GetAsAudioInfo()) {
    ADD_FAILURE() << "Missing audio track in " << aFileName;
    return false;
  }

  nsTArray<RefPtr<MediaRawData>> samples;
  for (;;) {
    auto got = WaitFor(InvokeAsync(taskQueue, __func__,
                                   [track] { return track->GetSamples(64); }));
    if (got.isErr()) {
      break;  // End of stream.
    }
    samples.AppendElements(got.inspect()->GetSamples());
  }
  if (samples.IsEmpty()) {
    ADD_FAILURE() << "No AAC sample demuxed from " << aFileName;
    return false;
  }

  ScopedPrefSetter decoderPrefs({
      {"media.gpu-process-decoder", false},
      {"media.rdd-process.enabled", false},
      {"media.utility-process.enabled", false},
      {"media.prefer-non-ffvpx", false},
  });
  // With the engine's identity declared, AAC must route to the bundled
  // FFmpeg (ffvpx), never to the host's own Media Foundation on Windows
  // (WMFDecoderModule::Supports, STEALTH-HOOK fingerprint/media/decode-samples).
  // zoom.stealth.fpp.hw_seed is an int pref, which ScopedPrefSetter (bool
  // only) cannot hold.
  const int32_t originalHwSeed = Preferences::GetInt("zoom.stealth.fpp.hw_seed", 0);
  Preferences::SetInt("zoom.stealth.fpp.hw_seed", 7);
  auto restoreHwSeed = MakeScopeExit(
      [&] { Preferences::SetInt("zoom.stealth.fpp.hw_seed", originalHwSeed); });
  RefPtr<PDMFactory> factory = MakeRefPtr<PDMFactory>();
  auto created = WaitFor(factory->CreateDecoder(
      CreateDecoderParams{*trackInfo->GetAsAudioInfo(),
                          CreateDecoderParams::WrapperSet({media::Wrapper::AudioTrimmer})}));
  if (created.isErr()) {
    ADD_FAILURE() << "No decoder for the AAC of " << aFileName;
    return false;
  }
  RefPtr<MediaDataDecoder> decoder = created.unwrap();
  auto shutdownDecoder =
      MakeScopeExit([&] { WaitForResolve(decoder->Shutdown()); });
  if (!decoder->GetDescriptionName().EqualsLiteral("ffvpx audio decoder")) {
    ADD_FAILURE() << "AAC went to " << decoder->GetDescriptionName().get()
                  << ", not to the bundled FFmpeg";
    return false;
  }
  if (WaitFor(decoder->Init()).isErr()) {
    ADD_FAILURE() << "Failed to initialize the ffvpx AAC decoder";
    return false;
  }

  aOut.mChannels = trackInfo->GetAsAudioInfo()->mChannels;
  aOut.mFrames = 0;
  aOut.mPcm.Clear();
  auto append = [&](const MediaDataDecoder::DecodedData& aData) {
    for (const auto& d : aData) {
      const AudioData* audio = d->As<AudioData>();
      aOut.mPcm.AppendElements(audio->Data());
      aOut.mFrames += audio->Frames();
    }
  };
  for (const auto& sample : samples) {
    auto decoded = WaitFor(decoder->Decode(sample));
    if (decoded.isErr()) {
      ADD_FAILURE() << "AAC decode error in " << aFileName;
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

// Decodes aFileName in plain C and with all the host's SIMD; false (and the
// test skipped) on a CPU without AVX.
bool DecodeInCAndAvx(const char* aFileName, Decoded& aC, Decoded& aAvx) {
  AvCpu cpu;
  if (!cpu.Load()) {
    ADD_FAILURE() << "Bundled FFmpeg not loadable";
    return false;
  }
  const int host = cpu.Host();
  if (!(host & kAvCpuFlagAvx)) {
    return false;
  }
  cpu.mForce(0);
  const bool c = DecodeWithFFVPX(aFileName, aC);
  cpu.mForce(host);
  const bool avx = DecodeWithFFVPX(aFileName, aAvx);
  return c && avx;
}

bool HostHasAvx() {
  AvCpu cpu;
  return cpu.Load() && (cpu.Host() & kAvCpuFlagAvx);
}

void ExpectMediaFoundationSamples(const char* aM4a, const char* aWav,
                                  uint32_t aExpectedFrames,
                                  uint32_t aPrimingFrames = kMfPrimingFrames) {
  Decoded c, avx;
  ASSERT_TRUE(DecodeInCAndAvx(aM4a, c, avx));
  EXPECT_STREQ(HexSha256(c.mPcm).get(), HexSha256(avx.mPcm).get())
      << aM4a << ": the output depends on the CPU";

  uint32_t wavChannels = 0;
  nsTArray<float> wav;
  ASSERT_TRUE(ReadFloatWav(aWav, &wavChannels, wav)) << aWav;
  ASSERT_EQ(wavChannels, c.mChannels);
  EXPECT_EQ(c.mFrames, aExpectedFrames) << aM4a;
  const size_t skip = aPrimingFrames * wavChannels;
  ASSERT_GE(wav.Length(), skip + c.mPcm.Length()) << aWav << " is too short";

  for (const Decoded* d : {&c, &avx}) {
    double maxAbs = 0;
    size_t equal = 0, first = SIZE_MAX;
    for (size_t i = 0; i < d->mPcm.Length(); i++) {
      const double diff =
          std::fabs(double(d->mPcm[i]) - double(wav[skip + i]));
      if (diff == 0) {
        equal++;
      } else if (first == SIZE_MAX) {
        first = i;
      }
      maxAbs = std::max(maxAbs, diff);
    }
    EXPECT_EQ(maxAbs, 0.0)
        << aM4a << (d == &c ? " (C)" : " (AVX)") << ": " << equal << "/"
        << d->mPcm.Length() << " samples equal to Media Foundation, first "
        << "difference at frame " << (first / wavChannels);
  }
}

}  // namespace

TEST(FFVPXAac, SameBytesInPlainCAndAvx)
{
  if (!HostHasAvx()) {
    GTEST_SKIP() << "This CPU has no AVX";
  }
  Decoded c, avx;
  ASSERT_TRUE(DecodeInCAndAvx("aac-lc-pns.m4a", c, avx));
  EXPECT_EQ(c.mFrames, avx.mFrames);
  EXPECT_STREQ(HexSha256(c.mPcm).get(), HexSha256(avx.mPcm).get())
      << "AAC output depends on the CPU";
}

// aac-lc-pns.m4a: AAC-LC 128 kb/s stereo 44.1 kHz with PNS and intensity
// stereo, 4 s (176400 frames after the edit list).
TEST(FFVPXAac, SameSamplesAsMediaFoundationLc)
{
  if (!HostHasAvx()) {
    GTEST_SKIP() << "This CPU has no AVX";
  }
  ExpectMediaFoundationSamples("aac-lc-pns.m4a", "aac-lc-pns.mf.wav", 176400);
}

// aac-noise.m4a: AAC-LC 96 kb/s stereo 48 kHz, pink noise (PNS), 3 s
// (144000 frames after the edit list).
TEST(FFVPXAac, SameSamplesAsMediaFoundationNoise)
{
  if (!HostHasAvx()) {
    GTEST_SKIP() << "This CPU has no AVX";
  }
  ExpectMediaFoundationSamples("aac-noise.m4a", "aac-noise.mf.wav", 144000);
}

// tns.m4a: FFmpeg 8.0.1 `aac -aac_tns 1`, 128 kb/s stereo 44.1 kHz, clicks +
// tones + noise, 5 s (220500 frames after the edit list). Exercises the TNS
// lattice filter (mf_apply_tns, aacdec_mf.h).
TEST(FFVPXAac, SameSamplesAsMediaFoundationTns)
{
  if (!HostHasAvx()) {
    GTEST_SKIP() << "This CPU has no AVX";
  }
  ExpectMediaFoundationSamples("tns.m4a", "tns.mf.wav", 220500);
}

// tns64.m4a: the same content as tns.m4a at 64 kb/s (more TNS filters per
// frame), 5 s (220500 frames after the edit list).
TEST(FFVPXAac, SameSamplesAsMediaFoundationTns64)
{
  if (!HostHasAvx()) {
    GTEST_SKIP() << "This CPU has no AVX";
  }
  ExpectMediaFoundationSamples("tns64.m4a", "tns64.mf.wav", 220500);
}

// lcfdk.m4a: `fdkaac -p 2 -b 128000` (libfdk-aac 2.0.2), 128 kb/s stereo
// 44.1 kHz, same content as tns.m4a; no edit list (fdkaac writes gapless
// info in an iTunSMPB tag instead), so neither ffvpx's demuxer nor Media
// Foundation trims the encoder's lookahead: every encoded frame (223232)
// comes out on both sides, with no priming to skip in the .mf.wav (unlike
// the edit-list files above). Exercises short windows and the KBD window
// (mf_imdct_and_windowing, aacdec_mf.h).
TEST(FFVPXAac, SameSamplesAsMediaFoundationLcfdk)
{
  if (!HostHasAvx()) {
    GTEST_SKIP() << "This CPU has no AVX";
  }
  ExpectMediaFoundationSamples("lcfdk.m4a", "lcfdk.mf.wav", 223232,
                                /* aPrimingFrames */ 0);
}
