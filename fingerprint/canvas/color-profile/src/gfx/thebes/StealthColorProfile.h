/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef MOZILLA_GFX_STEALTHCOLORPROFILE_H_
#define MOZILLA_GFX_STEALTHCOLORPROFILE_H_

#include <stdint.h>

#include "mozilla/Maybe.h"
#include "nsTArray.h"

namespace mozilla {
namespace gfx {

// The ICC profile of the output device (the "display profile" qcms converts tagged images
// to, in gfxPlatform::InitializeCMS) of a Windows machine whose monitor has no profile of
// its own: the system's sRGB profile, "sRGB Color Space Profile.icm". An ICC v2.1 monitor
// RGB/XYZ profile with the colorants and white point of that file, perceptual intent, and
// a 1024 point tone curve (StealthColorProfileCurve.inc, made by
// gen_stealth_color_profile.py), 2356 bytes: wtpt, rXYZ, gXYZ, bXYZ, and one `curv` shared
// by rTRC, gTRC and bTRC (the tags qcms reads for a matrix and curves profile).
//
// qcms treats it as a real profile, not as its own sRGB (qcms_profile_is_sRGB is false), so
// an image tagged sRGB, iCCP, cICP or gAMA+cHRM is converted to it exactly as retail Firefox
// on Windows converts it. Built from constants, in a fixed order: the same bytes on every
// machine, never read from the host.
nsTArray<uint8_t> StealthSystemColorProfileData();

// Whether the output profile is declared: the engine is on (zoom.stealth.fpp.hw_seed > 0).
// With it on, the host's profile is never read, gfx.color_management.display_profile is
// ignored, and gfx.color_management.force_srgb / native_srgb do not replace the profile.
bool StealthColorProfileDeclared();

// The profile the platform answers as the output device's: StealthSystemColorProfileData()
// with the engine on, nothing with it off (the platform then does what stock Firefox does).
Maybe<nsTArray<uint8_t>> StealthDeclaredColorProfileData();

// The size of the profile and its curve, for the gtests.
constexpr uint32_t kStealthColorProfileSize = 2356;
constexpr uint32_t kStealthColorProfileCurvePoints = 1024;

}  // namespace gfx
}  // namespace mozilla

#endif  // MOZILLA_GFX_STEALTHCOLORPROFILE_H_
