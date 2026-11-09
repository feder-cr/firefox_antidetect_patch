/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_StealthWmfCodecGate_h
#define mozilla_StealthWmfCodecGate_h

#include "StealthDeclarationGate.h"
#include "WMFUtils.h"

namespace mozilla {

/**
 * Whether WMFDecoderModule must refuse aType so PDMFactory falls through to
 * our own ffvpx port instead.
 *
 * The host's own Media Foundation (msauddecmft.dll for audio, the H.264/HEVC
 * MFTs for video, DXVA or their own software path) varies by Windows build
 * and GPU driver. With the engine's identity declared, every codec WMF would
 * otherwise offer must come from ffvpx, bit-exact to a specific declared
 * Media Foundation, never from whatever the real machine happens to have.
 * Audio: every type. Video: only H.264/HEVC (VP8/VP9/AV1 are unaffected).
 */
inline bool StealthWmfRefusesCodec(const WMFStreamType& aType) {
  if (!gfx::StealthEngineActive()) {
    return false;
  }
  if (!StreamTypeIsVideo(aType)) {
    return true;
  }
  return aType == WMFStreamType::H264 || aType == WMFStreamType::HEVC;
}

}  // namespace mozilla

#endif  // mozilla_StealthWmfCodecGate_h
