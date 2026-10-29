/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef GFX_STEALTHFONTFILESTREAM_H
#define GFX_STEALTHFONTFILESTREAM_H

// Our half of gfxDWriteCommon.cpp (fingerprint/fonts/bundle): the stream that
// borrows bytes, and the one create-flow shared by the owning and the borrowed
// entry points. Included only by gfxDWriteCommon.cpp, so it is not exported.

#include <stdint.h>

#include <unordered_map>

#include "gfxDWriteCommon.h"
#include "mozilla/Attributes.h"
#include "mozilla/StaticMutex.h"

// The registry of live font file streams, keyed by the uint64_t that
// DirectWrite hands back to gfxDWriteFontFileLoader::CreateStreamFromKey.
// Mozilla's code in gfxDWriteCommon.cpp (Release, ~gfxDWriteFontFileStream,
// CreateStreamFromKey, SizeOfIncludingThis) and the create-flow in
// StealthFontFileStream.cpp both use it, so it is defined in the one place and
// declared here. Defined in StealthFontFileStream.cpp.
extern mozilla::StaticMutex sFontFileStreamsMutex MOZ_UNANNOTATED;
extern uint64_t sNextFontFileKey;
extern std::unordered_map<uint64_t, gfxDWriteFontFileStream*> sFontFileStreams;

// The body of gfxDWriteFontFileLoader::CreateCustomFontFile (owning stream
// holding aFontData). Same contract as that function.
HRESULT StealthCreateOwningFontFile(FontData* aFontData,
                                    IDWriteFontFile** aFontFile,
                                    gfxDWriteFontFileStream** aFontFileStream);

#endif  // GFX_STEALTHFONTFILESTREAM_H
