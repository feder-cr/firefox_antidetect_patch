/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// The per-session audio noise (dom/media/webaudio/StealthAudioNoise.*): the
// values a page reads back are unchanged unless the seed is set AND
// zoom.stealth.audio.fp_noise is true, and then move by the same amount for
// the same seed. The reference functions below are the computations as they
// were written inline in AnalyserNode.cpp, AudioDestinationNode.cpp and
// DynamicsCompressorNode.h before they moved; the results are compared by
// bit pattern.

#include <stdint.h>
#include <string.h>

#include <vector>

#include "StealthAudioNoise.h"
#include "gtest/gtest.h"
#include "mozilla/Preferences.h"

namespace mozilla::dom {
// Declared in DynamicsCompressorNode.h; declared here so the test does not
// pull in the audio headers. A signature mismatch fails to link.
float StealthPerturbCompressorReduction(float aReduction);
}  // namespace mozilla::dom

using namespace mozilla;

namespace {

constexpr char kSeedPref[] = "zoom.stealth.fpp.hw_seed";
constexpr char kNoisePref[] = "zoom.stealth.audio.fp_noise";

// Sets both prefs, puts them back at the end of the test.
class AudioNoisePrefs {
 public:
  AudioNoisePrefs(int32_t aSeed, bool aNoise)
      : mOldSeed(Preferences::GetInt(kSeedPref, -1)),
        mOldNoise(Preferences::GetBool(kNoisePref, true)) {
    Preferences::SetInt(kSeedPref, aSeed);
    Preferences::SetBool(kNoisePref, aNoise);
  }
  ~AudioNoisePrefs() {
    Preferences::SetInt(kSeedPref, mOldSeed);
    Preferences::SetBool(kNoisePref, mOldNoise);
  }

 private:
  int32_t mOldSeed;
  bool mOldNoise;
};

uint32_t AudioNoiseBits(float aValue) {
  uint32_t bits;
  memcpy(&bits, &aValue, sizeof(bits));
  return bits;
}

bool AudioNoiseSameBits(const std::vector<float>& aA,
                        const std::vector<float>& aB) {
  return aA.size() == aB.size() &&
         (aA.empty() ||
          memcmp(aA.data(), aB.data(), aA.size() * sizeof(float)) == 0);
}

// A deterministic non-zero ramp.
std::vector<float> AudioNoiseRamp(size_t aLength) {
  std::vector<float> v(aLength);
  for (size_t i = 0; i < aLength; ++i) {
    v[i] = (float(int32_t(i % 64) - 32) + 0.5f) / 64.0f;
  }
  return v;
}

// References: the inline code as it was before the move.
void ReferenceFrequency(int32_t aSeed, std::vector<float>& aData) {
  int32_t seed = aSeed;
  size_t length = aData.size();
  if (seed > 0) {
    float noiseScale = float((uint32_t(seed) % 2001u) + 500u) * 1.0e-4f;
    for (size_t i = 0; i < length; ++i) {
      uint32_t h = uint32_t(seed) ^ uint32_t(i) * 2654435761u;
      h ^= (h >> 16); h *= 0x45d9f3bu; h ^= (h >> 16);
      aData[i] += (float(int32_t(h & 0xFFFFu) - 0x8000) / float(0x8000))
                  * noiseScale;
    }
  }
}

void ReferenceTimeDomain(int32_t aSeed, std::vector<float>& aData) {
  int32_t seed = aSeed;
  size_t length = aData.size();
  if (seed > 0) {
    float noiseScale = float((uint32_t(seed) % 2001u) + 500u) * 1.0e-7f;
    for (size_t i = 0; i < length; ++i) {
      uint32_t h = uint32_t(seed) ^ (uint32_t(i) * 2246822519u);
      h ^= (h >> 16); h *= 0x45d9f3bu; h ^= (h >> 16);
      aData[i] += (float(int32_t(h & 0xFFFFu) - 0x8000) / float(0x8000))
                  * noiseScale;
    }
  }
}

void ReferenceOffline(int32_t aSeed, bool aInputIsNull, uint32_t i,
                      uint32_t inputChannelCount, uint32_t mWriteIndex,
                      uint32_t mLength, uint32_t duration, float* outputData) {
  int32_t seed = aSeed;
  if (seed > 0 && mLength > 200 && !aInputIsNull && i < inputChannelCount) {
    float noiseScale = float((uint32_t(seed) % 2001u) + 500u) * 1.0e-9f;
    for (uint32_t s = 0; s < duration; ++s) {
      if (mWriteIndex + s < 100) continue;
      uint32_t h = uint32_t(seed);
      h ^= i * 2654435761u;
      h ^= (mWriteIndex + s) * 2246822519u;
      h ^= (h >> 16);
      h *= 0x45d9f3bu;
      h ^= (h >> 16);
      outputData[s] +=
          (float(int32_t(h & 0xFFFFu) - 0x8000) / float(0x8000)) * noiseScale;
    }
  }
}

float ReferenceCompressor(int32_t aSeed, float aReduction) {
  int32_t seed = aSeed;
  if (seed > 0) {
    uint32_t h = uint32_t(seed) ^ 0xA5A5A5A5u;
    h ^= (h >> 16); h *= 0x45d9f3bu; h ^= (h >> 16);
    aReduction += (float(int32_t(h & 0xFFFFu) - 0x8000) / float(0x8000))
                  * 1.0e-5f;
  }
  return aReduction;
}

constexpr int32_t kSeeds[] = {1, 7, 2000, 2001, 123456789, INT32_MAX};
constexpr size_t kLengths[] = {0, 1, 100, 1024, 16384};
constexpr float kReductionInputs[] = {0.0f, -12.5f, -0.25f, 3.5f, -60.0f};

}  // namespace

TEST(StealthAudioNoise, AnalyserMatchesReferenceBitForBit)
{
  for (int32_t seed : kSeeds) {
    AudioNoisePrefs prefs(seed, true);
    for (size_t length : kLengths) {
      std::vector<float> freq = AudioNoiseRamp(length);
      std::vector<float> freqRef = freq;
      std::vector<float> time = freq;
      std::vector<float> timeRef = freq;
      dom::StealthAddFrequencyNoise(Span<float>(freq), length);
      ReferenceFrequency(seed, freqRef);
      dom::StealthAddTimeDomainNoise(Span<float>(time), length);
      ReferenceTimeDomain(seed, timeRef);
      EXPECT_TRUE(AudioNoiseSameBits(freq, freqRef))
          << "frequency, seed " << seed << " length " << length;
      EXPECT_TRUE(AudioNoiseSameBits(time, timeRef))
          << "time domain, seed " << seed << " length " << length;
      if (length > 0) {
        // The two paths use different constants: they must not be swapped.
        EXPECT_FALSE(AudioNoiseSameBits(freq, time))
            << "seed " << seed << " length " << length;
        EXPECT_FALSE(AudioNoiseSameBits(freq, AudioNoiseRamp(length)))
            << "seed " << seed << " length " << length;
      }
    }
  }
}

TEST(StealthAudioNoise, AnalyserUnchangedWithoutSeedOrWhenDisabled)
{
  struct Case {
    int32_t mSeed;
    bool mNoise;
  };
  const Case cases[] = {{0, true}, {-1, true}, {123456789, false}};
  for (const Case& c : cases) {
    AudioNoisePrefs prefs(c.mSeed, c.mNoise);
    std::vector<float> freq = AudioNoiseRamp(1024);
    std::vector<float> time = freq;
    dom::StealthAddFrequencyNoise(Span<float>(freq), freq.size());
    dom::StealthAddTimeDomainNoise(Span<float>(time), time.size());
    EXPECT_TRUE(AudioNoiseSameBits(freq, AudioNoiseRamp(1024)))
        << "seed " << c.mSeed;
    EXPECT_TRUE(AudioNoiseSameBits(time, AudioNoiseRamp(1024)))
        << "seed " << c.mSeed;
  }
}

TEST(StealthAudioNoise, OfflineMatchesReferenceBitForBit)
{
  struct Block {
    uint32_t mWriteIndex;
    uint32_t mLength;
    uint32_t mDuration;
    uint32_t mChannel;
    uint32_t mInputChannels;
    bool mNull;
  };
  const Block blocks[] = {
      {0, 44100, 128, 0, 1, false},   // first block: first 100 samples skipped
      {128, 44100, 128, 0, 1, false}, // past the skipped range
      {44032, 44100, 68, 1, 2, false}, // last, short block, second channel
      {0, 100, 100, 0, 1, false},     // length <= 200: nothing
      {0, 200, 128, 0, 1, false},     // length == 200: nothing
      {128, 44100, 128, 0, 1, true},  // null input: nothing
      {128, 44100, 128, 2, 2, false}, // channel beyond the input: nothing
  };
  for (int32_t seed : kSeeds) {
    AudioNoisePrefs prefs(seed, true);
    for (const Block& b : blocks) {
      std::vector<float> out = AudioNoiseRamp(b.mDuration);
      std::vector<float> ref = out;
      dom::StealthAddOfflineAudioNoise(b.mNull, b.mChannel, b.mInputChannels,
                                       b.mWriteIndex, b.mLength, b.mDuration,
                                       out.data());
      ReferenceOffline(seed, b.mNull, b.mChannel, b.mInputChannels,
                       b.mWriteIndex, b.mLength, b.mDuration, ref.data());
      EXPECT_TRUE(AudioNoiseSameBits(out, ref))
          << "seed " << seed << " write index " << b.mWriteIndex << " length "
          << b.mLength << " channel " << b.mChannel;
    }
  }
}

TEST(StealthAudioNoise, OfflineSkipsFirstHundredSamplesAndSilence)
{
  AudioNoisePrefs prefs(12345, true);
  std::vector<float> out = AudioNoiseRamp(128);
  const std::vector<float> in = out;
  dom::StealthAddOfflineAudioNoise(false, 0, 1, 0, 44100, 128, out.data());
  for (size_t s = 0; s < 100; ++s) {
    EXPECT_EQ(AudioNoiseBits(out[s]), AudioNoiseBits(in[s])) << s;
  }
  bool changed = false;
  for (size_t s = 100; s < 128; ++s) {
    changed |= AudioNoiseBits(out[s]) != AudioNoiseBits(in[s]);
  }
  EXPECT_TRUE(changed);

  // A silent block stays silent.
  std::vector<float> silent(128, 0.0f);
  dom::StealthAddOfflineAudioNoise(true, 0, 1, 128, 44100, 128,
                                   silent.data());
  for (float v : silent) {
    EXPECT_EQ(AudioNoiseBits(v), AudioNoiseBits(0.0f));
  }
}

TEST(StealthAudioNoise, OfflineUnchangedWithoutSeedOrWhenDisabled)
{
  struct Case {
    int32_t mSeed;
    bool mNoise;
  };
  const Case cases[] = {{0, true}, {-1, true}, {123456789, false}};
  for (const Case& c : cases) {
    AudioNoisePrefs prefs(c.mSeed, c.mNoise);
    std::vector<float> out = AudioNoiseRamp(128);
    dom::StealthAddOfflineAudioNoise(false, 0, 1, 128, 44100, 128, out.data());
    EXPECT_TRUE(AudioNoiseSameBits(out, AudioNoiseRamp(128)))
        << "seed " << c.mSeed;
  }
}

TEST(StealthAudioNoise, CompressorUnchangedWithoutSeed)
{
  for (int32_t seed : {-1, 0}) {
    AudioNoisePrefs prefs(seed, true);
    for (float in : kReductionInputs) {
      EXPECT_EQ(AudioNoiseBits(dom::StealthPerturbCompressorReduction(in)),
                AudioNoiseBits(in));
    }
  }
}

TEST(StealthAudioNoise, CompressorUnchangedWhenNoiseDisabled)
{
  AudioNoisePrefs prefs(8, false);
  for (float in : kReductionInputs) {
    EXPECT_EQ(AudioNoiseBits(dom::StealthPerturbCompressorReduction(in)),
              AudioNoiseBits(in));
  }
}

TEST(StealthAudioNoise, CompressorMatchesReferenceBitForBit)
{
  for (int32_t seed : {1, 8, 123456789, 2147483647}) {
    AudioNoisePrefs prefs(seed, true);
    for (float in : kReductionInputs) {
      EXPECT_EQ(AudioNoiseBits(dom::StealthPerturbCompressorReduction(in)),
                AudioNoiseBits(ReferenceCompressor(seed, in)))
          << "seed " << seed << " input " << in;
    }
  }
}

// Fixed values for an input of 0 (so no contraction can change them), worked
// out outside the code; they pin the hash itself.
TEST(StealthAudioNoise, CompressorFixedValuesForZeroInput)
{
  struct Case {
    int32_t mSeed;
    uint32_t mBits;
  };
  const Case cases[] = {{1, 0xb6c8e0cau},
                        {8, 0xb6c9691au},
                        {123456789, 0x35f46887u},
                        {2147483647, 0x352f3a15u}};
  for (const Case& c : cases) {
    AudioNoisePrefs prefs(c.mSeed, true);
    EXPECT_EQ(AudioNoiseBits(dom::StealthPerturbCompressorReduction(0.0f)),
              c.mBits)
        << "seed " << c.mSeed;
  }
}

TEST(StealthAudioNoise, CompressorSmallAndDependsOnSeed)
{
  float first;
  {
    AudioNoisePrefs prefs(1, true);
    first = dom::StealthPerturbCompressorReduction(0.0f);
    EXPECT_LE(first, 1.0e-5f);
    EXPECT_GE(first, -1.0e-5f);
    EXPECT_NE(first, 0.0f);
  }
  AudioNoisePrefs prefs(8, true);
  EXPECT_NE(AudioNoiseBits(dom::StealthPerturbCompressorReduction(0.0f)),
            AudioNoiseBits(first));
}
