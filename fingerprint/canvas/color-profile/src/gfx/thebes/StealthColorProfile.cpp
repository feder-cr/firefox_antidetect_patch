/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// The output colour profile the stealth engine declares (see StealthColorProfile.h). It
// replaces what the host answers (Windows GetICMProfileW, Linux's _ICC_PROFILE and EDID
// atoms) at the point where the platform would read it, in
// gfxWindowsPlatform::GetPlatformCMSOutputProfileData_Impl and
// gfxPlatformGtk::GetPlatformCMSOutputProfileData.

#include "StealthColorProfile.h"

#include "StealthDeclarationGate.h"

namespace mozilla {
namespace gfx {

namespace {

constexpr uint32_t kHeaderSize = 128;
constexpr uint32_t kTagCount = 7;
constexpr uint32_t kTagTableSize = 4 + 12 * kTagCount;
constexpr uint32_t kXYZTagSize = 20;
constexpr uint32_t kCurveTagSize = 12 + 2 * kStealthColorProfileCurvePoints;
constexpr uint32_t kFirstTagOffset = kHeaderSize + kTagTableSize;
constexpr uint32_t kCurveOffset = kFirstTagOffset + 4 * kXYZTagSize;
static_assert(kCurveOffset + kCurveTagSize == kStealthColorProfileSize,
              "the profile's layout");

constexpr uint32_t Sig(char a, char b, char c, char d) {
  return (uint32_t(uint8_t(a)) << 24) | (uint32_t(uint8_t(b)) << 16) |
         (uint32_t(uint8_t(c)) << 8) | uint32_t(uint8_t(d));
}

// The sRGB curve, point i = round(65535 * linear(i / 1023)); constants, because pow() is
// not the same function in every libm.
constexpr uint16_t kCurve[kStealthColorProfileCurvePoints] = {
#include "StealthColorProfileCurve.inc"
};

void Put32(nsTArray<uint8_t>& aOut, uint32_t aValue) {
  aOut.AppendElement(uint8_t(aValue >> 24));
  aOut.AppendElement(uint8_t(aValue >> 16));
  aOut.AppendElement(uint8_t(aValue >> 8));
  aOut.AppendElement(uint8_t(aValue));
}

void Put16(nsTArray<uint8_t>& aOut, uint16_t aValue) {
  aOut.AppendElement(uint8_t(aValue >> 8));
  aOut.AppendElement(uint8_t(aValue));
}

void PutZeros(nsTArray<uint8_t>& aOut, uint32_t aCount) {
  for (uint32_t i = 0; i < aCount; i++) {
    aOut.AppendElement(0);
  }
}

// A tag table entry: signature, offset, size.
void PutTag(nsTArray<uint8_t>& aOut, uint32_t aSig, uint32_t aOffset,
            uint32_t aSize) {
  Put32(aOut, aSig);
  Put32(aOut, aOffset);
  Put32(aOut, aSize);
}

// An `XYZ ` tag: the type, 4 reserved bytes, three s15Fixed16 numbers (raw).
void PutXYZ(nsTArray<uint8_t>& aOut, uint32_t aX, uint32_t aY, uint32_t aZ) {
  Put32(aOut, Sig('X', 'Y', 'Z', ' '));
  Put32(aOut, 0);
  Put32(aOut, aX);
  Put32(aOut, aY);
  Put32(aOut, aZ);
}

}  // namespace

nsTArray<uint8_t> StealthSystemColorProfileData() {
  nsTArray<uint8_t> out;
  out.SetCapacity(kStealthColorProfileSize);

  // The 128 byte header: size, no CMM, version 2.1.0, monitor, RGB, XYZ PCS, no date, 'acsp',
  // platform MSFT, no flags, manufacturer, model, attributes, intent 0 (perceptual), the D50
  // illuminant (0.964203, 1.0, 0.824905), no creator, no profile ID, reserved.
  Put32(out, kStealthColorProfileSize);
  Put32(out, 0);
  Put32(out, 0x02100000);
  Put32(out, Sig('m', 'n', 't', 'r'));
  Put32(out, Sig('R', 'G', 'B', ' '));
  Put32(out, Sig('X', 'Y', 'Z', ' '));
  PutZeros(out, 12);
  Put32(out, Sig('a', 'c', 's', 'p'));
  Put32(out, Sig('M', 'S', 'F', 'T'));
  PutZeros(out, 4 + 4 + 4 + 8);
  Put32(out, 0);
  Put32(out, 0x0000F6D6);
  Put32(out, 0x00010000);
  Put32(out, 0x0000D32D);
  PutZeros(out, kHeaderSize - out.Length());

  // The tag table.
  Put32(out, kTagCount);
  PutTag(out, Sig('w', 't', 'p', 't'), kFirstTagOffset, kXYZTagSize);
  PutTag(out, Sig('r', 'X', 'Y', 'Z'), kFirstTagOffset + kXYZTagSize, kXYZTagSize);
  PutTag(out, Sig('g', 'X', 'Y', 'Z'), kFirstTagOffset + 2 * kXYZTagSize,
         kXYZTagSize);
  PutTag(out, Sig('b', 'X', 'Y', 'Z'), kFirstTagOffset + 3 * kXYZTagSize,
         kXYZTagSize);
  PutTag(out, Sig('r', 'T', 'R', 'C'), kCurveOffset, kCurveTagSize);
  PutTag(out, Sig('g', 'T', 'R', 'C'), kCurveOffset, kCurveTagSize);
  PutTag(out, Sig('b', 'T', 'R', 'C'), kCurveOffset, kCurveTagSize);

  // The tag data: the white point and the colorants of the Windows profile, then the curve.
  PutXYZ(out, 62289, 65536, 71372);
  PutXYZ(out, 28578, 14581, 912);
  PutXYZ(out, 25241, 46981, 6362);
  PutXYZ(out, 9376, 3972, 46799);
  Put32(out, Sig('c', 'u', 'r', 'v'));
  Put32(out, 0);
  Put32(out, kStealthColorProfileCurvePoints);
  for (uint16_t v : kCurve) {
    Put16(out, v);
  }
  return out;
}

bool StealthColorProfileDeclared() { return StealthEngineActive(); }

Maybe<nsTArray<uint8_t>> StealthDeclaredColorProfileData() {
  if (!StealthColorProfileDeclared()) {
    return Nothing();
  }
  return Some(StealthSystemColorProfileData());
}

}  // namespace gfx
}  // namespace mozilla
