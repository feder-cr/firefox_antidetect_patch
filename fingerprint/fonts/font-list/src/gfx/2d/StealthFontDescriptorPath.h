/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef MOZILLA_GFX_STEALTHFONTDESCRIPTORPATH_H_
#define MOZILLA_GFX_STEALTHFONTDESCRIPTORPATH_H_

// Font descriptors in the parent process (Linux).
//
// A recorded font descriptor carries a file path. Stock Firefox checks it with
// FcFontMatch against the host's fontconfig; the process runs on an empty
// configuration with no font in it, so that check never matches and every
// recording with text fails (page.screenshot() with HTML text never answers).
// The path is checked against the one thing that is ours instead: it must
// resolve (symlinks and ".." followed) to a file inside <GRE>/fonts, the
// bundle. Anything else is refused, and there is no fallback to a host font.

#include <stdint.h>

#include <string>

#include "mozilla/AlreadyAddRefed.h"

namespace mozilla::gfx {

class UnscaledFont;

// Always true in this engine: the descriptor is checked against the bundle.
// A function (not a constant) so that the Mozilla code after the hook stays
// reachable for the compiler (-Wunreachable-code).
bool StealthHandlesFontDescriptors();

// True, with the canonical path in aOut, when aData/aLen names an existing
// file whose canonical path lies inside the canonical aBundleDir.
bool StealthBundleDescriptorPath(const uint8_t* aData, uint32_t aLen,
                                 const std::string& aBundleDir,
                                 std::string& aOut);

// The UnscaledFont for a descriptor inside aBundleDir, null otherwise.
//
// aIndex comes from the content process, so it is not trusted: bit 31 set
// (a negative int for FreeType) is refused here. The rest is validated by
// FreeType when the face is first opened (UnscaledFontFreeType::InitFace):
// the low 16 bits are the face of a collection and the high 15 the named
// variable instance, and FT_New_Face fails for either that does not exist.
// A failed open leaves InitFace null and CreateScaledFont answers nullptr, so
// a bad index is a failed recording, never a crash.
already_AddRefed<UnscaledFont> StealthUnscaledFontFromBundleDir(
    const std::string& aBundleDir, const uint8_t* aData, uint32_t aLen,
    uint32_t aIndex);

// The same with the bundle directory of this process (<GRE>/fonts).
already_AddRefed<UnscaledFont> StealthUnscaledFontFromDescriptor(
    const uint8_t* aData, uint32_t aLen, uint32_t aIndex);

}  // namespace mozilla::gfx

#endif
