/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "StealthAudioNoise.h"

#include <stdint.h>

#include "DynamicsCompressorNode.h"
#include "mozilla/Assertions.h"
#include "mozilla/StaticPrefs_zoom.h"

namespace mozilla::dom {

namespace {

// Gate shared by the two AnalyserNode entry points: the session seed must be
// positive and the zoom.stealth.audio.fp_noise switch must be on. Reads each
// pref once and returns the seed to use.
bool StealthAudioNoiseSeed(int32_t* aSeed) {
  int32_t seed = mozilla::StaticPrefs::zoom_stealth_fpp_hw_seed();
  if (seed > 0 && mozilla::StaticPrefs::zoom_stealth_audio_fp_noise()) {
    *aSeed = seed;
    return true;
  }
  return false;
}

// Integer finalizer of the per-sample hash (xor-shift, multiply, xor-shift),
// the one the AnalyserNode paths use. Each caller keeps its own float
// arithmetic so the results do not depend on how the compiler shares code
// between them.
uint32_t StealthAudioNoiseMix(uint32_t aHash) {
  aHash ^= (aHash >> 16);
  aHash *= 0x45d9f3bu;
  aHash ^= (aHash >> 16);
  return aHash;
}

}  // namespace

// Per-session noise on the frequency data so the sum a page computes over
// getFloatFrequencyData differs between sessions. Amplitude is
// (seed % 2001 + 500) * 1e-4, i.e. 0.05 to 0.25 dB; the per-sample term is a
// Fibonacci-hash of the sample index mixed with the seed. The gate includes
// zoom.stealth.audio.fp_noise: gating on the seed alone once let this path
// keep adding noise while that switch said not to.
//
// Why the switch exists and ships FALSE: reverse engineering on 2026-06-22
// isolated the audio noise as the dominant driver of a commercial
// fingerprinting service's tamper score on Windows, 0.4349 to 0.0564 from
// disabling it alone. Until 2026-08-09 this site (and the time-domain and
// compressor ones) ignored the switch, so the 0.0564 recorded as clean was
// measured with this path still running. Known bug T3 in 70-known-bugs.md,
// closed with the fix it proposed.
void StealthAddFrequencyNoise(const Span<float>& aData, size_t aLength) {
  MOZ_ASSERT(aLength <= aData.Length());
  int32_t seed = 0;
  if (!StealthAudioNoiseSeed(&seed)) {
    return;
  }
  float noiseScale = float((uint32_t(seed) % 2001u) + 500u) * 1.0e-4f;
  for (size_t i = 0; i < aLength; ++i) {
    uint32_t h =
        StealthAudioNoiseMix(uint32_t(seed) ^ (uint32_t(i) * 2654435761u));
    aData[i] += (float(int32_t(h & 0xFFFFu) - 0x8000) / float(0x8000)) *
                noiseScale;
  }
}

// Per-session noise on the time-domain data so the sum a page computes over
// getFloatTimeDomainData differs between sessions. Amplitude is
// (seed % 2001 + 500) * 1e-7. Same gate as the frequency path (the seed alone
// used to be enough here too). This path has none of the guards the offline
// path has (silent channel, small buffer, first 100 samples), and that is
// intended: every element is perturbed, silent or not.
void StealthAddTimeDomainNoise(const Span<float>& aData, size_t aLength) {
  MOZ_ASSERT(aLength <= aData.Length());
  int32_t seed = 0;
  if (!StealthAudioNoiseSeed(&seed)) {
    return;
  }
  float noiseScale = float((uint32_t(seed) % 2001u) + 500u) * 1.0e-7f;
  for (size_t i = 0; i < aLength; ++i) {
    uint32_t h =
        StealthAudioNoiseMix(uint32_t(seed) ^ (uint32_t(i) * 2246822519u));
    aData[i] += (float(int32_t(h & 0xFFFFu) - 0x8000) / float(0x8000)) *
                noiseScale;
  }
}

// Per-session audio noise, derived from zoom.stealth.fpp.hw_seed.
//
// Only the offline path calls this (OfflineDestinationNodeEngine is
// exclusive to OfflineAudioContext). It changes the sums and spectral bins a
// page can compute from the rendered buffer, so the audio fingerprint differs
// from one session to the next.
//
// Constraints that keep the noise from being detectable:
//   1. !aInputIsNull && aChannel < aInputChannelCount: skip silent channels
//      (noise on pure silence is physically impossible on real hardware).
//   2. aLength > 200: skip a check that renders a silent 1-channel, 100-frame
//      OfflineAudioContext and looks for non-zero samples; a perturbed result
//      there would mark the audio stack as fake.
//   3. aWriteIndex + s >= 100: skip the first 100 samples, because a public
//      fingerprinting test page sums the first 100 bins to detect noise.
void StealthAddOfflineAudioNoise(bool aInputIsNull, uint32_t aChannel,
                                 uint32_t aInputChannelCount,
                                 uint32_t aWriteIndex, uint32_t aLength,
                                 uint32_t aDuration, float* aOutput) {
  int32_t seed = mozilla::StaticPrefs::zoom_stealth_fpp_hw_seed();
  if (seed > 0 && mozilla::StaticPrefs::zoom_stealth_audio_fp_noise() &&
      aLength > 200 && !aInputIsNull && aChannel < aInputChannelCount) {
    MOZ_ASSERT(aOutput);
    // Scale: (seed % 2001 + 500) * 1e-9, i.e. 5e-7 to 2.5e-6 depending on the
    // seed. Inaudible, but enough to change float sums.
    float noiseScale = float((uint32_t(seed) % 2001u) + 500u) * 1.0e-9f;
    for (uint32_t s = 0; s < aDuration; ++s) {
      // Skip the first 100 samples (the audio noise detection check).
      if (aWriteIndex + s < 100) continue;
      // Deterministic XOR-hash of (seed, channel, sample_position).
      uint32_t h = uint32_t(seed);
      h ^= aChannel * 2654435761u;
      h ^= (aWriteIndex + s) * 2246822519u;
      h ^= (h >> 16);
      h *= 0x45d9f3bu;
      h ^= (h >> 16);
      aOutput[s] += (float(int32_t(h & 0xFFFFu) - 0x8000) / float(0x8000)) *
                    noiseScale;
    }
  }
}

// Per-session noise on the compressor's reported gain reduction, so
// compressorGainReduction varies between sessions: one session always sees
// the same value, different sessions see different ones. The result is within
// +-1e-5 of the input. Gated on the seed AND zoom.stealth.audio.fp_noise;
// gating on the seed alone once ignored fp_noise, so turning the noise off
// had no effect here. Called from DynamicsCompressorNode::SetReduction (main
// thread); returns aReduction unchanged when the noise is off. Private to
// this site: the seed mix (seed ^ 0xA5A5A5A5u) differs from the AnalyserNode
// ones, only the final avalanche step is the same.
float StealthPerturbCompressorReduction(float aReduction) {
  int32_t seed = mozilla::StaticPrefs::zoom_stealth_fpp_hw_seed();
  if (seed > 0 && mozilla::StaticPrefs::zoom_stealth_audio_fp_noise()) {
    uint32_t h = uint32_t(seed) ^ 0xA5A5A5A5u;
    h ^= (h >> 16);
    h *= 0x45d9f3bu;
    h ^= (h >> 16);
    aReduction += (float(int32_t(h & 0xFFFFu) - 0x8000) / float(0x8000)) *
                  1.0e-5f;
  }
  return aReduction;
}

}  // namespace mozilla::dom
