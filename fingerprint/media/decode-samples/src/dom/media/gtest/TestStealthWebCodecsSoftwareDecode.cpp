/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// The software-only WebCodecs decode gate (dom/media/webcodecs/DecoderTemplate.*,
// ShouldPreferSoftwareDecoder, zoom.stealth.fpp.hw_seed).
//
// REA (invisible_WebCodecs/notes/nv12-rgb-conversion.md) found that WebCodecs'
// D3D11/DXVA hardware path does its NV12->RGB conversion in the GPU
// (ID3D11VideoContext::VideoProcessorBlt), which depends on the driver: a host
// dependency. With the engine on, the decoder must always run in software
// (libyuv, fixed Firefox code), whatever the page's `hardwareAcceleration`
// asks for in VideoDecoderConfig. With the engine off this is stock Firefox:
// only an explicit "prefer-software" asks for software.

#include "gtest/gtest.h"
#include "mozilla/Preferences.h"
#include "mozilla/dom/DecoderTemplate.h"
#include "mozilla/dom/VideoDecoderBinding.h"

using namespace mozilla;
using mozilla::dom::HardwareAcceleration;
using mozilla::dom::ShouldPreferSoftwareDecoder;

namespace {

// Named distinctly from the other Stealth gtests' own `kSeedPref`: unified
// sources put every file's unnamed namespace into the same translation
// unit's single anonymous namespace, so a same-named constexpr collides.
constexpr char kWebCodecsHwSeedPref[] = "zoom.stealth.fpp.hw_seed";

// Sets the engine's seed pref, puts it back (the default, engine off) at the
// end of the test.
class ScopedHwSeedPref {
 public:
  explicit ScopedHwSeedPref(int32_t aValue) {
    Preferences::SetInt(kWebCodecsHwSeedPref, aValue);
  }
  ~ScopedHwSeedPref() { Preferences::SetInt(kWebCodecsHwSeedPref, -1); }
};

}  // namespace

// Engine off: today's behaviour. Neither "no-preference" nor
// "prefer-hardware" asks for software.
TEST(StealthWebCodecsSoftwareDecode, EngineOffFollowsThePage)
{
  ScopedHwSeedPref off(-1);
  EXPECT_FALSE(
      ShouldPreferSoftwareDecoder(HardwareAcceleration::No_preference));
  EXPECT_FALSE(
      ShouldPreferSoftwareDecoder(HardwareAcceleration::Prefer_hardware));
  EXPECT_TRUE(
      ShouldPreferSoftwareDecoder(HardwareAcceleration::Prefer_software));
}

// Engine on: software always, even when the page prefers hardware or states
// no preference. This is the line that fails on the code before this gate
// existed (the inline `== Prefer_software` check it replaced).
TEST(StealthWebCodecsSoftwareDecode, EngineOnAlwaysPrefersSoftware)
{
  ScopedHwSeedPref on(1);
  EXPECT_TRUE(
      ShouldPreferSoftwareDecoder(HardwareAcceleration::No_preference));
  EXPECT_TRUE(
      ShouldPreferSoftwareDecoder(HardwareAcceleration::Prefer_hardware));
  EXPECT_TRUE(
      ShouldPreferSoftwareDecoder(HardwareAcceleration::Prefer_software));
}
