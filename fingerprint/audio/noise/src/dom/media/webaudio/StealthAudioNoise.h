/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef DOM_MEDIA_WEBAUDIO_STEALTHAUDIONOISE_H_
#define DOM_MEDIA_WEBAUDIO_STEALTHAUDIONOISE_H_

#include <stddef.h>
#include <stdint.h>

#include "mozilla/Span.h"

namespace mozilla::dom {

// Per-session perturbation of the audio values a page can read back, so the
// sums a page can compute over them differ between sessions. Every function
// does nothing unless zoom.stealth.fpp.hw_seed > 0 and
// zoom.stealth.audio.fp_noise is true. They only read prefs and write the
// buffer they are given, so they are safe inside a ProcessData callback and
// on the audio rendering thread (no allocation, no GC).
//
// The compressor's gain reduction is the fourth site; it is declared in
// DynamicsCompressorNode.h, an exported header that cannot include this one,
// and defined in StealthAudioNoise.cpp.

// Noise on getFloatFrequencyData output (decibel values). Adds to the first
// aLength elements of aData in place; aLength must not exceed aData.Length().
void StealthAddFrequencyNoise(const Span<float>& aData, size_t aLength);

// Noise on getFloatTimeDomainData output (samples in [-1, 1]). Adds to the
// first aLength elements of aData in place, silent or not, with no length
// guard; aLength must not exceed aData.Length().
void StealthAddTimeDomainNoise(const Span<float>& aData, size_t aLength);

/**
 * Per-session noise on the audio an OfflineAudioContext renders.
 *
 * Called by OfflineDestinationNodeEngine::ProcessBlock once per output
 * channel, right after the input block has been copied into the result
 * buffer. Safe on the audio rendering thread: both prefs it reads
 * (zoom.stealth.fpp.hw_seed, zoom.stealth.audio.fp_noise) are
 * RelaxedAtomic with a main-thread-independent mirror.
 *
 * @param aInputIsNull        aInput.IsNull() of the block being rendered.
 * @param aChannel            index of the channel being written.
 * @param aInputChannelCount  aInput.ChannelCount() of the block.
 * @param aWriteIndex         frame index of the first sample of this block in
 *                            the whole rendered buffer.
 * @param aLength             total length of the OfflineAudioContext in
 *                            frames.
 * @param aDuration           number of frames in this block.
 * @param aOutput             this block's samples of channel aChannel
 *                            (already offset by aWriteIndex); aDuration
 *                            floats are readable and writable.
 */
void StealthAddOfflineAudioNoise(bool aInputIsNull, uint32_t aChannel,
                                 uint32_t aInputChannelCount,
                                 uint32_t aWriteIndex, uint32_t aLength,
                                 uint32_t aDuration, float* aOutput);

}  // namespace mozilla::dom

#endif  // DOM_MEDIA_WEBAUDIO_STEALTHAUDIONOISE_H_
